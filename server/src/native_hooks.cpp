#include "native_hooks.h"
#include "worker_thread.h"
#include "logger.h"
#include "profiler_thread.h"

#include <Windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include "detours.h"

#include <vector>
#include <string>
#include <sstream>
#include <utility>
#include <cctype>
#include <cstdlib>

/*
	Config file format (one hook per line, '|'-separated, '#' starts a comment):

	    module | target | role | argmap | label

	- module : DLL the function lives in, e.g. UnityPlayer.dll
	- target : decorated function name (resolved via the module's PDB, which the profiler
	           already requires) OR a relative virtual address as +0xHEX from module base
	- role   : alloc | realloc | free
	- argmap : comma-separated key=value; keys size/ptr/oldptr/if/plane. Value slots are
	           a1..a4 (the 1st..4th integer/pointer argument, i.e. RCX,RDX,R8,R9 on x64) or
	           'ret' (return value). For member functions 'this' is a1, so a real size is
	           usually a2. The size may also be a product of two args ("size=a1*a2") for
	           calloc-style allocators whose size is count*element_size.
	             if=aN&MASK   : record only when argument aN has any bit of MASK set (hex 0x..
	                            or decimal). E.g. VirtualAlloc only on MEM_COMMIT (a3&0x1000),
	                            VirtualFree only on MEM_DECOMMIT|MEM_RELEASE (a3&0xC000). This
	                            tracks committed physical memory instead of reserved address space.
	             plane=reserve: put this hook on the reservation plane (page commits) instead of
	                            the default allocation plane. Reservation hooks are NOT suppressed
	                            when they fire inside an allocation-plane hook (the usual case for
	                            VirtualAlloc, called from Unity's allocators), so committed pages
	                            and handed-out allocations are tracked at the same time. NOTE: when
	                            both planes are hooked they overlap the same physical memory, so
	                            read them per-type - don't sum the grand total.
	- label  : quoted display name, used as the allocation "type" in the UI

	Example:
	    UnityPlayer.dll | ?AllocInternal@... | alloc   | size=a2, ptr=ret            | "Unity Alloc"
	    UnityPlayer.dll | ?ReallocInternal@..| realloc | oldptr=a2, size=a3, ptr=ret | "Unity Realloc"
	    UnityPlayer.dll | ?FreeInternal@...  | free    | ptr=a2                      | "Unity Free"
	    ucrtbase.dll    | calloc             | alloc   | size=a1*a2, ptr=ret         | "CRT calloc"
	    kernel32.dll    | VirtualAlloc       | alloc   | size=a2, ptr=ret, if=a3&0x1000, plane=reserve | "VirtualAlloc (commit)"
	    kernel32.dll    | VirtualFree        | free    | ptr=a1, if=a3&0xC000, plane=reserve          | "VirtualFree"

	Limitations (x64): the target must take at most 4 integer/pointer arguments (in
	registers) and, for alloc/realloc, return the pointer in RAX. Functions with more
	arguments, by-value struct/float arguments, or an out-parameter pointer are not
	supported. Max MAX_HOOKS hooks.
*/

namespace owlcat
{
	static const int MAX_HOOKS = 128;

	// A generic x64 detour: any integer/pointer function of up to 4 args maps onto this.
	typedef void* (*generic_fn)(void*, void*, void*, void*);

	struct hook_desc
	{
		enum role_t { ALLOC, REALLOC, FREE };

		// The trampoline to the original function. DetourAttach rewrites this from the
		// resolved target address to the trampoline. Must be the first member so
		// &hook_desc == &original for DetourAttach.
		void* original = nullptr;
		role_t role = ALLOC;
		// Argument slot holding each value: 1..4 for a1..a4, 0 for the return value, -1 = none
		int size_arg = -1;
		// Optional second size arg: when set, the size is size_arg * size_arg2
		// (for calloc(count, size), whose allocation size is the product).
		int size_arg2 = -1;
		int ptr_arg = 0;      // where the (new) pointer comes from; 0 = return value
		int oldptr_arg = -1;  // realloc: the freed pointer
		uint32_t label_index = 0;

		// Optional flag predicate ("if=aN&MASK"): only record when args[flag_arg] has any of
		// flag_mask's bits set. Used to record VirtualAlloc only on MEM_COMMIT and VirtualFree
		// only on MEM_DECOMMIT|MEM_RELEASE, i.e. committed memory rather than reserved address
		// space. flag_mask == 0 means no filter (always record).
		int flag_arg = -1;
		uint64_t flag_mask = 0;

		// Which tracking plane this hook belongs to (see profiler_thread.h). false = allocation
		// plane (default); true = reservation plane (VirtualAlloc pages), which is NOT suppressed
		// when it fires inside an allocation-plane hook, so committed pages and handed-out
		// allocations are tracked independently.
		bool reservation = false;
	};

