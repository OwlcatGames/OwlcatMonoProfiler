#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network.h" // owlcat::capture_flags

namespace owlcat
{
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
		// Type and callstack definitions. The profiler assigns the ids, and reports each definition
		// once, before the first allocation that references it
		virtual void report_type(uint32_t type_id, const char* name) = 0;
		virtual void report_callstack(uint32_t callstack_id, const char* text) = 0;
		virtual void report_references(uint64_t request_id, const std::vector<object_references_t>& references) = 0;
		virtual void report_paused(uint64_t request_id, bool ok) = 0;
		virtual void report_resumed(uint64_t request_id, bool ok) = 0;
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
