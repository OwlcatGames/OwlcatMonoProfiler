#include <iostream>
#include <sstream>
#include <algorithm>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <functional>
#include <unordered_map>

#include "mono/metadata/profiler.h"

#include "logger.h"

#include "mono_profiler.h"
#include "worker_thread.h"
#include "functor.h"
#include "mono_functions.h"

using namespace owlcat::mono_functions;

namespace owlcat
{
	// Mono doesn't seem to actually define these constants, so we define them ourselves
	typedef enum {
		MONO_PROFILE_NONE = 0,
		MONO_PROFILE_APPDOMAIN_EVENTS = 1 << 0,
		MONO_PROFILE_ASSEMBLY_EVENTS = 1 << 1,
		MONO_PROFILE_MODULE_EVENTS = 1 << 2,
		MONO_PROFILE_CLASS_EVENTS = 1 << 3,
		MONO_PROFILE_JIT_COMPILATION = 1 << 4,
		MONO_PROFILE_INLINING = 1 << 5,
		MONO_PROFILE_EXCEPTIONS = 1 << 6,
		MONO_PROFILE_ALLOCATIONS = 1 << 7,
		MONO_PROFILE_GC = 1 << 8,
		MONO_PROFILE_THREADS = 1 << 9,
		MONO_PROFILE_REMOTING = 1 << 10,
		MONO_PROFILE_TRANSITIONS = 1 << 11,
		MONO_PROFILE_ENTER_LEAVE = 1 << 12,
		MONO_PROFILE_COVERAGE = 1 << 13,
		MONO_PROFILE_INS_COVERAGE = 1 << 14,
		MONO_PROFILE_STATISTICAL = 1 << 15,
		MONO_PROFILE_METHOD_EVENTS = 1 << 16,
		MONO_PROFILE_MONITOR_EVENTS = 1 << 17,
		MONO_PROFILE_IOMAP_EVENTS = 1 << 18, /* this should likely be removed, too */
		MONO_PROFILE_GC_MOVES = 1 << 19
	} MonoProfileFlags;

	// Profiler implementation
	class mono_profiler::details
	{
	public:
		// If true, the profiler was already initialized
		bool m_started = false;

		// A handle to Mono dynamic library
		std::unique_ptr<library> m_module_mono;

		// Logger. Maybe shouldn't be here.
		logger m_logger;
		// A file log sink. This well break on platforms where we don't have write access to file system, probably
		std::unique_ptr<sink_file> m_log_sink;

		// The worker thread which does most of heavy lifting
		std::unique_ptr<worker_thread> m_processing_thread;

		// A sink for reporting events to client. mono_profiler_server is reponsible for creating and destroying it
		events_sink* m_events_sink;

		// Current frame
		uint64_t m_frame_index = 0;

		// A mutex to prevent two allocations being reported concurrently from different threads (this leads to wrong frame number order in sequential allocations)
		std::mutex m_add_alloc_mutex;

	private:
		void on_shutdown()
		{
		}

		// Callback for allocation events
		void on_allocation(MonoObject* obj, MonoClass* klass)
		{
			std::scoped_lock items_lock(m_add_alloc_mutex);

			m_processing_thread->add_allocation_async(m_frame_index, klass, obj);
		}

		// Callback for GC events
		void on_gc(MonoProfilerGCEvent e)
		{
			if (e != MONO_GC_EVENT_END)
				return;

			auto t1 = std::chrono::high_resolution_clock::now();
			int iterations = m_processing_thread->do_gc_sync(m_frame_index, false);
			auto t2 = std::chrono::high_resolution_clock::now();

			std::chrono::duration<double> diff = t2 - t1;
			char tmp[256];
			sprintf(tmp, "GC took %fs. %i iterations, %f per iter", diff.count(), iterations, diff.count()/iterations);
			m_logger.log_str(tmp);
		}
		
