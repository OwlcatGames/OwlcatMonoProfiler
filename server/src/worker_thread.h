#pragma once

#include "mono/metadata/profiler.h"
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <unordered_map>
#include <concurrentqueue.h>
//#include "tsl/robin_map.h"

//#define DEBUG_ALLOCS

// Uncomment to periodically log the sizes of the profiler server's memory-holding
// containers (work queue, live-object map, callstack tables, network send buffer, ...)
// to the profiler log. For diagnosing the in-game profiler's memory overhead. The
// define lives here (not on the compiler command line) so that every translation unit
// that includes this header agrees on worker_thread's layout.
#define OWLCAT_PROFILER_MEMLOG
// How often to log, in milliseconds.
#ifndef OWLCAT_PROFILER_MEMLOG_INTERVAL_MS
	#define OWLCAT_PROFILER_MEMLOG_INTERVAL_MS 5000
#endif

#if defined(OWLCAT_PROFILER_MEMLOG)
	#include <chrono>
#endif

extern volatile size_t map_size;

/*
	Allocator used to examine the size of hash maps used by the profiler
*/
template<typename T>
class counting_allocator
{
private:
	using real_allocator = std::allocator<T>;
	real_allocator m_real_allocator;

public:
	counting_allocator() {}
	counting_allocator(const counting_allocator<T>& other) {}
	template<typename TOther>
	counting_allocator(const counting_allocator<TOther>& other) {}

	template<typename X>
	struct rebind
	{
		typedef counting_allocator<X> other;
	};

	using value_type = typename real_allocator::value_type;
	using size_type = typename real_allocator::size_type;
	using difference_type = typename real_allocator::difference_type;
	using propagate_on_container_move_assignment = typename real_allocator::propagate_on_container_move_assignment;
	using is_always_equal = typename real_allocator::is_always_equal;

	value_type* allocate(std::size_t n)
	{
		map_size += n * sizeof(T);
		return m_real_allocator.allocate(n);
	}

	void deallocate(value_type* p, std::size_t n)
	{
		map_size -= n * sizeof(T);
		m_real_allocator.deallocate(p, n);
	}
};

template<>
class counting_allocator<void>
{
public:
	template<typename X>
	struct rebind
	{
		typedef counting_allocator<X> other;
	};

	using value_type = void;
	using size_type = size_t;
	using difference_type = ptrdiff_t;
};


// return that all specializations of this allocator are interchangeable
template <class T1, class T2>
bool operator== (const counting_allocator<T1>&,
	const counting_allocator<T2>&) throw() {
	return true;
}
template <class T1, class T2>
bool operator!= (const counting_allocator<T1>&,
	const counting_allocator<T2>&) throw() {
	return false;
}

namespace owlcat
{
	class events_sink;
	class logger;

	/*
		A private, low-fragmentation heap for the profiler's own large, high-churn containers
		(the live-object map, callstack table, native-allocation map). Isolating their millions
		of small, mostly same-size node allocations from the process's shared CRT heap keeps the
		profiler from fragmenting the game's heap (and vice versa), and lets us measure the
		profiler's own footprint. Backed by a dedicated HeapCreate heap with the Low-Fragmentation
		Heap enabled (see worker_thread.cpp). The definitions live in the .cpp so this header
		doesn't pull in Windows.h.
	*/
	void* profiler_heap_alloc(size_t bytes);
	void profiler_heap_free(void* p, size_t bytes) noexcept;
	// Logical bytes currently allocated through profiler_allocator (sum of requested sizes).
	size_t profiler_heap_logical_bytes();

	template<typename T>
	class profiler_allocator
	{
	public:
		using value_type = T;
		profiler_allocator() noexcept {}
		template<typename U> profiler_allocator(const profiler_allocator<U>&) noexcept {}
		template<typename U> struct rebind { using other = profiler_allocator<U>; };

		T* allocate(std::size_t n) { return static_cast<T*>(profiler_heap_alloc(n * sizeof(T))); }
		void deallocate(T* p, std::size_t n) noexcept { profiler_heap_free(p, n * sizeof(T)); }
	};
	template<class T, class U> bool operator==(const profiler_allocator<T>&, const profiler_allocator<U>&) noexcept { return true; }
	template<class T, class U> bool operator!=(const profiler_allocator<T>&, const profiler_allocator<U>&) noexcept { return false; }

