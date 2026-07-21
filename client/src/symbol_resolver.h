#pragma once

#include <string>

namespace owlcat
{
	/*
		Resolves native "Module.dll+0xRVA" frames in a callstack text into
		"Module.dll!Function+0xNN (file:line)" using DbgHelp against local PDBs/DLLs
		found in a configured search path.

		Managed frames (already "Namespace.Class.Method") and markers ("<no stack>")
		contain no "+0x" token and pass through unchanged.

		Not thread-safe: a single owner thread (the client's symbolication thread) must
		perform all calls, including set_search_path. PDBs are loaded lazily on first
		reference to a module (the loading cost falls on that thread only).
	*/
	class symbol_resolver
	{
	public:
		symbol_resolver();
		~symbol_resolver();

		// Sets the ';'-separated symbol search path (directories with PDBs/DLLs) and
		// forgets any modules loaded so far, so they reload from the new path.
		void set_search_path(const std::string& path);

		// Returns the callstack text with its native frames symbolicated. If a module's
		// symbols can't be found, its frames are left as "Module.dll+0xRVA".
		std::string symbolicate(const std::string& raw);

	private:
		struct impl;
		impl* m_impl;
	};
}
