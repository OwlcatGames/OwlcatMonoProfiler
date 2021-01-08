#pragma once

#include "mono\metadata\profiler.h"
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <concurrentqueue.h>
//#include "tsl/robin_map.h"

//#define DEBUG_ALLOCS

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

		// Total size of allocated and deallocated memory
		uint64_t m_allocated = 0;
		uint64_t m_freed = 0;

		/*
			Callstack storage
		*/
		struct stack_backtrace
		{
			stack_backtrace()
			{
				backtrace.reserve(32);
			}

			mono_bool add_trace(MonoMethod* method, int32_t native_offset, int32_t il_offset, mono_bool managed);

			std::vector<MonoMethod*> backtrace;
			bool stopword_met = false;
		};

		enum class work_item_type : uint8_t {alloc, free};
		/*
			A work item to be processed by worker thread
		*/
		struct work_item
		{
			// Frame when event happened
			uint64_t frame;
			// Class of allocated object (nullptr for other events)
			MonoClass* klass = 0;
			// Allocated object itself (nullptr for other events)
			MonoObject* obj = 0;
			// Size of allocated or freed object
			uint32_t size;
			// Callstack at which object was allocated (empty for other events)
			stack_backtrace backtrace;
			// Type of event
			work_item_type type;
		};

		/*
			Concurrent queue of items to be processed
		*/
		moodycamel::ConcurrentQueue<work_item> m_work_items;
		/*
			Token use to make events reported from different treads appear in correct sequence
			(see "High-level design" section of https://github.com/cameron314/concurrentqueue)
		*/
		moodycamel::ProducerToken m_work_items_token;
		/*
			ConcurrentQueue doesn't support any way to check if it is empty, so here's a bool
			that the worker thread sets when it failed to dequeue an item.
		*/
		bool m_work_items_empty = true;
		/*
			A pointer to a sink used to report events to client
		*/
		events_sink* m_events_sink;
		/*
			When true, signals the thread to stop the loop
		*/
		bool m_stop = false;
		/*
			Last frame when GC was performed. When client wants to find references to an object,
			we check this variable and the current frame to see if list of parents needs to be
			updated
		*/
		uint64_t m_last_gc_frame = 0;

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
			Information about a single allocation
		*/
		struct alloc_info
		{
			// Allocation size
			uint32_t size;
			// A set of flags, temporary and permanent for this allocation
			uint8_t flags;
			// List of objects that refer to this allocation.
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
				IGNORE_IN_GC  = 1 << 2,
			};

			void set_flag(flag f) { flags |= (uint8_t)f; }
			void reset_flag(flag f) { flags &= ~(uint8_t)f; }
			bool flag(flag f) { return (flags & (uint8_t)f) != 0; }
		};

		//using allocations_map = tsl::sparse_map<uint64_t, alloc_info, std::hash<uint64_t>, std::equal_to<uint64_t>, counting_allocator<std::pair<uint64_t, alloc_info>>>;
		//using allocations_map = tsl::robin_map<uint64_t, alloc_info>;
		using allocations_map = std::unordered_map<uint64_t, alloc_info>;
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

	private:
		// Main processing function
		void do_work();

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
		worker_thread(events_sink* sink);
		~worker_thread();		

		// Starts the thread
		void start();
		// Signals the thread to stop
		void stop();

		// Adds allocation event to work queue
		void add_allocation_async(uint64_t frame, MonoClass* klass, MonoObject* obj);
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
