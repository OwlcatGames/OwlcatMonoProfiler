#pragma once

#include <string>
#include <vector>

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
		// Symbol resolution status of one module referenced by callstacks
		struct module_symbols_info
		{
			enum class status_t
			{
				pending,  // module referenced, but no frame resolved yet (load is deferred)
				loaded,   // PDB or other debug info found and parsed
				mismatch, // only a wrong-version PDB was found; its names are NOT used
				exports,  // no PDB; function names come from the DLL export table only
				none,     // no symbols found in the search path
				failed,   // DbgHelp could not register the module at all
			};

			std::string name;        // e.g. "UnityPlayer.dll"
			status_t status;
			std::string symbol_file; // path of the loaded PDB (or image, for exports), if any
		};

		// Identity of a module in the target process, as reported by the profiler
		// server: where the module was loaded from and which exact PDB matches it
		// (RSDS signature from the module's debug directory).
		struct module_identity
		{
			std::string path;      // full path of the module in the target process
			uint8_t pdb_guid[16] = {};
			uint32_t pdb_age = 0;
			std::string pdb_name;  // build-time pdb path from the RSDS record

			// False if the module has no debug directory (all-zero GUID)
			bool has_signature() const
			{
				for (auto b : pdb_guid)
					if (b != 0)
						return true;
				return false;
			}
		};

	public:
		symbol_resolver();
		~symbol_resolver();

		// Sets the ';'-separated symbol search path (directories with PDBs/DLLs) and
		// forgets any modules loaded so far, so they reload from the new path.
		// Module identities are kept: they describe the target process, not the path.
		void set_search_path(const std::string& path);

		// Sets (or updates) a module's identity. When known, symbol loading prefers the
		// exact PDB matching the module's signature (found via GUID+age validation on
		// the search path) over today's first-name-match search, so several versions of
		// the same module's PDB can coexist on the path. If the module was already
		// loaded with a different identity, it is dropped and reloads lazily.
		void set_module_identity(const std::string& module_name, const module_identity& identity);

		// Returns the callstack text with its native frames symbolicated. If a module's
		// symbols can't be found, its frames are left as "Module.dll+0xRVA".
		std::string symbolicate(const std::string& raw);

		// Modules referenced by symbolicated callstacks so far whose symbols could NOT
		// be loaded from the current search path. Lets callers report exactly which
		// PDBs are missing. Reset by set_search_path.
		std::vector<std::string> get_unresolved_modules() const;

		// Symbol resolution status of every module referenced by symbolicated
		// callstacks so far. Reset by set_search_path.
		std::vector<module_symbols_info> get_modules() const;

	private:
		struct impl;
		impl* m_impl;
	};
}