		// Callback for root registration
		void on_root_register(const mono_byte* start, int source, size_t size, const void* key, const char* name)
		{
			//char tmp[1024];
			//sprintf(tmp, "Add: source=%i %p %I64u %s", source, start, size, name);
			//m_logger.log_str(tmp);
			m_processing_thread->register_root((const char*)start, size);
		}

		// Callback for root unregistration
		void on_root_unregister(const mono_byte* start)
		{			
			//char tmp[1024];
			//sprintf(tmp, "Rem: %p", start);
			//m_logger.log_str(tmp);
			m_processing_thread->unregister_root((const char*)start);
		}
		
	public:
		details(events_sink* sink)
		{
			m_events_sink = sink;
			m_log_sink = std::make_unique<sink_file>("log.txt");
			m_logger.add_sink(m_log_sink.get());
		}

		~details()
		{
			m_log_sink.reset();
		}

		bool try_restart_profiling()
		{
			if (!m_started)
				return false;

			m_logger.log_str("restarting profiling");
			m_processing_thread->stop();
			m_processing_thread = std::make_unique<worker_thread>(m_events_sink);
			m_processing_thread->start();

			return true;
		}

		bool setup_mono_functions()
		{
			library* module_mono = m_module_mono.get();

			return
				install_allocations_proc.init(module_mono, m_logger) &&
				install_gc_proc.init(module_mono, m_logger) &&
				set_events_proc.init(module_mono, m_logger) &&
#ifdef OWLCAT_MONO
				create_profiler_handle.init(module_mono, m_logger) &&
#endif
				set_gc_register_root_proc.init(module_mono, m_logger) &&
				set_gc_unregister_root_proc.init(module_mono, m_logger) &&
				profiler_install_proc.init(module_mono, m_logger) &&
				get_class_namespace.init(module_mono, m_logger) &&
				get_class_name.init(module_mono, m_logger) &&
				stack_walk.init(module_mono, m_logger) &&
				get_method_name.init(module_mono, m_logger) &&
				method_get_class.init(module_mono, m_logger) &&
				object_get_class.init(module_mono, m_logger) &&
				object_get_size.init(module_mono, m_logger) &&
				//begin_liveness_calculation.init(module_mono, m_logger) &&
				//end_liveness_calculation.init(module_mono, m_logger) &&
				//calculate_liveness_from_statics.init(module_mono, m_logger) &&
				true;
		}

