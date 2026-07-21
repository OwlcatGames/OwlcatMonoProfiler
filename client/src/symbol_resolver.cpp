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

	struct symbol_resolver::impl
	{
		// A unique, fake "process" handle keying this DbgHelp session (we are not the
		// target process; modules are loaded manually at synthetic bases).
		HANDLE process = nullptr;
		bool initialized = false;
		std::string search_path;

		// module name -> synthetic load base, or 0 if its symbols couldn't be loaded
		// (cached so a missing PDB isn't retried for every frame)
		std::unordered_map<std::string, uint64_t> modules;
		// Synthetic address space per module. We register each module with an explicit,
		// generously-large span (rather than letting DbgHelp derive the size from the image,
		// which we may not have) so that base+rva always lands inside the module's range.
		// The stride between bases is larger than the span, so ranges never overlap even for
		// a huge GameAssembly.dll.
		static const uint64_t module_span = 0x80000000ULL;   // 2 GB
		static const uint64_t module_stride = 0x100000000ULL; // 4 GB
		uint64_t next_base = module_stride;

		// Loads (once) the symbols for a module and returns its synthetic base, or 0 on failure.
		uint64_t get_module_base(const std::string& name)
		{
			auto it = modules.find(name);
			if (it != modules.end())
				return it->second;

			uint64_t base = next_base;
			next_base += module_stride;

			// ImageName = module name; DbgHelp searches the path for the image and/or its PDB.
			// An explicit non-zero DllSize is essential: with 0, DbgHelp registers a zero-length
			// range when it can't read the image's SizeOfImage (e.g. only the PDB is present),
			// and SymFromAddr then fails with ERROR_MOD_NOT_FOUND for every address.
			DWORD64 loaded = SymLoadModuleEx(process, nullptr, name.c_str(), nullptr, base, (DWORD)module_span, nullptr, 0);
			uint64_t result = (loaded != 0) ? base : 0;

			// SymLoadModuleEx returns 0 both on error and when the module is already loaded;
			// the latter reports ERROR_SUCCESS. We never double-load (results are cached), but
			// treat "already loaded" defensively as success at the requested base.
			if (loaded == 0 && GetLastError() == ERROR_SUCCESS)
				result = base;

			modules[name] = result;
			return result;
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

			const uint64_t base = get_module_base(module);
			if (base == 0)
				return line;

			const DWORD64 addr = base + rva;

			// SYMBOL_INFO needs room for the name after the struct; keep it 8-byte aligned.
			ULONG64 storage[(sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)) / sizeof(ULONG64) + 1] = {};
			SYMBOL_INFO* sym = (SYMBOL_INFO*)storage;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = MAX_SYM_NAME;

			DWORD64 sym_disp = 0;
			if (!SymFromAddr(process, addr, &sym_disp, sym))
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

	void symbol_resolver::set_search_path(const std::string& path)
	{
		m_impl->search_path = path;
		if (!m_impl->initialized)
			return;

		for (auto& m : m_impl->modules)
			if (m.second != 0)
				SymUnloadModule64(m_impl->process, m.second);
		m_impl->modules.clear();
		m_impl->next_base = 0x100000000ULL;

		SymSetSearchPath(m_impl->process, path.empty() ? nullptr : path.c_str());
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
	std::string symbol_resolver::symbolicate(const std::string& raw) { return raw; }

#endif
}
