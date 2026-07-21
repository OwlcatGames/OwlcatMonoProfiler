#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "network.h"

namespace owlcat
{
	/*
		A list of references to an object
	*/
	struct object_references_t
	{
		// Object address
		uint64_t address;
		// Object type
		std::string type;

		struct parent_info
		{
			uint64_t address;
		};
		// A list of references to this object
		std::vector<parent_info> parents;
	};

	struct search_result_t
	{
		uint64_t type_id;
		std::vector<uint64_t> callstacks;
	};

	// Callbacks for commands
	using find_references_callback = std::function<void(const std::vector<uint64_t> addresses, std::string error, const std::vector<object_references_t>& result)>;
	using pause_app_callback = std::function<void(bool ok)>;
	using resume_app_callback = std::function<void(bool ok)>;

	class mono_profiler_client_data;
	/*
		Profiler client class. It handles network communucations, stores the profiling data in database and provides functions to query it
	*/
	class mono_profiler_client
	{
		friend class mono_profiler_client_data;
		class details;

		details *m_details;

	public:
		mono_profiler_client();
		~mono_profiler_client();

//#if defined(WIN32)
		enum LaunchResult
		{
			OK,
			DETOUR_CREATE_WITH_DLL_FAILED,
			DETOUR_PIPE_TIMEOUT,
			DETOUR_PIPE_FAILED,
			DETOUR_PIPE_READ_FAILED,
			PDB_NOT_FOUND,
			DETOUR_FAILED,
			BAD_VERSION,
			DETOUR_FAILED_LATE,
			CONNECT_FAILED
		};

		// Launches target executable, detouring it using profiler's DLL. Allows profiling non-instrumented apps.
		// capture_flags selects managed/native tracking (owlcat::capture_flags); native_config
		// is the text of the native-hook config file (only used when CAPTURE_NATIVE is set).
		LaunchResult launch_executable(const std::string& executable, const std::string& args, int port, const std::string& db_file_name, const std::string& dll_location, uint32_t capture_flags = CAPTURE_MANAGED, const std::string& native_config = std::string());
//#endif
		// Attempts to connect to a running profiler server, then tells it what to capture.
		// capture_flags / native_config are sent to the server as CMD_CONFIGURE.
		bool start(const std::string& addr, int server_port, const std::string& db_file_name, uint32_t capture_flags = CAPTURE_MANAGED, const std::string& native_config = std::string());
		// Stops communications with profiler server. Leaves current profiling data accessible.
		void stop();
		// Closes profiler data database. It will no longer be accessible.
		void close_db();
		// Saves in-memory or temporary database to a persistent file
		bool save_db(const std::string& new_db_file_name, bool move);
		// Opens previously saved profiling data
		bool open_data(const std::string& file);

		// Returns true if the client is connected to a server
		bool is_connected() const;
		// Returns true if the client is currently neither connected, nor disconnected?
		bool is_connecting() const;
		// Returns true if database is open
		bool is_data_open() const;

		// Returns the number of messages in network queue
		size_t get_network_messages_count() const;

		// Returns the total number of profiler events (allocations and frees) written to
		// the database so far. Monotonic: sample it periodically to measure the insert rate.
		uint64_t get_db_inserted_events_count() const;

		// Returns the path of the event log file of the current capture (empty if no
		// capture is open). Used by diagnostic tools to access the raw event stream.
		const char* get_event_log_path() const;

		// Sets the local symbol search path (';'-separated directories) used to resolve
		// native callstack frames to function names. Applied in the background and
		// re-resolves all known callstacks.
		void set_symbol_paths(const std::string& paths);

		// Returns an interface for querying profiling data
		mono_profiler_client_data* get_data();

		// Sends a command to server to retrieve a list of references to the specified list of objects
		void find_objects_references(const std::vector<uint64_t>& addresses, find_references_callback callback);
		// Sends a command to server to pause the app
		void pause_app(pause_app_callback callback);
		// Sends a command to server to unpause the app
		void resume_app(resume_app_callback callback);

		// Returns a name for thr specified type from an in-memory cache
		const char* get_type_name(uint64_t type_id) const;
	};

	/*
		A description of a live object (i.e. an object that was allocated, and was not deallocated during the specified timeframe)
	*/
	struct live_object
	{
		live_object(uint64_t addr, uint64_t size, uint64_t frame, uint64_t type_id, uint64_t callstack_id)
			: addr(addr)
			, size(size)
			, frame(frame)
			, type_id(type_id)
			, callstack_id(callstack_id)
		{
		}

		// Object address
		uint64_t addr;
		// Object size
		uint64_t size;
		// Frame when object was allocated
		uint64_t frame;
		// Object's type ID
		uint64_t type_id;
		// The ID of callstack where the object was allocated
		uint64_t callstack_id;
	};

	// Universal progress callback typr
	using progress_func_t = std::function<bool(size_t current, size_t max)>;

	/*
		Interface for querying profiling data saved in database
	*/
	class mono_profiler_client_data
	{
		mono_profiler_client::details* m_source;

	protected:
		mono_profiler_client_data(mono_profiler_client::details* source);
		friend class mono_profiler_client::details;

	public:
		// Get minimum and maximum known frame indx
		void get_frame_boundaries(uint64_t& min, uint64_t& max);
		// Get number of allocations, frees, maximum number of allocations, frees and running total of allocated memory for the specified timeframe
		void get_frame_stats(std::vector<uint64_t>& alloc_counts, std::vector<uint64_t>& free_counts, uint64_t& max_allocs, uint64_t& max_frees, std::vector<uint64_t>& size_points, int64_t& max_size, uint64_t from_frame, uint64_t to_frame);
		// Per-frame whole-process memory (committed/working-set/GC-heap bytes), aligned like
		// get_frame_stats' size_points. Empty for captures made before this was added.
		void get_memory_series(std::vector<uint64_t>& committed_points, std::vector<uint64_t>& working_set_points, std::vector<uint64_t>& gc_heap_points, uint64_t& max_committed, uint64_t from_frame, uint64_t to_frame);
		// Returns a list of live objects for the specified timeframe
		void get_live_objects(std::vector<live_object>& objects, int from, int to, progress_func_t progress_func);
		// Returns a name for type ID
		const char* get_type_name(uint64_t type_id) const;
		// Returns callstack text for callstack ID (symbolicated if resolved). Returned by
		// value because the text can be updated by the background symbolication thread.
		std::string get_callstack(uint64_t callstack_id) const;
		// Returns number of known types
		size_t get_types_count() const;
		// Returns number of known callstacks
		size_t get_callstacks_count() const;
		// Queries the database for information about a particular allocation
		bool get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id) const;
	};
}
