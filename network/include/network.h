#pragma once

#include <cstdint>
#include <vector>
#include <string>

//#define DEBUG_NETWORK
//
//#ifdef DEBUG_NETWORK
//	#define DEBUG_NETWORK_TEXT
//#endif

namespace owlcat
{
	namespace protocol
	{
		enum message
		{
			SRV_ALLOC = 1,
			SRV_FREE,
			SRV_REFERENCES,
			SRV_PAUSE,
			SRV_RESUME,
			// Definitions of type and callstack ids referenced by SRV_ALLOC messages.
			// A definition is always sent before the first SRV_ALLOC that references it.
			SRV_TYPE,
			SRV_CALLSTACK,
			// Definition of a single callstack frame ("Class.Method" or "Module.dll+0xRVA").
			// Callstacks are sent as sequences of frame ids (SRV_CALLSTACK), and each frame
			// id is defined once here before any callstack that references it. This collapses
			// the huge redundancy of the ~90k unique frame lines shared across millions of
			// callstacks (they would otherwise be re-sent as full text in every callstack).
			SRV_FRAME,
			// Per-frame whole-process memory snapshot (working set, committed bytes, managed
			// GC heap). Lets the UI graph total committed memory against the tracked allocations
			// - the gap is native allocator pool overhead the per-allocation view can't show.
			SRV_MEMSTATS,
		};

		enum command
		{
			CMD_REFERENCES = 1,
			CMD_PAUSE,
			CMD_RESUME,
			// Sent by the client immediately after connecting: selects what to capture
			// (managed/native) and carries the native-hook config text. The in-game
			// profiler defers its startup until this arrives, so it can hook per the
			// client's configuration (this is how the Editor / manually-instrumented
			// builds are configured, where env vars can't reach the injected DLL).
			CMD_CONFIGURE,
		};

		extern const char* pipe_name;

		extern const char* error_ok;
		extern const char* error_symbols;
		extern const char* error_detour;
		extern const char* error_deque;
		extern const char* error_detour_late;
	}

	// What a capture tracks. Sent by the client in CMD_CONFIGURE. The managed backend
	// (Mono vs IL2CPP) is fixed at compile time (two server DLLs); these flags select
	// managed and/or native heap tracking at runtime.
	enum capture_flags : uint32_t
	{
		CAPTURE_MANAGED = 1 << 0, // Mono/IL2CPP managed heap (the pseudo-GC)
		CAPTURE_NATIVE  = 1 << 1, // native heap, via hooked allocators (see native_hooks)
	};

	struct message
	{
#ifdef DEBUG_NETWORK
static uint32_t next_id;
#endif
		message() {}
		message(uint8_t type, uint32_t length, const uint8_t* data)
		{
#ifdef DEBUG_NETWORK
			header.id = next_id++;
#endif
			header.type = type;
			header.length = length;
			this->data.assign(data, data + length);
		}

		struct header
		{
#ifdef DEBUG_NETWORK
uint32_t id;
#endif
			uint32_t length = 0;
			uint8_t type = 0;
		};

		header header;
		std::vector<uint8_t> data;
	};

	class network
	{
		class details;
		details* m_details;

	public:
		network();
		~network();

		void listen_async(int port);
		void listen_sync(int port);
		bool connect(const std::string& address, int port);

		bool is_connected() const;
		bool is_connecting() const;
		bool is_listening() const;

		// Number of connections established so far. Can be used to detect reconnects.
		uint64_t connection_generation() const;

		void stop();

		void write_message(uint8_t type, uint32_t length, const uint8_t* data);
		bool read_message(message& msg);

		size_t get_read_messages_count() const;
		// Bytes currently buffered on the send side (accumulated + in-flight). Grows without
		// bound if the socket can't drain as fast as events are produced, so it's a key
		// figure for diagnosing profiler memory use during an allocation storm.
		size_t get_pending_write_bytes() const;
	};
}
