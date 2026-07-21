#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network.h" // owlcat::capture_flags

namespace owlcat
{
	class logger;

	/*
		Structure that stores information about references to the specified objects
	*/
	struct object_references_t
	{
		object_references_t(uint64_t _addr, uint64_t _size)
			: addr(_addr)
		{
		}

		// Object address
		uint64_t addr;
		// Object type as string
		std::string type;
		// A list of addresses of object's parents
		std::vector<uint64_t> parents;
	};

	/*
		Interface used by profiler to report events and send responses to commands
	*/
	class events_sink
	{
	public:
		virtual void report_alloc(uint64_t frame, uint64_t addr, uint32_t size, uint32_t type_id, uint32_t callstack_id) = 0;
		virtual void report_free(uint64_t frame, uint64_t addr, uint32_t size) = 0;
		// Type, frame and callstack definitions. The profiler assigns the ids, and reports each
		// definition once, before the first message that references it.
		virtual void report_type(uint32_t type_id, const char* name) = 0;
		// A single callstack frame line, interned by text. Reported before any callstack that
		// references it (see report_callstack).
		virtual void report_frame(uint32_t frame_id, const char* text) = 0;
		// A callstack, as the sequence of frame ids (top of stack first) that make it up.
		virtual void report_callstack(uint32_t callstack_id, const std::vector<uint32_t>& frame_ids) = 0;
		// Per-frame whole-process memory snapshot (see SRV_MEMSTATS). No-op by default so
		// sinks that don't care (e.g. the test) needn't implement it.
		virtual void report_memstats(uint64_t frame, uint64_t working_set, uint64_t committed, uint64_t gc_heap) {}
		virtual void report_references(uint64_t request_id, const std::vector<object_references_t>& references) = 0;
		virtual void report_paused(uint64_t request_id, bool ok) = 0;
		virtual void report_resumed(uint64_t request_id, bool ok) = 0;
		// Diagnostic hook (see OWLCAT_PROFILER_MEMLOG in worker_thread.h): the worker calls
		// this so the sink can log the size of the containers it owns (the type/callstack
		// definition tables and the network send buffer). No-op by default.
		virtual void log_memory_stats(logger*) {}
		// Bytes currently buffered on the send side. The worker uses this to apply back-pressure
		// (throttle game allocations) when the client can't keep up, so the buffer stays bounded
		// instead of growing without limit. 0 means "no send side" (never throttle).
		virtual uint64_t pending_send_bytes() { return 0; }
	};

	/*
	    Main profile class
	*/
	class mono_profiler
	{
		class details;
		details* m_details;

	public:
		mono_profiler(events_sink* sink);
		~mono_profiler();

		// Starts, or restarts the profiler. flags selects managed/native tracking;
		// native_config is the path to the native-hook config file (used only when
		// CAPTURE_NATIVE is set).
		bool start(uint32_t flags, const std::string& native_config);
		// Notifies the profiler that frame number has changed
		void on_frame();

		// Finds references to the specified object and reports them back to client with events_sink interface
		void find_references(uint64_t request_id, const std::vector<uint64_t>& addresses);
		// Pauses the profiled application (see implementation for details)
		void pause_app(uint64_t request_id);
		// Unpauses the profiled application
		void resume_app(uint64_t request_id);
	};
}
