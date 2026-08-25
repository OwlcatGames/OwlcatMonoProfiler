#include "symbol_resolver.h"

#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#if defined(WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace owlcat
{
#if defined(WIN32)

	// SymFindFileInPath validates the id/two/three signature only against symbol-server
	// (srv*) path elements; a candidate from a plain directory is a NAME match only, and
	// without a callback the search stops at the first one. This callback checks every
	// candidate's real signature: returning TRUE means "not it, keep searching", FALSE
	// accepts the candidate (SymFindFileInPath then returns it).
	static BOOL CALLBACK find_matching_pdb_callback(PCSTR filename, PVOID context)
	{
		const symbol_resolver::module_identity* id = (const symbol_resolver::module_identity*)context;

		SYMSRV_INDEX_INFO info = {};
		info.sizeofstruct = sizeof(info);
		if (!SymSrvGetFileIndexInfo(filename, &info, 0))
			return TRUE; // unreadable or not a PDB - keep searching

		if (memcmp(&info.guid, id->pdb_guid, sizeof(id->pdb_guid)) != 0 || info.age != id->pdb_age)
			return TRUE; // a different build's PDB - keep searching

		return FALSE; // exact match - stop here
	}

	struct symbol_resolver::impl
	{
		// A unique, fake "process" handle keying this DbgHelp session (we are not the
		// target process; modules are loaded manually at synthetic bases).
		HANDLE process = nullptr;
		bool initialized = false;
		std::string search_path;

		// Per referenced module: its synthetic load base (0 if registration failed),
		// and whether actual symbols turned out to be available. With
		// SYMOPT_DEFERRED_LOADS a missing PDB is NOT detected at SymLoadModuleEx -
		// the load "succeeds" and the failure only surfaces at the first SymFromAddr,
		// so has_symbols is determined there (checked flags that determination).
		struct module_state
		{
			uint64_t base = 0;
			bool checked = false;
			bool has_symbols = false;
			module_symbols_info::status_t status = module_symbols_info::status_t::pending;
			std::string symbol_file;
		};
		std::unordered_map<std::string, module_state> modules;
		// Module identities reported by the profiler server (or loaded from a capture),
		// keyed by the module name as it appears in frame lines
		std::unordered_map<std::string, module_identity> identities;
		// Synthetic address space per module. We register each module with an explicit,
		// generously-large span (rather than letting DbgHelp derive the size from the image,
		// which we may not have) so that base+rva always lands inside the module's range.
		// The stride between bases is larger than the span, so ranges never overlap even for
		// a huge GameAssembly.dll.
		static const uint64_t module_span = 0x80000000ULL;   // 2 GB
		static const uint64_t module_stride = 0x100000000ULL; // 4 GB
		uint64_t next_base = module_stride;

		// Loads (once) the symbols for a module and returns its state entry.
		module_state& get_module(const std::string& name)
		{
			auto it = modules.find(name);
			if (it != modules.end())
				return it->second;

			uint64_t base = next_base;
			next_base += module_stride;

			// When the module's identity is known, don't trust DbgHelp's name-only search:
			// with several same-named PDBs on the path (e.g. two Unity versions) it takes
			// the first name match. Instead find the exact PDB by signature, or load via
			// the module's real image so DbgHelp can validate candidates against it.
			DWORD64 loaded = 0;
			auto id_it = identities.find(name);
			if (id_it != identities.end())
			{
				const module_identity& id = id_it->second;
				if (id.has_signature())
				{
					// The PDB's file name is normally the RSDS record's basename; fall back
					// to "<module>.pdb" if the record was empty
					const char* pdb_file = id.pdb_name.c_str();
					if (const char* slash = strrchr(pdb_file, '\\'))
						pdb_file = slash + 1;
					std::string fallback;
					if (pdb_file[0] == 0)
					{
						fallback = name.substr(0, name.find_last_of('.')) + ".pdb";
						pdb_file = fallback.c_str();
					}

					char found[MAX_PATH] = {};
					if (SymFindFileInPath(process, nullptr, pdb_file, (PVOID)id.pdb_guid, id.pdb_age, 0, SSRVOPT_GUIDPTR, found, find_matching_pdb_callback, (PVOID)&id))
						loaded = SymLoadModuleEx(process, nullptr, found, name.c_str(), base, (DWORD)module_span, nullptr, 0);
				}

				// No exact match on the path: load via the module's on-disk image (if this
				// machine has it - profiling is usually local). DbgHelp reads its debug
				// directory and only accepts a PDB with the matching signature, checking
				// next to the image as well as the search path.
				if (loaded == 0 && !id.path.empty() && GetFileAttributesA(id.path.c_str()) != INVALID_FILE_ATTRIBUTES)
					loaded = SymLoadModuleEx(process, nullptr, id.path.c_str(), name.c_str(), base, (DWORD)module_span, nullptr, 0);
			}

			// ImageName = module name; DbgHelp searches the path for the image and/or its PDB.
			// An explicit non-zero DllSize is essential: with 0, DbgHelp registers a zero-length
			// range when it can't read the image's SizeOfImage (e.g. only the PDB is present),
			// and SymFromAddr then fails with ERROR_MOD_NOT_FOUND for every address.
			if (loaded == 0)
				loaded = SymLoadModuleEx(process, nullptr, name.c_str(), nullptr, base, (DWORD)module_span, nullptr, 0);
			uint64_t result = (loaded != 0) ? base : 0;

			// SymLoadModuleEx returns 0 both on error and when the module is already loaded;
			// the latter reports ERROR_SUCCESS. We never double-load (results are cached), but
			// treat "already loaded" defensively as success at the requested base.
			if (loaded == 0 && GetLastError() == ERROR_SUCCESS)
				result = base;

			module_state state;
			state.base = result;
			if (result == 0)
			{
				// Registration itself failed - definitively no symbols
				state.checked = true;
				state.has_symbols = false;
				state.status = module_symbols_info::status_t::failed;
			}
			return modules.emplace(name, state).first->second;
		}

		// Symbolicates a single callstack line. Returns it unchanged if it isn't a native
		// "Module+0xRVA" frame or its symbols aren't available.
		std::string symbolicate_line(const std::string& line)
		{
			const size_t plus = line.find("+0x");
			if (plus == std::string::npos)
				return line;

			std::string module = line.substr(0, plus);
			const uint64_t rva = strtoull(line.c_str() + plus + 3, nullptr, 16);

			module_state& state = get_module(module);
			// Never emit names from a wrong-version PDB - they would be plausible-looking
			// garbage. The mismatch is surfaced in the symbol status UI instead.
			if (state.base == 0 || state.status == module_symbols_info::status_t::mismatch)
				return line;

			const DWORD64 addr = state.base + rva;

			// SYMBOL_INFO needs room for the name after the struct; keep it 8-byte aligned.
			ULONG64 storage[(sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)) / sizeof(ULONG64) + 1] = {};
			SYMBOL_INFO* sym = (SYMBOL_INFO*)storage;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = MAX_SYM_NAME;

			DWORD64 sym_disp = 0;
			BOOL found = SymFromAddr(process, addr, &sym_disp, sym);

			// First SymFromAddr triggers the deferred load; only now does DbgHelp
			// know whether any symbols actually exist for this module
			if (!state.checked)
			{
				state.checked = true;

				IMAGEHLP_MODULE64 mi = {};
				mi.SizeOfStruct = sizeof(mi);
				if (SymGetModuleInfo64(process, addr, &mi))
				{
					switch (mi.SymType)
					{
					case SymPdb:
					{
						// When the module's exact PDB signature is known, judge the loaded
						// PDB by that signature alone. DbgHelp's PdbUnmatched flag cannot be
						// used then: it is also set whenever a PDB is loaded without an image
						// to validate against - which is exactly how the signature-matched
						// PDB found on the search path is loaded (as the "image" itself).
						auto id_it = identities.find(module);
						if (id_it != identities.end() && id_it->second.has_signature())
							state.status = memcmp(&mi.PdbSig70, id_it->second.pdb_guid, sizeof(id_it->second.pdb_guid)) == 0
								? module_symbols_info::status_t::loaded
								: module_symbols_info::status_t::mismatch;
						else
							state.status = mi.PdbUnmatched ? module_symbols_info::status_t::mismatch : module_symbols_info::status_t::loaded;
						state.symbol_file = mi.LoadedPdbName;
						break;
					}
					case SymExport:
						state.status = module_symbols_info::status_t::exports;
						state.symbol_file = mi.LoadedImageName;
						break;
					case SymNone:
					case SymDeferred:
						state.status = module_symbols_info::status_t::none;
						break;
					default: // Coff/Cv/Sym/Dia - debug info of some other kind
						state.status = module_symbols_info::status_t::loaded;
						state.symbol_file = mi.LoadedPdbName[0] != 0 ? mi.LoadedPdbName : mi.LoadedImageName;
						break;
					}

				}
				else
					state.status = module_symbols_info::status_t::none;

				// A successful SymFromAddr means symbols exist even if the module info
				// query disagrees (defensive; not expected in practice)
				if (found && state.status == module_symbols_info::status_t::none)
					state.status = module_symbols_info::status_t::loaded;

				// A mismatched PDB does not count as having symbols: the module needs the
				// right PDB, so keep it on the unresolved list
				state.has_symbols =
					state.status == module_symbols_info::status_t::loaded ||
					state.status == module_symbols_info::status_t::exports;
			}

			if (!found || state.status == module_symbols_info::status_t::mismatch)
				return line;

			std::string out = module + "!" + sym->Name;
			if (sym_disp != 0)
			{
				char tmp[32];
				snprintf(tmp, sizeof(tmp), "+0x%llX", (unsigned long long)sym_disp);
				out += tmp;
			}

			// Optional source location
			IMAGEHLP_LINE64 li = {};
			li.SizeOfStruct = sizeof(li);
			DWORD line_disp = 0;
			if (SymGetLineFromAddr64(process, addr, &line_disp, &li) && li.FileName != nullptr)
			{
				const char* fn = strrchr(li.FileName, '\\');
				fn = (fn != nullptr) ? fn + 1 : li.FileName;
				char tmp[MAX_PATH + 32];
				snprintf(tmp, sizeof(tmp), " (%s:%lu)", fn, (unsigned long)li.LineNumber);
				out += tmp;
			}

			// Bound the line length. Demangled C++ names (templates) can be up to
			// MAX_SYM_NAME (~2000) chars each; with up to 64 frames a callstack cell would
			// balloon to ~100KB, which makes the Qt table's text layout pathologically slow
			// (it lays each cell out at unbounded width). Such names are unreadable in a
			// list anyway, so truncate.
			static const size_t max_line_length = 300;
			if (out.size() > max_line_length)
			{
				out.resize(max_line_length);
				out += "...";
			}

			return out;
		}
	};

	symbol_resolver::symbol_resolver()
		: m_impl(new impl())
	{
		// The instance address is a convenient unique key for this DbgHelp session.
		m_impl->process = (HANDLE)this;
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
		m_impl->initialized = SymInitialize(m_impl->process, nullptr, FALSE) != FALSE;
	}

	symbol_resolver::~symbol_resolver()
	{
		if (m_impl->initialized)
			SymCleanup(m_impl->process);
		delete m_impl;
	}

	void symbol_resolver::set_module_identity(const std::string& module_name, const module_identity& identity)
	{
		auto it = m_impl->identities.find(module_name);
		if (it != m_impl->identities.end())
		{
			// Same identity re-reported (e.g. definitions re-sent on reconnect): keep the
			// already-loaded module
			if (it->second.path == identity.path &&
				memcmp(it->second.pdb_guid, identity.pdb_guid, sizeof(identity.pdb_guid)) == 0 &&
				it->second.pdb_age == identity.pdb_age)
				return;
		}

		m_impl->identities[module_name] = identity;

		// If the module was already loaded under the old identity, drop it so the next
		// frame that references it reloads with the new one
		auto mod_it = m_impl->modules.find(module_name);
		if (mod_it != m_impl->modules.end())
		{
			if (m_impl->initialized && mod_it->second.base != 0)
				SymUnloadModule64(m_impl->process, mod_it->second.base);
			m_impl->modules.erase(mod_it);
		}
	}

	void symbol_resolver::set_search_path(const std::string& path)
	{
		m_impl->search_path = path;
		if (!m_impl->initialized)
			return;

		for (auto& m : m_impl->modules)
			if (m.second.base != 0)
				SymUnloadModule64(m_impl->process, m.second.base);
		m_impl->modules.clear();
		m_impl->next_base = 0x100000000ULL;

		SymSetSearchPath(m_impl->process, path.empty() ? nullptr : path.c_str());
	}

	std::vector<std::string> symbol_resolver::get_unresolved_modules() const
	{
		std::vector<std::string> result;
		for (auto& m : m_impl->modules)
			if (m.second.checked && !m.second.has_symbols)
				result.push_back(m.first);
		return result;
	}

	std::vector<symbol_resolver::module_symbols_info> symbol_resolver::get_modules() const
	{
		std::vector<module_symbols_info> result;
		result.reserve(m_impl->modules.size());
		for (auto& m : m_impl->modules)
			result.push_back({ m.first, m.second.status, m.second.symbol_file });
		return result;
	}

	std::string symbol_resolver::symbolicate(const std::string& raw)
	{
		if (!m_impl->initialized)
			return raw;

		std::string result;
		result.reserve(raw.size() + 64);

		size_t pos = 0;
		while (pos < raw.size())
		{
			const size_t nl = raw.find('\n', pos);
			const size_t end = (nl == std::string::npos) ? raw.size() : nl;

			result += m_impl->symbolicate_line(raw.substr(pos, end - pos));
			if (nl != std::string::npos)
				result += '\n';

			pos = (nl == std::string::npos) ? raw.size() : nl + 1;
		}

		return result;
	}

#else // non-Windows: symbolication is a no-op (DbgHelp is Windows-only)

	struct symbol_resolver::impl {};
	symbol_resolver::symbol_resolver() : m_impl(nullptr) {}
	symbol_resolver::~symbol_resolver() {}
	void symbol_resolver::set_search_path(const std::string&) {}
	void symbol_resolver::set_module_identity(const std::string&, const module_identity&) {}
	std::string symbol_resolver::symbolicate(const std::string& raw) { return raw; }
	std::vector<std::string> symbol_resolver::get_unresolved_modules() const { return {}; }
	std::vector<symbol_resolver::module_symbols_info> symbol_resolver::get_modules() const { return {}; }

#endif
}