		void init(mono_profiler* profiler)
		{
			m_logger.log_str("new profiler");
			//profiler = new OwlcatMonoProfiler();

			// 1. Install the profiler and the function to be called when Mono shuts down (won't ever be called in Editor, so don't rely on it to do clean up there...)
			m_logger.log_str("profiler_install_proc");
			profiler_install_proc((MonoProfiler*)this, 
				[](MonoProfiler* p)
				{
					((details*)p)->on_shutdown();
				}
			);

			// 2. Install allocations handler. It will be called each time an objects is allocated on GC heap
			m_logger.log_str("install_allocations_proc");
			install_allocations_proc(
				[](MonoProfiler* p, MonoObject* obj, MonoClass* klass)
				{
					((details*)p)->on_allocation(obj, klass);
				}				
			);

			// 3. Install GC event handler. It will be called whenever GC does something, like begin or end garbage collection
			m_logger.log_str("install_gc_proc");
			install_gc_proc(
				[](MonoProfiler* p, MonoProfilerGCEvent e, int gen)
				{
					((details*)p)->on_gc(e);
				},
				[](MonoProfiler* p, int64_t new_size)
				{
					//((details*)p)->on_heap_resize(e);
				}
			);

			// 4. Specify which events we're interested in. Unless this is called, the handlers above won't be active
			m_logger.log_str("set_events_proc");
			set_events_proc(MONO_PROFILE_ALLOCATIONS | MONO_PROFILE_GC);

			// 5. Create a handle for "new" profiler interface
#if OWLCAT_MONO
			m_logger.log_str("create_profiler_handle");
			auto handle = create_profiler_handle((MonoProfiler*)this);

			//bool allocations_ok = set_enable_allocations();
			//m_logger.log_str(allocations_ok ? "Can log allocations" : "CAN'T LOG ALLOCATIONS");			

			// 6. Install roots callback with the new handle
			m_logger.log_str("set_gc_register_root_proc");
			set_gc_register_root_proc(handle,
				[](MonoProfiler* p, const mono_byte* start, size_t size, MonoGCRootSource source, const void* key, const char* name)
				{
					// This values must not be used to register roots according to Mono docs
					if (source == MONO_ROOT_SOURCE_STACK || source == MONO_ROOT_SOURCE_FINALIZER_QUEUE)
						return;

					((details*)p)->on_root_register(start, source, size, key, name);
				});
			set_gc_unregister_root_proc(handle,
				[](MonoProfiler* p, const mono_byte* start)
				{
					((details*)p)->on_root_unregister(start);
				});
#else
			// 6. Install roots callback with the new handle
			m_logger.log_str("set_gc_register_root_proc");
			set_gc_register_root_proc(
				[](MonoProfiler *p, char* start, size_t size)
				{
					((details*)p)->on_root_register((mono_byte*)start, MONO_ROOT_SOURCE_EXTERNAL, size, "", "");
				});
			set_gc_unregister_root_proc(
				[](MonoProfiler* p, char* start)
				{
					((details*)p)->on_root_unregister((mono_byte*)start);
				});
#endif
			// 7. Start worker thread that stores allocations in memory (it does a lot of heavy lifting with strings and containers, so we run it in another thread)
			m_processing_thread = std::make_unique<worker_thread>(m_events_sink);
			m_processing_thread->start();
		}
	};

	mono_profiler::mono_profiler(events_sink* sink)
	{
		m_details = new details(sink);
	}

	bool mono_profiler::start()
	{
		// If the start is called the second time, we don't need to do anything, but
		// restart the worker thread
		if (m_details->try_restart_profiling())
			return true;

		m_details->m_logger.log_str("mono_profiler::start called");

#if OWLCAT_MONO
		m_details->m_logger.log_str("mono-2.0-bdwgc.dll");
#else		
		m_details->m_logger.log_str("GameAssembly.dll");
#endif

		// TODO: different name for *nix
		// Get a handle to Mono library
#if OWLCAT_MONO
		m_details->m_module_mono = library::get_library("mono-2.0-bdwgc.dll");
#else		
		m_details->m_module_mono = library::get_library("GameAssembly.dll");
#endif

		if (m_details->m_module_mono == nullptr)
		{
			m_details->m_logger.log_str("Failed to get handle to mono-2.0-bdwgc.dll");
			return false;
		}

		// Find Mono functions that we need
		if (!m_details->setup_mono_functions())
		{
			return false;
		}

		m_details->init(this);

		m_details->m_logger.log_str("all OK, starting profiling");

		m_details->m_started = true;

		return true;
	}

	void mono_profiler::on_frame()
	{
		++m_details->m_frame_index;
	}

	void mono_profiler::find_references(uint64_t request_id, const std::vector<uint64_t>& addresses)
	{
		if (m_details->m_processing_thread->is_paused())
		{
			m_details->m_processing_thread->find_references(request_id, addresses, m_details->m_frame_index);
		}
		else
		{
			std::scoped_lock items_lock(m_details->m_add_alloc_mutex);
			m_details->m_processing_thread->find_references(request_id, addresses, m_details->m_frame_index);
		}
	}

	void mono_profiler::pause_app(uint64_t request_id)
	{
		m_details->m_processing_thread->pause_app(request_id);
	}

	void mono_profiler::resume_app(uint64_t request_id)
	{
		m_details->m_processing_thread->resume_app(request_id);
	}
}