	// Process-wide hook state (there is one profiler per process)
	static hook_desc g_hooks[MAX_HOOKS];
	static generic_fn g_stub_table[MAX_HOOKS];
	static worker_thread* g_worker = nullptr;
	static const std::atomic<uint64_t>* g_frame = nullptr;

	// The module's TLS index, emitted by the compiler because we use thread_local.
	extern "C" unsigned long _tls_index;

	// True once the current thread's TLS storage (and this module's block within it) exists,
	// so that reading a thread_local is safe. A hooked heap function can be called by the OS
	// loader WHILE it builds a new thread's TLS vector (LdrpAllocateTls); at that point the
	// thread_local does not exist yet and touching it faults. TEB.ThreadLocalStoragePointer
	// is at gs:[0x58] on x64.
	static inline bool thread_tls_ready()
	{
		void** tls_vector = (void**)__readgsqword(0x58);
		return tls_vector != nullptr && tls_vector[_tls_index] != nullptr;
	}

	static void* dispatch(int index, void* a1, void* a2, void* a3, void* a4)
	{
		hook_desc& h = g_hooks[index];
		generic_fn original = (generic_fn)h.original;

		// If this thread's TLS isn't set up yet, we're being called from inside the loader
		// initializing a new thread (via a hooked heap allocator). We cannot touch any
		// thread_local here; forward without recording (these allocations aren't interesting).
		if (!thread_tls_ready() || g_worker == nullptr)
			return original(a1, a2, a3, a4);

		// The profiler's own threads and its recording path must never be recorded, on either
		// plane (reentrancy + no self-profiling). This trumps everything below.
		if (t_profiler_internal_thread)
			return original(a1, a2, a3, a4);

		// Within a plane, attribute nested allocations to the outermost hooked frame. The two
		// planes are independent, so a reservation (VirtualAlloc) made inside an allocation-plane
		// hook is still recorded - that's how committed pages and handed-out allocations coexist.
		bool& plane_flag = h.reservation ? t_profiler_in_reservation_hook : t_profiler_in_alloc_hook;
		if (plane_flag)
			return original(a1, a2, a3, a4);

		// args[1..4] = a1..a4; args[0] unused (0 means 'return value')
		void* args[5] = { nullptr, a1, a2, a3, a4 };

		// Optional flag predicate: only record when the chosen arg has the required bits (e.g.
		// VirtualAlloc only on MEM_COMMIT). The real call always proceeds; we just skip recording.
		if (h.flag_mask != 0)
		{
			uint64_t flags = (h.flag_arg >= 1 && h.flag_arg <= 4) ? (uint64_t)args[h.flag_arg] : 0;
			if ((flags & h.flag_mask) == 0)
				return original(a1, a2, a3, a4);
		}

		// Read the inputs BEFORE calling the original (realloc may free/overwrite them)
		void* oldptr = (h.oldptr_arg >= 1 && h.oldptr_arg <= 4) ? args[h.oldptr_arg] : nullptr;
		void* freeptr = (h.role == hook_desc::FREE && h.ptr_arg >= 1 && h.ptr_arg <= 4) ? args[h.ptr_arg] : nullptr;
		uint32_t size = 0;
		if (h.size_arg >= 1 && h.size_arg <= 4)
		{
			uint64_t s = (uint64_t)args[h.size_arg];
			if (h.size_arg2 >= 1 && h.size_arg2 <= 4) // e.g. calloc: size = count * elem_size
				s *= (uint64_t)args[h.size_arg2];
			size = (uint32_t)s;
		}

		// Run the real function with this plane's reentrancy flag set: nested same-plane hooks
		// attribute to this frame, while the other plane still records.
		void* ret;
		{
			flag_scope plane(plane_flag);
			ret = original(a1, a2, a3, a4);
		}

		// Record with t_profiler_internal_thread set, so the recording's own allocations
		// (callstack capture, the work queue) are not themselves recorded on either plane.
		profiler_internal_scope scope;
		const uint64_t frame = g_frame->load(std::memory_order_relaxed);

		switch (h.role)
		{
		case hook_desc::ALLOC:
		{
			void* p = (h.ptr_arg == 0) ? ret : ((h.ptr_arg >= 1 && h.ptr_arg <= 4) ? args[h.ptr_arg] : nullptr);
			if (p != nullptr)
				g_worker->add_native_allocation(frame, (uint64_t)p, size, h.label_index);
			break;
		}
		case hook_desc::FREE:
			if (freeptr != nullptr)
				g_worker->add_native_free(frame, (uint64_t)freeptr);
			break;
		case hook_desc::REALLOC:
		{
			void* p = (h.ptr_arg == 0) ? ret : ((h.ptr_arg >= 1 && h.ptr_arg <= 4) ? args[h.ptr_arg] : nullptr);
			// realloc(ptr, 0) frees; realloc(nullptr, n) allocates; otherwise free old + alloc new
			if (oldptr != nullptr)
				g_worker->add_native_free(frame, (uint64_t)oldptr);
			if (p != nullptr && size != 0)
				g_worker->add_native_allocation(frame, (uint64_t)p, size, h.label_index);
			break;
		}
		}

		return ret;
	}