	/*
		Worker thread that stores current list of live allocations and handles
		allocations and GC events
	*/
	class worker_thread
	{
	private:
		// Callstacks containing these words won't be collected
		// TODO: Allow to specify stop-list from C# or client?
		std::vector<const char *> m_stopwords;		

		// For debugging
		//FILE* m_alloc_loc;

		// Total size of allocated and deallocated memory (managed + native)
		uint64_t m_allocated = 0;
		uint64_t m_freed = 0;
		// The native-only portion of the above, so managed live (= total - native) can be
		// compared against what the GC reports (mono_gc_get_used_size) to tell whether the
		// pseudo-GC is tracking the managed heap accurately or over-retaining dead objects.
		uint64_t m_native_allocated = 0;
		uint64_t m_native_freed = 0;

		/*
			Callstack storage. A fixed-size buffer, so that work_item is trivially copyable
			and enqueueing it never touches the heap on the game's threads.
		*/
		struct stack_backtrace
		{
			// Hard safety cap. Stacks deeper than this are truncated: the innermost
			// MAX_DEPTH frames are kept, and the truncation is logged (once).
			static constexpr size_t MAX_DEPTH = 64;

			mono_bool add_trace(MonoMethod* method, int32_t native_offset, int32_t il_offset, mono_bool managed);

			// Either MonoMethod* pointers (mono_stack_walk / IL2CPP walk), or raw instruction
			// pointers (native capture on Windows + Mono). The mode is fixed for the lifetime
			// of the worker, so the two kinds never mix within one session.
			void* frames[MAX_DEPTH];
			uint32_t count = 0;
			// True if frames beyond MAX_DEPTH were dropped
			bool overflow = false;
		};

		// alloc/free are Mono events (managed by the pseudo-GC); native_alloc/native_free
		// come from hooked native allocators and are freed explicitly (never GC-swept).
		enum class work_item_type : uint8_t {alloc, free, native_alloc, native_free};
		/*
			A work item to be processed by worker thread
		*/
		struct work_item
		{
			// Frame when event happened
			uint64_t frame;
			// Class of allocated object (nullptr for non-Mono-alloc events). For native
			// events, obj holds the raw native address instead.
			MonoClass* klass = 0;
			// Allocated object itself (nullptr for other events); native events store the address here
			MonoObject* obj = 0;
			// Size of allocated or freed object
			uint32_t size;
			// Index into the native type labels, for native_alloc events (see set_native_types)
			uint32_t native_type = 0;
			// Callstack at which object was allocated (empty for free events)
			stack_backtrace backtrace;
			// Type of event
			work_item_type type;
		};

		/*
			Concurrent queue of items to be processed
		*/
		moodycamel::ConcurrentQueue<work_item> m_work_items;
		/*
			Token for events enqueued by the profiler itself (free events from the pseudo-GC).
			Allocations from the game's threads are enqueued without a token: each thread then
			gets its own implicit producer inside the queue, so allocating threads never
			synchronize with each other. Per-thread ordering is preserved; global frame
			ordering across threads is restored by the clamp in do_work (see m_max_seen_frame).
		*/
		moodycamel::ProducerToken m_work_items_token;
		/*
			ConcurrentQueue doesn't support any way to check if it is empty, so here's a flag
			that the worker thread sets when it failed to dequeue an item. Atomic, because
			do_gc_sync spins on it from other threads.
		*/
		std::atomic<bool> m_work_items_empty = true;
		/*
			A pointer to a sink used to report events to client
		*/
		events_sink* m_events_sink;
		/*
			When true, signals the thread to stop the loop
		*/
		bool m_stop = false;
		/*
			Generation counters used to decide if the parents lists are up to date.
			m_gc_generation is incremented by every GC pass; a parents-building pass
			(only_update_parents == true) also sets m_parents_generation to the new value.
			Parents are stale whenever the two differ. Atomic, because find_references
			reads them from another thread.
		*/
		std::atomic<uint64_t> m_gc_generation = 0;
		std::atomic<uint64_t> m_parents_generation = (uint64_t)-1;

		/*
			Thread itself
		*/
		std::thread m_thread;
		/*
			Mutex that is preventing any work from being done on the thread while GC operation is in
			progress
		*/
		std::mutex m_gc_mutex;
		/*
			Mutex that is preventing concurrent access to list of roots
		*/
		std::mutex m_roots_mutex;
		/*
			This mutex along with the associated lock is used to pause the app
		*/
		std::shared_mutex m_stop_mutex;
		std::unique_lock<std::shared_mutex> m_stop_lock;
		/*
			True while allocations must block on m_stop_mutex (app pause, or a references
			query in progress). Checked on every allocation instead of always taking the
			shared lock, which would be an atomic RMW on a shared cache line.
		*/
		std::atomic<bool> m_allocs_blocked = false;

