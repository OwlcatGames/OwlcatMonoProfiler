#include "worker_thread.h"
#include "mono_functions.h"
#include "mono_profiler.h"
#include "logger.h"

#include <thread>
#include <string>
#include <cstring>
#include <execution>
#include <cassert>
#include <type_traits>
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
		if (count >= MAX_DEPTH)
		{
			overflow = true;
			// Stop the walk: the frames would be dropped anyway. Only Mono honors the
			// return value; IL2CPP's walk can't be interrupted, so there we just drop frames.
			return 1;
		}

		frames[count++] = method;

		return 0;
	}

	worker_thread::worker_thread(events_sink* sink, logger* log, bool capture_raw_ips, bool jit_available)
		: m_events_sink(sink)
		, m_work_items_token(m_work_items)
		, m_logger(log)
		, m_capture_raw_ips(capture_raw_ips)
		, m_jit_available(jit_available)
	{
		// The whole point of the fixed-size backtrace buffer is to keep work_item
		// trivially copyable, so that enqueueing it never touches the heap
		static_assert(std::is_trivially_copyable<work_item>::value, "work_item must stay trivially copyable");

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
#ifdef WIN32
		__try
#endif
		{
			auto klass = object_get_class((MonoObject*)address);
			get_full_class_name(buffer, buffer_size, klass);
		}
#ifdef WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
#endif
	}

	const worker_thread::method_entry& worker_thread::resolve_method(MonoMethod* method)
	{
		auto iter = m_method_cache.find(method);
		if (iter != m_method_cache.end())
			return iter->second;

		auto klass = method_get_class(method);

		char method_name[2048];
		snprintf(method_name, sizeof(method_name) - 1, "%s.%s\n", get_class_name(klass), get_method_name(method));

		method_entry entry;
		entry.text = method_name;
		entry.stopword = false;
		for (auto s : m_stopwords)
		{
			if (strstr(method_name, s) != nullptr)
			{
				entry.stopword = true;
				break;
			}
		}

		return m_method_cache.emplace(method, std::move(entry)).first->second;
	}

	uint32_t worker_thread::intern_type(MonoClass* klass)
	{
		auto iter = m_type_ids.find(klass);
		if (iter != m_type_ids.end())
			return iter->second;

		char full_name[2048];
		get_full_class_name(full_name, sizeof(full_name), klass);

		uint32_t id = m_next_type_id++;
		m_type_ids.emplace(klass, id);

		// The definition must reach the client before any allocation that references it
		m_events_sink->report_type(id, full_name);

		return id;
	}

	uint32_t worker_thread::intern_native_type(uint32_t label_index)
	{
		if (label_index >= m_native_type_ids.size())
			return 0;

		if (m_native_type_ids[label_index] < 0)
		{
			uint32_t id = m_next_type_id++;
			m_native_type_ids[label_index] = (int64_t)id;
			// Reported on the worker thread, like intern_type, so the sink's definition
			// bookkeeping stays single-threaded
			m_events_sink->report_type(id, m_native_type_labels[label_index].c_str());
		}

		return (uint32_t)m_native_type_ids[label_index];
	}