	template<int I>
	static void* generic_stub(void* a1, void* a2, void* a3, void* a4)
	{
		return dispatch(I, a1, a2, a3, a4);
	}

	template<int... Is>
	static void build_stub_table(std::index_sequence<Is...>)
	{
		((g_stub_table[Is] = &generic_stub<Is>), ...);
	}

	// ---------------- config parsing ----------------

	static std::string trim(const std::string& s)
	{
		size_t b = 0, e = s.size();
		while (b < e && isspace((unsigned char)s[b])) ++b;
		while (e > b && isspace((unsigned char)s[e - 1])) --e;
		return s.substr(b, e - b);
	}

	// Maps an argmap value (a1..a4 / ret) to a slot: 1..4, or 0 for 'ret', or -1 if invalid
	static int parse_arg_slot(const std::string& v)
	{
		std::string s = trim(v);
		if (s == "ret")
			return 0;
		if (s.size() == 2 && s[0] == 'a' && s[1] >= '1' && s[1] <= '4')
			return s[1] - '0';
		return -1;
	}

	struct parsed_hook
	{
		std::string module;
		std::string target;
		hook_desc desc;
		std::string label;
	};

	// Parses one "size=a2, ptr=ret" argmap into the descriptor. The size value may also be
	// a product of two args ("size=a1*a2") for allocators like calloc(count, size).
	static void parse_argmap(const std::string& argmap, hook_desc& desc)
	{
		std::stringstream ss(argmap);
		std::string pair;
		while (std::getline(ss, pair, ','))
		{
			auto eq = pair.find('=');
			if (eq == std::string::npos)
				continue;
			std::string key = trim(pair.substr(0, eq));
			std::string val = pair.substr(eq + 1);

			if (key == "size")
			{
				auto star = val.find('*');
				if (star != std::string::npos)
				{
					desc.size_arg = parse_arg_slot(val.substr(0, star));
					desc.size_arg2 = parse_arg_slot(val.substr(star + 1));
				}
				else
					desc.size_arg = parse_arg_slot(val);
			}
			else if (key == "ptr") // may legitimately be 'ret' (slot 0)
				desc.ptr_arg = parse_arg_slot(val);
			else if (key == "oldptr")
				desc.oldptr_arg = parse_arg_slot(val);
			else if (key == "if") // flag predicate: "aN&MASK" (MASK is hex 0x.. or decimal)
			{
				auto amp = val.find('&');
				if (amp != std::string::npos)
				{
					int fa = parse_arg_slot(val.substr(0, amp));
					if (fa >= 1 && fa <= 4)
					{
						desc.flag_arg = fa;
						desc.flag_mask = strtoull(trim(val.substr(amp + 1)).c_str(), nullptr, 0);
					}
				}
			}
			else if (key == "plane")
			{
				std::string p = trim(val);
				for (auto& c : p) c = (char)tolower((unsigned char)c);
				desc.reservation = (p == "reserve" || p == "reservation");
			}
		}
	}

	static bool parse_line(const std::string& line, parsed_hook& out)
	{
		// Split into up to 5 '|' fields
		std::vector<std::string> fields;
		std::stringstream ss(line);
		std::string field;
		while (std::getline(ss, field, '|'))
			fields.push_back(trim(field));
		if (fields.size() < 5)
			return false;

		out.module = fields[0];
		out.target = fields[1];

		std::string role = fields[2];
		for (auto& c : role) c = (char)tolower((unsigned char)c);
		if (role == "alloc") out.desc.role = hook_desc::ALLOC;
		else if (role == "realloc") out.desc.role = hook_desc::REALLOC;
		else if (role == "free") out.desc.role = hook_desc::FREE;
		else return false;

		// Defaults, then override from argmap
		out.desc.size_arg = -1;
		out.desc.size_arg2 = -1;
		out.desc.ptr_arg = (out.desc.role == hook_desc::FREE) ? -1 : 0; // alloc/realloc default ptr=ret
		out.desc.oldptr_arg = -1;
		out.desc.flag_arg = -1;
		out.desc.flag_mask = 0;
		out.desc.reservation = false;
		parse_argmap(fields[3], out.desc);

		std::string label = fields[4];
		if (label.size() >= 2 && label.front() == '"' && label.back() == '"')
			label = label.substr(1, label.size() - 2);
		out.label = label;

		return true;
	}