		/*
			Send-side back-pressure. When the outbound buffer or the work queue grows past a
			high-water mark (the client can't keep up), m_send_throttle is set and game
			allocation threads block on m_throttle_cv until it drains below the low-water mark.
			This keeps the profiler's transient buffers bounded instead of growing until OOM.
			Separate from the pause gate (m_stop_mutex) on purpose, so back-pressure and an
			explicit pause don't contend.
		*/
		std::atomic<bool> m_send_throttle{ false };
		std::mutex m_throttle_mutex;
		std::condition_variable m_throttle_cv;
		uint32_t m_throttle_counter = 0;

		/*
			Information about a single allocation
		*/
		struct alloc_info
		{
			// Allocation size
			uint32_t size;
			// A set of flags, temporary and permanent for this allocation
			uint8_t flags;
			// List of objects that refer to this allocation.
			// Only filled during a parents-building GC pass (only_update_parents == true),
			// because it is only ever read by find_references.
			// TODO: Optimize this, it takes about 30 bytes even when empty, and a lot of objects only have 1 or 2 references
			std::vector<uint64_t, counting_allocator<uint64_t>> parents;
#ifdef DEBUG_ALLOCS			
			std::string original_class;
			struct alloc_info* parent = 0;
			bool reallocated = false;
#endif

			enum class flag : uint8_t
			{
				TMP_ALLOCATED = 1 << 0,
				TMP_VISITED   = 1 << 1,
				IS_ROOT		  = 1 << 2,
			};

			void set_flag(flag f) { flags |= (uint8_t)f; }
			void reset_flag(flag f) { flags &= ~(uint8_t)f; }
			bool flag(flag f) { return (flags & (uint8_t)f) != 0; }
		};

		//using allocations_map = tsl::sparse_map<uint64_t, alloc_info, std::hash<uint64_t>, std::equal_to<uint64_t>, counting_allocator<std::pair<uint64_t, alloc_info>>>;
		//using allocations_map = tsl::robin_map<uint64_t, alloc_info>;
		// Nodes come from the profiler's private heap: this is the highest-count container
		// (one node per live managed object - millions), so its allocations dominate fragmentation.
		using allocations_map = std::unordered_map<uint64_t, alloc_info, std::hash<uint64_t>, std::equal_to<uint64_t>,
			profiler_allocator<std::pair<const uint64_t, alloc_info>>>;
		allocations_map m_allocations;

		inline uint64_t alloc_key(allocations_map::iterator& iter) { return iter->first; }
		inline alloc_info& alloc_value(allocations_map::iterator& iter) { return iter->second; }
		//inline uint64_t alloc_key(allocations_map::iterator& iter) { return iter.key(); }
		//inline alloc_info& alloc_value(allocations_map::iterator& iter) { return iter.value(); }

		struct stack_entry
		{
			uint64_t addr;
			alloc_info* info;
		};
		// Storing iterators doesn't work with robin_map/sparse_map
		//std::vector<allocations_map::iterator> m_stack;
		/*
			Working stack of GC
		*/
		std::vector<stack_entry> m_stack;

		/*
			Information about a root GC area, i.e. an area of memory which stores objects
			that should never be freed even if no references exist to them.
		*/
		struct root_info
		{
			const char* start;
			uint64_t size;
		};
		// A list of GC roots
		std::vector<root_info> m_roots;

		/*
			Live native allocations: address -> size. Native memory is freed explicitly by
			hooked free functions, so (unlike m_allocations) this is never touched by the
			pseudo-GC. It exists only so a native free(ptr), which carries no size, can
			recover the freed block's size for the size-accounting on the client.
			Only touched from the worker thread.
		*/
		std::unordered_map<uint64_t, uint32_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
			profiler_allocator<std::pair<const uint64_t, uint32_t>>> m_native_allocations;

		/*
			Labels for native allocation "types" (the display name of each configured hook).
			A native allocation has no MonoClass*, so its type is the hook's label. Ids are
			minted lazily from the same counter as Mono types (m_next_type_id), so native and
			managed type ids never collide on the wire.
		*/
		std::vector<std::string> m_native_type_labels;
		std::vector<int64_t> m_native_type_ids; // label index -> minted id, or -1 if not yet minted