#if defined(WIN32)
	/*
		Captures raw instruction pointers of the current callstack.

		RtlCaptureStackBackTrace can't be used here: for speed and lock-safety it only
		consults the static unwind tables of loaded modules, so it stops at the first
		jit-compiled (managed) frame. Instead, we unwind manually:
		- frames that have unwind info (native module code, and jit code if the runtime
		  registered dynamic function tables) are unwound with RtlVirtualUnwind;
		- jit frames without unwind info are unwound by following the RBP chain - Mono's
		  JIT emits standard "push rbp; mov rbp, rsp" frames on Windows x64. If a frame
		  omits the frame pointer, the chain skips it (or stops at the sanity checks).
	*/
	static void capture_stack_impl(void** frames, uint32_t max_depth, uint32_t* count)
	{
		CONTEXT ctx;
		RtlCaptureContext(&ctx);

		// Stack bounds of the current thread, for validating the RBP chain
		NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
		uint64_t stack_base = (uint64_t)tib->StackBase;

		while (*count < max_depth)
		{
			if (ctx.Rip == 0)
				break;

			frames[(*count)++] = (void*)ctx.Rip;

			uint64_t image_base = 0;
			RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
			if (function != nullptr)
			{
				// A frame with proper unwind info
				void* handler_data = nullptr;
				uint64_t establisher_frame = 0;
				uint64_t prev_rsp = ctx.Rsp;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx, &handler_data, &establisher_frame, nullptr);

				// The stack must strictly grow upwards while unwinding
				if (ctx.Rsp <= prev_rsp)
					break;
			}
			else
			{
				// No unwind info: a jit-compiled managed frame or a trampoline.
				// Follow the RBP chain: [rbp] = caller's rbp, [rbp+8] = return address.
				uint64_t rbp = ctx.Rbp;
				if (rbp < ctx.Rsp || rbp + 16 > stack_base || (rbp & 7) != 0)
					break;

				ctx.Rip = *(uint64_t*)(rbp + 8);
				ctx.Rsp = rbp + 16;
				ctx.Rbp = *(uint64_t*)rbp;
			}
		}
	}

	static uint32_t capture_stack(void** frames, uint32_t max_depth)
	{
		uint32_t count = 0;

		// The RBP chain can contain garbage (e.g. if a jit frame omitted the frame
		// pointer after all): if a read faults, just keep the frames collected so far
		__try
		{
			capture_stack_impl(frames, max_depth, &count);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		return count;
	}

	// Returns the module of the profiler DLL itself
	static HMODULE own_module()
	{
		static HMODULE module = []()
		{
			HMODULE m = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&own_module, &m);
			return m;
		}();
		return module;
	}

#if OWLCAT_MONO
	static HMODULE mono_module()
	{
		static HMODULE module = GetModuleHandleA("mono-2.0-bdwgc.dll");
		return module;
	}

	// The domain pointer can go stale if a domain is reloaded, so guard the lookup with SEH,
	// like other places that touch memory we don't control
	static void* jit_info_table_find_safe(void* domain, void* ip)
	{
		__try
		{
			return mono_functions::jit_info_table_find(domain, ip);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		return nullptr;
	}
#endif

	const worker_thread::ip_entry& worker_thread::resolve_ip(void* ip)
	{
		auto iter = m_ip_cache.find(ip);
		if (iter != m_ip_cache.end())
			return iter->second;

		ip_entry entry;

#if OWLCAT_MONO
		// Is this a managed frame? Try the domain captured from the game's threads first,
		// then the root domain. (Mono only - IL2CPP managed code is AOT-compiled native
		// code, so it resolves through the module+offset path below. Also skipped when the
		// jit functions weren't resolved, e.g. native-only capture.)
		if (m_jit_available)
		{
		void* domain = m_jit_domain.load(std::memory_order_relaxed);
		void* jit_info = domain != nullptr ? jit_info_table_find_safe(domain, ip) : nullptr;
		if (jit_info == nullptr)
		{
			void* root_domain = mono_functions::get_root_domain();
			if (root_domain != nullptr && root_domain != domain)
				jit_info = jit_info_table_find_safe(root_domain, ip);
		}

		if (jit_info != nullptr)
		{
			MonoMethod* method = mono_functions::jit_info_get_method(jit_info);
			if (method != nullptr)
			{
				const method_entry& resolved = resolve_method(method);
				entry.text = resolved.text;
				entry.stopword = resolved.stopword;
				entry.managed = true;
				return m_ip_cache.emplace(ip, std::move(entry)).first->second;
			}
		}
		} // if (m_jit_available)
#endif

		// A native frame: report it as module+offset. This also identifies the frames
		// belonging to the profiler itself and to the Mono runtime (the allocation
		// machinery), which are skipped from the top of callstacks.
		HMODULE module = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &module) && module != nullptr)
		{
			char path[MAX_PATH];
			path[0] = 0;
			GetModuleFileNameA(module, path, sizeof(path));

			const char* basename = strrchr(path, '\\');
			basename = basename != nullptr ? basename + 1 : path;

			char line[512];
			snprintf(line, sizeof(line) - 1, "%s+0x%llX\n", basename, (unsigned long long)((char*)ip - (char*)module));
			entry.text = line;
			entry.runtime_internal = module == own_module();
#if OWLCAT_MONO
			if (module == mono_module())
				entry.runtime_internal = true;
#endif
		}
		else
		{
			// Not inside any module and not jit code: probably a trampoline
			entry.text = "<unknown>\n";
		}

		return m_ip_cache.emplace(ip, std::move(entry)).first->second;
	}