	// Resolves a target to an address: either a decorated name via the module's symbols,
	// or module base + RVA if given as "+0xHEX"
	static void* resolve_target(const std::string& module, const std::string& target)
	{
		if (!target.empty() && target[0] == '+')
		{
			HMODULE m = GetModuleHandleA(module.c_str());
			if (m == nullptr)
				return nullptr;
			uint64_t rva = strtoull(target.c_str() + 1, nullptr, 16);
			return (char*)m + rva;
		}

		// DetourFindFunction resolves non-exported functions through the module's PDB
		return DetourFindFunction(module.c_str(), target.c_str());
	}

	native_hooks::native_hooks(worker_thread* worker, const std::atomic<uint64_t>* frame, logger* log)
	{
		g_worker = worker;
		g_frame = frame;
		m_log = log;
	}

	native_hooks::~native_hooks()
	{
		// Hooks stay for the life of the process (the game is exiting anyway); no detach.
	}

	void native_hooks::log(const char* msg)
	{
		if (m_log != nullptr)
			m_log->log_str(msg);
	}

	int native_hooks::install(const std::string& config_text)
	{
		std::istringstream file(config_text);

		std::vector<parsed_hook> hooks;
		std::string line;
		while (std::getline(file, line))
		{
			std::string trimmed = trim(line);
			if (trimmed.empty() || trimmed[0] == '#')
				continue;
			parsed_hook h;
			if (parse_line(trimmed, h))
			{
				if ((int)hooks.size() >= MAX_HOOKS)
					break;
				h.desc.label_index = (uint32_t)hooks.size();
				hooks.push_back(h);
			}
		}

		if (hooks.empty())
			return 0;

		build_stub_table(std::make_index_sequence<MAX_HOOKS>{});

		// Register every hook's label as a native type (index-aligned with hooks), so the
		// worker can name native allocations even for hooks that fail to resolve
		m_labels.clear();
		m_labels.reserve(hooks.size());
		for (auto& h : hooks)
			m_labels.push_back(h.label);
		g_worker->set_native_types(m_labels);

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		// Suspend every other thread and register it with the transaction. Unlike the
		// early UnityPlayer hooks in dll_main (installed before the game runs), native
		// hooks can be installed on a running process (the Editor), where other threads
		// may be executing exactly the code we are patching. Detours rewrites their
		// contexts if their IP lands in a patched region; they must be suspended first.
		std::vector<HANDLE> suspended;
		const DWORD my_pid = GetCurrentProcessId();
		const DWORD my_tid = GetCurrentThreadId();
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te = {};
			te.dwSize = sizeof(te);
			if (Thread32First(snapshot, &te))
			{
				do
				{
					if (te.th32OwnerProcessID != my_pid || te.th32ThreadID == my_tid)
						continue;
					HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
					if (h != nullptr)
					{
						if (SuspendThread(h) != (DWORD)-1)
						{
							DetourUpdateThread(h);
							suspended.push_back(h);
						}
						else
							CloseHandle(h);
					}
				} while (Thread32Next(snapshot, &te));
			}
			CloseHandle(snapshot);
		}

		int installed = 0;
		for (size_t i = 0; i < hooks.size(); ++i)
		{
			void* target = resolve_target(hooks[i].module, hooks[i].target);
			if (target == nullptr)
			{
				// Unresolved: skip (its label still exists, it just never fires). Most likely
				// the module isn't loaded, or the name can't be found without its PDB.
				log((std::string("Native hooks: could not resolve ") + hooks[i].module + " / " + hooks[i].target).c_str());
				continue;
			}

			g_hooks[i] = hooks[i].desc;
			g_hooks[i].original = target;
			if (DetourAttach(&g_hooks[i].original, (PVOID)g_stub_table[i]) == NO_ERROR)
				++installed;
			else
			{
				g_hooks[i].original = nullptr;
				log((std::string("Native hooks: DetourAttach failed for ") + hooks[i].target).c_str());
			}
		}

		LONG commit_result = DetourTransactionCommit();

		// Resume the threads we suspended, whatever the outcome
		for (HANDLE h : suspended)
		{
			ResumeThread(h);
			CloseHandle(h);
		}

		if (commit_result != NO_ERROR)
		{
			log("Native hooks: DetourTransactionCommit failed");
			return 0;
		}

		char summary[128];
		snprintf(summary, sizeof(summary) - 1, "Native hooks: installed %d of %zu configured hooks", installed, hooks.size());
		log(summary);

		m_installed = installed > 0;
		return installed;
	}

	void native_hooks::rebind(worker_thread* worker)
	{
		// Hooks are already attached and dispatch through g_worker; just repoint it and
		// re-register the labels on the new worker.
		g_worker = worker;
		if (worker != nullptr && !m_labels.empty())
			worker->set_native_types(m_labels);
	}
}