		/*
			Symbol resolution caches and id interning (see do_work).
			Mono/IL2CPP never unload classes or methods in Unity, so caching resolved
			names by pointer is safe for the lifetime of the process.
			Everything below is only touched from the worker thread.
		*/

		// A resolved method name, as it appears as one line of callstack text
		struct method_entry
		{
			// "Class.Method\n"
			std::string text;
			// True if the name contains one of m_stopwords
			bool stopword;
			// Interned id of this frame line (see intern_frame_line). Not set for stopword frames.
			uint32_t frame_id = 0;
		};
		std::unordered_map<MonoMethod*, method_entry> m_method_cache;

		// Types already reported to client, and the ids they were reported with
		std::unordered_map<MonoClass*, uint32_t> m_type_ids;
		uint32_t m_next_type_id = 0;

		struct callstack_entry
		{
			uint32_t id;
			// Callstacks that contain a stopword are never reported, and neither are allocations made with them
			bool stopword;
		};
		// A callstack is identified by a 128-bit hash of its raw frame-pointer sequence. We do
		// not store the frames themselves (that vector-per-callstack was gigabytes and one heap
		// allocation each); a 128-bit hash makes a collision - which would merely merge two
		// distinct callstacks under one id - astronomically unlikely at millions of stacks.
		struct callstack_hash
		{
			uint64_t h0, h1;
			bool operator==(const callstack_hash& o) const { return h0 == o.h0 && h1 == o.h1; }
		};
		struct callstack_hash_hasher
		{
			size_t operator()(const callstack_hash& k) const { return (size_t)(k.h0 ^ (k.h1 * 1099511628211ULL)); }
		};
		// Callstacks already reported to client, keyed by the 128-bit hash. Nodes come from the
		// profiler's private heap (millions of them).
		std::unordered_map<callstack_hash, callstack_entry, callstack_hash_hasher, std::equal_to<callstack_hash>,
			profiler_allocator<std::pair<const callstack_hash, callstack_entry>>> m_callstack_ids;
		uint32_t m_next_callstack_id = 0;

		// Frame lines interned by text: the same "Class.Method" / "Module.dll+0xRVA" line
		// appears in thousands of callstacks, but is sent to the client only once (as
		// SRV_FRAME) and thereafter referenced by id. There are only ~tens of thousands of
		// unique lines, versus millions of unique callstacks.
		std::unordered_map<std::string, uint32_t> m_frame_line_ids;
		uint32_t m_next_frame_id = 0;
		// Reused buffer for building a callstack's frame-id sequence (avoids an allocation
		// per first-seen callstack).
		std::vector<uint32_t> m_scratch_frame_ids;

		// Highest frame seen in dequeued items so far; used to keep reported frames monotonic
		uint64_t m_max_seen_frame = 0;
		// True if we already logged that some callstack was truncated
		bool m_overflow_logged = false;
		// Logger, owned by mono_profiler
		logger* m_logger = nullptr;

		// True if backtraces contain raw instruction pointers captured natively, instead
		// of MonoMethod* pointers from mono_stack_walk (decided once at profiler start,
		// see mono_profiler::details::choose_backtrace_mode)
		bool m_capture_raw_ips = false;
		// True if the Mono jit-info lookup functions were resolved and are safe to call
		// from resolve_ip. False for IL2CPP, or for native-only when Mono functions are
		// unavailable - in which case raw-IP frames resolve as module+offset only.
		bool m_jit_available = false;

#if defined(WIN32)
		// A resolved instruction pointer: either a managed method line, or a native module+offset line
		struct ip_entry
		{
			std::string text;
			bool stopword = false;
			// True for addresses inside the profiler or the Mono runtime itself. Such frames
			// at the top of a stack are the allocation machinery, and are skipped.
			bool runtime_internal = false;
			// True if the address resolved to a managed method
			bool managed = false;
			// Interned id of this frame line (see intern_frame_line)
			uint32_t frame_id = 0;
		};
		std::unordered_map<void*, ip_entry> m_ip_cache;

		// The domain used for jit info lookups. Captured on the game's threads (which are
		// attached to the runtime), used on the worker thread.
		std::atomic<void*> m_jit_domain{nullptr};

		// Statistics to diagnose broken native unwinding (logged periodically):
		// if unwinding can't cross jit code, no callstack will contain managed frames,
		// and the average frame count will be very low
		uint64_t m_unique_ip_stacks = 0;
		uint64_t m_unique_ip_stacks_with_managed = 0;
		uint64_t m_unique_ip_frames = 0;
#endif

