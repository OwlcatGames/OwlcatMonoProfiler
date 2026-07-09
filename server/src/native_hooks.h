#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

namespace owlcat
{
	class worker_thread;
	class logger;

	/*
		Installs Detours hooks on user-specified native allocation functions (from a config
		file) and routes their alloc/free/realloc events into the worker thread as native
		events. See native_hooks.cpp for the config file format and the generic hooking
		mechanism.

		There is one instance per process (the hook dispatch uses process-wide state).
	*/
	class native_hooks
	{
	public:
		// frame points to the profiler's current frame counter; log may be null.
		native_hooks(worker_thread* worker, const std::atomic<uint64_t>* frame, logger* log);
		~native_hooks();

		// Parses the config (the text content of a hook config file, delivered by the
		// client over the connection) and installs the hooks. Returns the number of hooks
		// successfully installed (0 if empty/unparseable or nothing resolved). Other
		// threads are suspended during installation, so this is safe to call on a running
		// process (e.g. the Editor).
		int install(const std::string& config_text);

		// Points the (already installed) hooks at a new worker and re-registers the native
		// type labels on it. Used when the worker is recreated on a profiling restart.
		void rebind(worker_thread* worker);

	private:
		void log(const char* msg);

		logger* m_log = nullptr;
		bool m_installed = false;
		// Native type labels (one per configured hook), kept so they can be re-pushed to a
		// new worker on rebind
		std::vector<std::string> m_labels;
	};
}