#endif

	worker_thread::callstack_entry worker_thread::intern_callstack(const stack_backtrace& backtrace)
	{
		// FNV-1a over the raw pointer values
		uint64_t hash = 14695981039346656037ULL;
		for (uint32_t i = 0; i < backtrace.count; ++i)
		{
			hash ^= (uint64_t)backtrace.frames[i];
			hash *= 1099511628211ULL;
		}

		auto& bucket = m_callstack_ids[hash];
		for (auto& interned : bucket)
		{
			if (interned.frames.size() == backtrace.count &&
				(backtrace.count == 0 || memcmp(interned.frames.data(), backtrace.frames, backtrace.count * sizeof(void*)) == 0))
				return interned.entry;
		}

		// First time we see this callstack: resolve the frames (cached by pointer)
		// and build the full text
		callstack_entry entry{ 0, false };

		std::string backtrace_str;
		backtrace_str.reserve(4096);

#if defined(WIN32)
		if (m_capture_raw_ips)
		{
			// Frames are raw instruction pointers: each resolves to either a managed method,
			// or a native module+offset line, so the resulting callstack is a mix of managed
			// and native frames. Profiler and Mono frames at the top of the stack (the
			// allocation machinery itself) are skipped.
			bool seen_real_frame = false;
			bool any_managed = false;
			for (uint32_t i = 0; i < backtrace.count; ++i)
			{
				const ip_entry& resolved = resolve_ip(backtrace.frames[i]);
				if (!seen_real_frame && resolved.runtime_internal)
					continue;
				seen_real_frame = true;

				if (resolved.stopword)
				{
					entry.stopword = true;
					break;
				}

				if (resolved.managed)
					any_managed = true;
				backtrace_str.append(resolved.text);
			}

			// If native unwinding can't walk through jit code on this version of Mono,
			// callstacks will contain no managed frames at all, and the average depth
			// will be very low. Log statistics periodically, so that this is easy to
			// diagnose (see also OWLCAT_PROFILER_MONO_WALK).
			++m_unique_ip_stacks;
			m_unique_ip_frames += backtrace.count;
			if (any_managed)
				++m_unique_ip_stacks_with_managed;
			if (m_logger != nullptr && (m_unique_ip_stacks % 1024) == 0)
			{
				char tmp[256];
				snprintf(tmp, sizeof(tmp) - 1, "IP capture: %llu of %llu unique callstacks contain managed frames, %.1f frames on average",
					(unsigned long long)m_unique_ip_stacks_with_managed, (unsigned long long)m_unique_ip_stacks,
					(double)m_unique_ip_frames / (double)m_unique_ip_stacks);
				m_logger->log_str(tmp);
			}
		}
		else
#endif
		{
			for (uint32_t i = 0; i < backtrace.count; ++i)
			{
				const method_entry& resolved = resolve_method((MonoMethod*)backtrace.frames[i]);
				if (resolved.stopword)
				{
					entry.stopword = true;
					break;
				}

				backtrace_str.append(resolved.text);
			}
		}

		if (!entry.stopword)
		{
			// This mostly means objects allocated directly from native Unity code, like scene objects
			if (backtrace_str.empty())
				backtrace_str = "<no stack>";

			entry.id = m_next_callstack_id++;

			// The definition must reach the client before any allocation that references it
			m_events_sink->report_callstack(entry.id, backtrace_str.c_str());
		}

		bucket.push_back({ std::vector<void*>(backtrace.frames, backtrace.frames + backtrace.count), entry });

		return entry;
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

			// Items from different game threads have no global ordering between them,
			// so an item's frame can be slightly older than one we've already processed.
			// Clamp it: the client requires monotonic frame numbers.
			if (item.frame < m_max_seen_frame)
				item.frame = m_max_seen_frame;
			else
				m_max_seen_frame = item.frame;

			// Warn (once) if a callstack was truncated
			if (item.backtrace.overflow && !m_overflow_logged)
			{
				m_overflow_logged = true;
				if (m_logger)
				{
					char tmp[256];
					snprintf(tmp, sizeof(tmp) - 1, "A callstack was deeper than %u frames and was truncated. This is only logged once.", (unsigned)stack_backtrace::MAX_DEPTH);
					m_logger->log_str(tmp);
				}
			}

			// ---------- Native events. These bypass m_allocations (and thus the pseudo-GC):
			// native memory is freed explicitly, not collected.
			if (item.type == work_item_type::native_free)
			{
				// Recover the freed block's size, which free(ptr) doesn't carry
				uint64_t naddr = (uint64_t)item.obj;
				auto it = m_native_allocations.find(naddr);
				if (it != m_native_allocations.end())
				{
					m_events_sink->report_free(item.frame, naddr, it->second);
					m_freed += it->second;
					m_native_allocations.erase(it);
				}
				// A free of an untracked address (allocated before hooks were installed) is ignored
				continue;
			}

			if (item.type == work_item_type::native_alloc)
			{
				callstack_entry callstack = intern_callstack(item.backtrace);
				if (callstack.stopword)
					continue;

				uint32_t type_id = intern_native_type(item.native_type);
				uint64_t naddr = (uint64_t)item.obj;

				auto it = m_native_allocations.find(naddr);
				if (it != m_native_allocations.end())
				{
					// Address already live (a missed free, or in-place realloc): report the
					// old block freed before the new one, so size accounting stays correct
					m_events_sink->report_free(item.frame, naddr, it->second);
					m_freed += it->second;
					it->second = item.size;
				}
				else
					m_native_allocations.emplace(naddr, item.size);

				m_events_sink->report_alloc(item.frame, naddr, item.size, type_id, callstack.id);
				m_allocated += item.size;
				continue;
			}

			// Report free event to client
			if (item.type == work_item_type::free)
			{
				m_events_sink->report_free(item.frame, (uint64_t)item.obj, item.size);
				m_freed += item.size;
				continue;
			}

			// 1. ---------- Intern type and callstack. Names are resolved and definitions are
			// reported to client only for types and callstacks seen for the first time;
			// afterwards, it's a single hash lookup.

			uint32_t type_id = intern_type(item.klass);
			

			callstack_entry callstack = intern_callstack(item.backtrace);

			// Allocations with stopworded callstacks are not tracked at all
			if (callstack.stopword)
				continue;

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

			m_events_sink->report_alloc(item.frame, addr, item.size, type_id, callstack.id);
			m_allocated += item.size;
		}

		// Clear queue when thread stops
		auto empty_queue = moodycamel::ConcurrentQueue<work_item>();
		m_work_items.swap(empty_queue);
		m_allocations.clear();
		m_native_allocations.clear();
	}

	void worker_thread::start()
	{
		m_thread = std::thread(&worker_thread::do_work, this);
	}

	void worker_thread::stop()
	{
		// Unpause the app if it was paused
		if (m_stop_lock.owns_lock())
		{
			m_allocs_blocked = false;
			m_stop_lock.unlock();
		}

		m_stop = true;
		if (m_thread.joinable())
			m_thread.join();
	}

	void worker_thread::add_allocation_async(uint64_t frame, MonoClass* klass, MonoObject* obj)
	{
		// Pause gate. Taking m_stop_mutex in shared mode on every allocation is an atomic
		// RMW on a shared cache line, so the common case checks a simple flag instead.
		// The flag is advisory: an allocation racing with pause_app can slip through once,
		// which only means the pause takes effect on that thread's next allocation.
		if (m_allocs_blocked.load(std::memory_order_relaxed))
		{
			// Block until the pause or the references query ends
			std::shared_lock stop_lock(m_stop_mutex);
		}
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
#if defined(WIN32)
		if (m_capture_raw_ips)
		{
#if OWLCAT_MONO
			// Refresh the domain used for jit info lookups on the worker thread. This thread
			// is attached to the runtime (we are inside an allocation callback), so
			// mono_domain_get is valid here, and refreshing it every time keeps the pointer
			// good across domain reloads. Skipped if the jit functions weren't resolved.
			if (m_jit_available)
				m_jit_domain.store(mono_functions::domain_get(), std::memory_order_relaxed);
#endif

			// Capture raw instruction pointers. Unlike mono_stack_walk, this doesn't perform
			// a jit info table lookup per frame - that's where most of the capture cost is.
			// The pointers are translated to names on the worker thread, once per unique
			// callstack (see intern_callstack).
			item.backtrace.count = capture_stack(item.backtrace.frames, (uint32_t)stack_backtrace::MAX_DEPTH);
			// If the buffer is full, deeper frames may have been dropped
			item.backtrace.overflow = item.backtrace.count == stack_backtrace::MAX_DEPTH;
		}
		else
#endif
		{
#if OWLCAT_MONO
			mono_functions::stack_walk(
				[](MonoMethod* method, int32_t native_offset, int32_t il_offset, mono_bool managed, void* data) -> mono_bool
				{
					return ((stack_backtrace*)data)->add_trace(method, native_offset, il_offset, managed);
				}, (void*)&item.backtrace);
#else
			mono_functions::stack_walk(
				[](const Il2CppStackFrameInfo* frame_info, void* data)
				{
					((stack_backtrace*)data)->add_trace(frame_info->method, 0, 0, true);
				}, (void*)&item.backtrace);
#endif
		}
		// No token: each game thread gets its own implicit producer inside the queue,
		// so allocating threads don't synchronize with each other at all
		m_work_items.enqueue(item);
	}

	void worker_thread::set_native_types(const std::vector<std::string>& labels)
	{
		m_native_type_labels = labels;
		m_native_type_ids.assign(labels.size(), -1);
	}

	void worker_thread::add_native_allocation(uint64_t frame, uint64_t addr, uint32_t size, uint32_t label_index)
	{
		// Same pause gate as managed allocations
		if (m_allocs_blocked.load(std::memory_order_relaxed))
		{
			std::shared_lock stop_lock(m_stop_mutex);
		}

		work_item item;
		item.frame = frame;
		item.klass = nullptr;
		item.obj = (MonoObject*)addr;
		item.size = size;
		item.native_type = label_index;
		item.type = work_item_type::native_alloc;

#if defined(WIN32)
		// Native frames are always raw instruction pointers
		item.backtrace.count = capture_stack(item.backtrace.frames, (uint32_t)stack_backtrace::MAX_DEPTH);
		item.backtrace.overflow = item.backtrace.count == stack_backtrace::MAX_DEPTH;
#else
		item.backtrace.count = 0;
#endif

		m_work_items.enqueue(item);
	}

	void worker_thread::add_native_free(uint64_t frame, uint64_t addr)
	{
		if (m_allocs_blocked.load(std::memory_order_relaxed))
		{
			std::shared_lock stop_lock(m_stop_mutex);
		}

		work_item item;
		item.frame = frame;
		item.klass = nullptr;
		item.obj = (MonoObject*)addr;
		item.size = 0;
		item.native_type = 0;
		item.type = work_item_type::native_free;
		item.backtrace.count = 0;

		m_work_items.enqueue(item);
	}

	// It is possible that the memory pointed to by addr is no longer accessible to us.
	// We can't know if it is, so we use SEH to handle the resulting access violation.
	// Actually, we probably can call into BoehmGC itself to check, but this is a more complicated and less stable way
	// This approach, however, can hide some errors
	intptr_t get_ptr_safe(const uint8_t* p)
	{
#ifdef WIN32
		__try
		{
#endif
			return *((intptr_t*)p);
#ifdef WIN32
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// TODO: Add log?
		}

		return 0;
#endif
	}

	int worker_thread::do_gc_internal(uint64_t frame, bool only_update_parents)
	{
		int iterations = 0;

		std::scoped_lock gc_lock(m_gc_mutex);
		std::scoped_lock roots_lock(m_roots_mutex);

		++m_gc_generation;

		m_stack.reserve(1024 * 1024);
		m_stack.clear();

		// Clear all objects' marks
		for (auto iter = m_allocations.begin(); iter != m_allocations.end(); ++iter)
		{
			auto& alloc = alloc_value(iter);
			alloc.reset_flag(alloc_info::flag::TMP_ALLOCATED);
			alloc.reset_flag(alloc_info::flag::IS_ROOT);
			alloc.reset_flag(alloc_info::flag::TMP_VISITED);
			// Parents are only needed by find_references, so a normal collection
			// doesn't pay for maintaining them
			if (only_update_parents)
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
					if (only_update_parents)
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
				// Managed references are always pointer-aligned (objects are allocated aligned,
				// and reference fields sit at aligned offsets), and we only match exact object
				// base addresses. Scanning at every byte offset would be 8x slower and could
				// only produce false positives from values straddling two fields.
				p += sizeof(intptr_t);
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

		// Parents are now up to date with the object graph as of this pass
		if (only_update_parents)
			m_parents_generation = m_gc_generation.load();

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
		
		return do_gc_internal(frame, only_update_parents);
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

		// Clear the "visited" marks possibly left over from a previous query (a parents-building
		// GC pass also clears them, but it doesn't necessarily run for every query)
		for (auto iter = m_allocations.begin(); iter != m_allocations.end(); ++iter)
			alloc_value(iter).reset_flag(alloc_info::flag::TMP_VISITED);

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
					auto check_iter = m_allocations.find(parent);
					// The parent may have been freed by a GC pass after the parents list was built
					if (check_iter == m_allocations.end())
						continue;
					// Skip parents we have already seen
					if (alloc_value(check_iter).flag(alloc_info::flag::TMP_VISITED))
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
		// When the app is paused, allocations are already blocked, and we don't need any locks
		if (m_stop_lock.owns_lock())
		{
			// Parents lists are only built by a parents-building pass (a normal collection
			// skips them for speed), so rebuild them if any GC pass ran since the last rebuild.
			// The pass sets only_update_parents to true to avoid actually removing any objects
			// and reporting free events to client.
			if (m_parents_generation != m_gc_generation)
				do_gc_sync(frame, true);

			find_references_internal(request_id, addresses);
		}
		// When the app is not paused, block allocation reporting the same way pause_app does:
		// otherwise the work queue might never drain in do_gc_sync while the game keeps
		// allocating, and the list of allocations could change while we walk it
		else
		{
			std::unique_lock<std::shared_mutex> quiesce_lock(m_stop_mutex);
			m_allocs_blocked = true;

			if (m_parents_generation != m_gc_generation)
				do_gc_sync(frame, true);

			{
				std::scoped_lock gc_lock(m_gc_mutex);
				std::scoped_lock roots_lock(m_roots_mutex);
				find_references_internal(request_id, addresses);
			}

			m_allocs_blocked = false;
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
			m_allocs_blocked = true;
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
			m_allocs_blocked = false;
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