	private:
		// Main processing function
		void do_work();

		// Back-pressure. maybe_update_throttle (worker thread) sets/clears m_send_throttle from
		// the send-buffer and queue sizes; wait_if_throttled (game threads) blocks while it's set.
		void maybe_update_throttle();
		void wait_if_throttled();

#if defined(OWLCAT_PROFILER_MEMLOG)
		// Logs the sizes of the profiler's containers, at most once per interval. Called
		// from the worker loop, so it reads the worker-owned containers with no locking.
		void maybe_log_memory_stats();
		// Last time stats were logged; and a loop counter so the clock is only read
		// occasionally (not on every processed event during a storm).
		std::chrono::steady_clock::time_point m_last_memlog{};
		uint32_t m_memlog_counter = 0;

		// Pseudo-GC accounting captured at the last collection (post-sweep), to track how much
		// the conservative mark over-retains versus what BoehmGC actually keeps. Measured right
		// after the collection so it's directly comparable to the GC's used size at that instant
		// (unlike the MEMLOG snapshot, which also counts objects allocated since the last GC).
		uint64_t m_last_gc_kept_bytes = 0;
		uint64_t m_last_gc_kept_count = 0;
		uint64_t m_last_gc_freed_bytes = 0;
		uint64_t m_last_gc_freed_count = 0;
		int64_t m_last_gc_used_bytes = -1; // GC's own used size at that collection; -1 if unavailable
#endif

		// Resolves a method name via Mono functions, caching the result by method pointer
		const method_entry& resolve_method(MonoMethod* method);
#if defined(WIN32)
		// Resolves a raw instruction pointer to a managed method or a native module+offset,
		// caching the result by address
		const ip_entry& resolve_ip(void* ip);
#endif
		// Returns an id for the type, reporting a definition to the client on first sight
		uint32_t intern_type(MonoClass* klass);
		// Returns an id for a native allocation "type" (a hook label), minting + reporting on first use
		uint32_t intern_native_type(uint32_t label_index);
		// Returns an id for a single callstack frame line, reporting its definition (SRV_FRAME)
		// to the client on first sight. Interned by text, so identical lines share an id.
		uint32_t intern_frame_line(const std::string& text);
		// Returns interned info for a callstack, reporting a definition to the client on first sight
		callstack_entry intern_callstack(const stack_backtrace& backtrace);

		/*
			This function performs pseoud-GC on our list of allocations to mark all live objects
			and report all dead ones
		*/
		int do_gc_internal(uint64_t frame, bool only_update_parents);
		/*
			An attempt to use Unity's built-in functions to calculate liveness of objects. Doesn't work, but
			needs to be examined more closely.
		*/
		void do_gc_unity(uint64_t frame);

		/*
			Finds references to the specified list of objects and reports them via events sink
		*/
		void find_references_internal(uint64_t request_id, const std::vector<uint64_t>& addresses);

	public:
		worker_thread(events_sink* sink, logger* log, bool capture_raw_ips, bool jit_available);
		~worker_thread();		

		// Starts the thread
		void start();
		// Signals the thread to stop
		void stop();

		// Adds allocation event to work queue
		void add_allocation_async(uint64_t frame, MonoClass* klass, MonoObject* obj);
		// Sets the display labels for native allocation types (one per configured hook).
		// Must be called before any native events are enqueued.
		void set_native_types(const std::vector<std::string>& labels);
		// Adds a native allocation event (from a hooked allocator). Captures the callstack.
		void add_native_allocation(uint64_t frame, uint64_t addr, uint32_t size, uint32_t label_index);
		// Adds a native free event (from a hooked free). Size is recovered from m_native_allocations.
		void add_native_free(uint64_t frame, uint64_t addr);
		// Performs pseudo-GC operation, blocking the calling trhead. Reports free events.
		int do_gc_sync(uint64_t frame, bool only_update_parents);
		// Registers a GC root
		void register_root(const char* start, uint64_t size);
		// Unregisters a GC root
		void unregister_root(const char* start);
		// Finds references to the specified list of objects and reports them via events sink
		void find_references(uint64_t request_id, const std::vector<uint64_t>& addresses, uint64_t frame);
		// Pauses the profiled app
		void pause_app(uint64_t request_id);
		// Unpauses the profiled app
		void resume_app(uint64_t request_id);
		// Checks if the app is paused
		bool is_paused() const;
	};
}
