#pragma once

namespace owlcat
{
	/*
		True while the current thread is executing the profiler's own code. Native
		allocation hooks (native_hooks::dispatch) skip recording when this is set, for
		two reasons:

		1. The profiler's own threads (worker, network, command, watchdog) allocate
		   constantly. If a hooked allocator (e.g. the CRT the profiler itself uses)
		   recorded those, it would be noise - and, on the worker thread, a feedback
		   loop (processing an event allocates, which would generate another event).

		2. During recording of one allocation, our code allocates; those nested calls to
		   hooked allocators must not be recorded, so that an allocation is attributed to
		   the outermost hooked function (e.g. Unity's allocator, not the CRT malloc it
		   calls internally).

		Set it for a thread's whole lifetime on profiler-owned threads, or use
		profiler_internal_scope around profiler code that runs transiently on a game
		thread (the Mono allocation/GC/root callbacks).

		inline thread_local: one instance per thread, shared across all translation units.
	*/
	inline thread_local bool t_profiler_internal_thread = false;

	struct profiler_internal_scope
	{
		bool m_prev;
		profiler_internal_scope() : m_prev(t_profiler_internal_thread) { t_profiler_internal_thread = true; }
		~profiler_internal_scope() { t_profiler_internal_thread = m_prev; }
	};

	/*
		Per-plane reentrancy for native hooks. Hooks belong to one of two independent planes:

		- allocation plane (malloc / operator new / Unity's DynamicHeapAllocator / ...): tracks
		  memory HANDED OUT to the program;
		- reservation plane (VirtualAlloc pages): tracks memory COMMITTED from the OS.

		Within a plane, an allocation is attributed to the OUTERMOST hooked frame (t_profiler_in_*
		is set while a plane's target runs, so nested same-plane hooks are suppressed). ACROSS
		planes there is no suppression: a page reservation made *inside* an allocator - the normal
		case - is still recorded, so committed pages and handed-out allocations can be seen at the
		same time without one hiding the other. (t_profiler_internal_thread still trumps both: the
		profiler's own recording allocations are never recorded on either plane.)
	*/
	inline thread_local bool t_profiler_in_alloc_hook = false;
	inline thread_local bool t_profiler_in_reservation_hook = false;

	// RAII save/restore for one of the plane flags (nesting-safe).
	struct flag_scope
	{
		bool& m_flag;
		bool m_prev;
		flag_scope(bool& f) : m_flag(f), m_prev(f) { m_flag = true; }
		~flag_scope() { m_flag = m_prev; }
	};
}
