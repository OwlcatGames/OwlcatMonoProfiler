#include "worker_thread.h"
#include "mono_functions.h"
#include "mono_profiler.h"
#include "logger.h"

#include <thread>
#include <string>
#include <execution>
#include <cassert>
#include <mono/metadata/object.h>

using namespace owlcat::mono_functions;

volatile size_t map_size = 0;

namespace owlcat
{
	/*
		A callback for Mono's stack-walking function. Does nothing, but stores method pointer for now
	*/
	mono_bool worker_thread::stack_backtrace::add_trace(MonoMethod* method, int32_t native_offset, int32_t il_offset, mono_bool managed)
	{
		backtrace.push_back(method);

		return 0;
	}

	worker_thread::worker_thread(events_sink* sink)
		: m_events_sink(sink)
		, m_work_items_token(m_work_items)
	{
		//TODO: Allow to specify stopwords externally
		m_stopwords.push_back("UberConsole");
		m_stopwords.push_back("FPSCounter");
		m_stopwords.push_back("CullStateChanged");
		m_stopwords.push_back("IMGUI");

		//m_alloc_loc = fopen("allocs.log", "w");
	}

	worker_thread::~worker_thread()
	{
		stop();
	}

	// Gets full class name, including namespace, into the specified buffer. Will not overflow the buffer.
	void get_full_class_name(char *buffer, size_t buffer_size, MonoClass* klass)
	{
		const char* namespace_name = get_class_namespace(klass);
		if (namespace_name == nullptr)
			namespace_name = "<global>";
		const char* class_name = get_class_name(klass);
		snprintf(buffer, buffer_size - 1, "%s.%s", namespace_name, class_name);
	}

	// This is called by find_references. The object in question may no longer be allocated in reality, so guard with SEH
	void get_full_class_name(char* buffer, size_t buffer_size, uint64_t address)
	{
		__try
		{
			auto klass = object_get_class((MonoObject*)address);
			get_full_class_name(buffer, buffer_size, klass);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	/*
		Main processing function. Dequeues events from queue, updates set of live allocations, and reports events to client
	*/
	void worker_thread::do_work()
	{
		while (!m_stop)
		{
			m_work_items_empty = false;
			// If GC is in progress, block.
			std::scoped_lock gc_lock(m_gc_mutex);

			// Try to dequeue a work item
			work_item item;
			if (!m_work_items.try_dequeue(item))
			{
				m_work_items_empty = true;
				std::this_thread::yield();
				continue;
			}

			// Report free event to client
			if (item.type == work_item_type::free)
			{
				m_events_sink->report_free(item.frame, (uint64_t)item.obj, item.size);
				m_freed += item.size;
				continue;
			}

			// 1. ---------- Get full class name

			static char full_name[2048];
			get_full_class_name(full_name, sizeof(full_name), item.klass);
			

			// 2. ---------- Get function names for callstack and check stop-list

			static std::string backtrace_str;
			backtrace_str.reserve(1024 * 10);
			backtrace_str.clear();

			bool stopword_met = false;
			for (auto& bt_item : item.backtrace.backtrace)
			{
				auto klass = method_get_class(bt_item);

				static char method_name[2048];
				snprintf(method_name, sizeof(method_name)-1, "%s.%s\n", get_class_name(klass), get_method_name(bt_item));
				for (auto s : m_stopwords)
				{
					if (strstr(method_name, s) != nullptr)
					{
						stopword_met = true;
						break;
					}
				}

				if (stopword_met)
					break;

				backtrace_str.append(method_name);
			}

			if (stopword_met)
				continue;

			// This mostly means objects allocated directly from native Unity code, like scene objects
			if (item.backtrace.backtrace.empty())
				backtrace_str = "<no stack>";

			auto addr = (uint64_t)item.obj;
			auto addr_iter = m_allocations.find(addr);
			if (addr_iter == m_allocations.end())
			{
#ifdef DEBUG_ALLOCS
				m_allocations.insert(std::make_pair(addr, alloc_info{ item.size, false, std::string(get_class_name(object_get_class(item.obj)))}));
#else
				m_allocations.insert(std::make_pair(addr, alloc_info{ item.size, false }));
#endif
			}
			else // reallocation
			{
#ifdef DEBUG_ALLOCS
				auto new_name = std::string(get_class_name(object_get_class(item.obj)));
				if (new_name != addr_iter->second.original_class)
					assert("Reallocation with a different class?!");
				addr_iter->second.reallocated = true;
#endif

				m_events_sink->report_free(item.frame, addr, addr_iter->second.size);
				m_freed += addr_iter->second.size;
				auto& alloc = alloc_value(addr_iter);
				alloc.size = item.size;
			}

			m_events_sink->report_alloc(item.frame, addr, item.size, full_name, backtrace_str.c_str());
			m_allocated += item.size;
		}

		// Clear queue when thread stops
		m_work_items.swap(moodycamel::ConcurrentQueue<work_item>());
		m_allocations.clear();
	}

	void worker_thread::start()
	{
		m_thread = std::thread(&worker_thread::do_work, this);
	}

	void worker_thread::stop()
	{
		// Unpause the app if it was paused
		if (m_stop_lock.owns_lock())
			m_stop_lock.unlock();

		m_stop = true;
		if (m_thread.joinable())
			m_thread.join();
	}

	void worker_thread::add_allocation_async(uint64_t frame, MonoClass* klass, MonoObject* obj)
	{
		std::shared_lock stop_lock(m_stop_mutex);
		//fprintf(m_alloc_loc, "%p\n", obj);
		//fflush(m_alloc_loc);

#ifdef DEBUG_ALLOCS
		std::scoped_lock lock(m_gc_mutex);
		auto addr_iter = m_allocations.find((uint64_t)obj);
		if (addr_iter != m_allocations.end())
		{
			auto new_name = std::string(get_class_name(object_get_class(obj)));
			if (new_name != addr_iter->second.original_class)
				printf("Reallocation with a different class?!");
			addr_iter->second.reallocated = true;
		}
#endif		

		work_item item;
		item.frame = frame;
		item.klass = klass;
		item.obj = obj;
		item.size = mono_functions::object_get_size(obj);
		item.type = work_item_type::alloc;

		// This is a heavy call, but it can only be done here, for obvious reasons.
		// We ease things up a bit by only collecting addresses here. do_work translates them into strings in another thread.
		mono_functions::stack_walk(
			[](MonoMethod* method, int32_t native_offset, int32_t il_offset, mono_bool managed, void* data) -> mono_bool
			{
				return ((stack_backtrace*)data)->add_trace(method, native_offset, il_offset, managed);
			}, (void*)&item.backtrace);

		m_work_items.enqueue(m_work_items_token, item);
	}

	// It is possible that the memory pointed to by addr is no longer accessible to us.
	// We can't know if it is, so we use SEH to handle the resulting access violation.
	// Actually, we probably can call into BoehmGC itself to check, but this is a more complicated and less stable way
	// This approach, however, can hide some errors
	intptr_t get_ptr_safe(const uint8_t* p)
	{
		__try
		{
			return *((intptr_t*)p);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// TODO: Add log?
		}

		return 0;
	}

	int worker_thread::do_gc_internal(uint64_t frame, bool only_update_parents)
	{
		int iterations = 0;

		std::scoped_lock gc_lock(m_gc_mutex);
		std::scoped_lock roots_lock(m_roots_mutex);

		m_stack.reserve(1024 * 1024);
		m_stack.clear();

		// Clear all objects' marks
		for (auto iter = m_allocations.begin(); iter != m_allocations.end(); ++iter)
		{
			auto& alloc = alloc_value(iter);
			alloc.reset_flag(alloc_info::flag::TMP_ALLOCATED);
			alloc.reset_flag(alloc_info::flag::IS_ROOT);
			alloc.reset_flag(alloc_info::flag::TMP_VISITED);
			alloc.parents.clear();
#ifdef DEBUG_ALLOCS
			iter.second.parent = nullptr;
#endif
		}

		// 1. Push roots onto stack
		for (auto& r : m_roots)
		{
			uintptr_t* p = (uintptr_t*)r.start;
			while (p < (uintptr_t*)r.start + r.size / sizeof(uintptr_t))
			{
				uintptr_t ref = *p;

				if (ref != 0)
				{
					auto iter = m_allocations.find(ref);
					if (iter != m_allocations.end() && !iter->second.flag(alloc_info::flag::TMP_ALLOCATED))
					{
						alloc_value(iter).set_flag(alloc_info::flag::IS_ROOT);
						alloc_value(iter).set_flag(alloc_info::flag::TMP_ALLOCATED);
						m_stack.push_back({ alloc_key(iter), &alloc_value(iter) });
					}
				}

				++p;
			}
		}

		// 2. Process stack
		while (!m_stack.empty())
		{
			++iterations;
			//auto obj_iter = m_stack.back();
			//m_stack.pop_back();

			//const uint8_t* p = (const uint8_t*)obj_iter->first;
			//const uint8_t* e = (const uint8_t*)obj_iter->first + obj_iter->second.size;

			auto entry = m_stack.back();
			m_stack.pop_back();

			const uint8_t* p = (const uint8_t*)entry.addr;
			const uint8_t* e = (const uint8_t*)entry.addr + entry.info->size;

			while (p + sizeof(intptr_t) <= e)
			{				
				intptr_t candidate = get_ptr_safe(p);
				auto iter = m_allocations.find(candidate);
				if (iter != m_allocations.end())
				{
					auto& alloc = alloc_value(iter);
					alloc.parents.push_back(entry.addr);
					if (!iter->second.flag(alloc_info::flag::TMP_ALLOCATED))
					{
						alloc.reset_flag(alloc_info::flag::IS_ROOT);
						alloc.set_flag(alloc_info::flag::TMP_ALLOCATED);
#ifdef DEBUG_ALLOCS
						iter->second.parent = entry.info;
#endif
						m_stack.push_back({ alloc_key(iter), &alloc });
						//m_stack.push_back(iter);
					}
				}
				++p;
			}
		}

		// If only parents update was requeste, do not remove unmarked objects
		if (!only_update_parents)
		{
			// 3. Forget all unmarked objects
			for (auto iter = m_allocations.begin(); iter != m_allocations.end(); )
			{
				if (!iter->second.flag(alloc_info::flag::TMP_ALLOCATED))
				{
					work_item item;
					item.type = work_item_type::free;
					item.frame = frame;
					item.klass = nullptr;
					item.obj = (MonoObject*)iter->first;
					item.size = iter->second.size;
					m_work_items.enqueue(m_work_items_token, item);

					//auto iter2 = iter;
					//iter = ++iter;
					//m_allocations.erase(iter2);
					iter = m_allocations.erase(iter);
				}
				else
					++iter;
			}
		}

		return iterations;
	}

	/*
		Experimental pseudo-GC function using Unity's own code.
		Doesn't work.
	*/
	void worker_thread::do_gc_unity(uint64_t frame)
	{
		// Clear all objects' marks
		for (auto iter : m_allocations)
		{
			iter.second.reset_flag(alloc_info::flag::TMP_ALLOCATED);
		}

		auto state = begin_liveness_calculation(nullptr, 1024 * 1024, [](void* arr, int size, void* callback_userdata)
			{
				MonoObject** objs = (MonoObject**)arr;
				std::unordered_map<uint64_t, alloc_info>& allocations = *(std::unordered_map<uint64_t, alloc_info>*)callback_userdata;
				for (int i = 0; i < size; ++i)
				{
					auto obj = objs[i];
					auto iter = allocations.find((uint64_t)obj);
					if (iter != allocations.end())
						iter->second.set_flag(alloc_info::flag::TMP_ALLOCATED);
				}
			}, & m_allocations, []() {}, []() {});
		calculate_liveness_from_statics(state);
		end_liveness_calculation(state);

		for (auto iter = m_allocations.begin(); iter != m_allocations.end(); )
		{
			if (!iter->second.flag(alloc_info::flag::TMP_ALLOCATED))
			{
				m_events_sink->report_free(frame, iter->first, iter->second.size);				
				auto iter2 = iter;
				iter = ++iter;
				m_allocations.erase(iter2);
			}
			else
				++iter;
		}
	}

	int worker_thread::do_gc_sync(uint64_t frame, bool only_update_parents)
	{
		// Wait for all previous allocations to be processed to keep the order of events
		while (!m_work_items_empty)
			std::this_thread::yield();		
		
		int r = do_gc_internal(frame, only_update_parents);

		m_last_gc_frame = frame;
		return r;
	}

	void worker_thread::register_root(const char* start, uint64_t size)
	{
		std::scoped_lock roots_lock(m_roots_mutex);
		m_roots.push_back({ start, size });
	}

	void worker_thread::unregister_root(const char* start)
	{
		std::scoped_lock roots_lock(m_roots_mutex);
		auto new_end = std::remove_if(m_roots.begin(), m_roots.end(), [&](auto& r) {return r.start == start; });
		m_roots.erase(new_end, m_roots.end());
	}

	struct references_stack_entry_t
	{
		uint64_t addr;
		object_references_t* info;
	};

	void worker_thread::find_references_internal(uint64_t request_id, const std::vector<uint64_t>& addresses)
	{
		std::vector<object_references_t> filtered_results;

		// Stack of addresses to process
		std::vector<uint64_t> interesting_addresses = addresses;

		while (!interesting_addresses.empty())
		{
			auto addr = interesting_addresses.back();
			interesting_addresses.pop_back();

			auto iter = m_allocations.find(addr);
			if (iter != m_allocations.end())
			{
				static char full_name[2048];
				get_full_class_name(full_name, sizeof(full_name), iter->first);

				filtered_results.push_back({ iter->first, {} });
				filtered_results.back().type = full_name;
				if (iter->second.flag(alloc_info::flag::IS_ROOT))
					filtered_results.back().type += " (Root)";
				if (!iter->second.flag(alloc_info::flag::TMP_ALLOCATED))
					filtered_results.back().type += " (Deleted)";
				filtered_results.back().parents.resize(iter->second.parents.size());
				memcpy(filtered_results.back().parents.data(), iter->second.parents.data(), sizeof(uint64_t) * iter->second.parents.size());

				// Push all object's parents onto stack
				for (auto& parent : iter->second.parents)
				{
					// Check if we already have information about this object. It would be faster to mark the object somehow, but we don't want to spare the memory
					auto check_iter = m_allocations.find(parent);
					if (check_iter != m_allocations.end() && alloc_value(check_iter).flag(alloc_info::flag::TMP_VISITED))
						continue;

					alloc_value(check_iter).set_flag(alloc_info::flag::TMP_VISITED);

					interesting_addresses.push_back(parent);
				}
			}
		}

		m_events_sink->report_references(request_id, filtered_results);
	}

	void worker_thread::find_references(uint64_t request_id, const std::vector<uint64_t>& addresses, uint64_t frame)
	{
		// List of references/parents is only updated during a call to GC. If the last time the GC was called
		// is not in current frame, we need to update list of parents for all objects. But we set the second argument
		// to true to avoid actually removing any objects and reporting free events to client
		if (frame > m_last_gc_frame)
			do_gc_sync(frame, true);

		// When the app is paused, we don't need any locks
		if (m_stop_lock.owns_lock())
		{
			find_references_internal(request_id, addresses);
		}
		// When the app is not paused, we need to hold locks, or the list of allocations can change during find_references_internal call
		else
		{
			std::scoped_lock gc_lock(m_gc_mutex);
			std::scoped_lock roots_lock(m_roots_mutex);
			find_references_internal(request_id, addresses);
		}
	}

	/*
		"Pauses" the profiled app. Actually, all it does is to hold a hard lock on m_stop_mutex.
		The same mutex is locked by all allocations attempt, which means that all threads that
		attempt a managed allocation will be blocked. However, purely native threads WILL CONTINUE TO RUN!
		We have no sure way to stop all threads while avoiding stopping profiler threads, so this is the
		best we can do. Seems good enough for most uses.
	*/
	void worker_thread::pause_app(uint64_t request_id)
	{
		if (!m_stop_lock.owns_lock())
		{
			m_stop_lock = std::unique_lock<std::shared_mutex>(m_stop_mutex);
		}

		m_events_sink->report_paused(request_id, true);
	}
	
	/*
		Unpauses the profiled app
	*/
	void worker_thread::resume_app(uint64_t request_id)
	{
		if (m_stop_lock.owns_lock())
		{
			m_stop_lock.unlock();
			m_stop_lock.release();
		}

		m_events_sink->report_resumed(request_id, true);
	}

	bool worker_thread::is_paused() const
	{
		return m_stop_lock.owns_lock();
	}
}
