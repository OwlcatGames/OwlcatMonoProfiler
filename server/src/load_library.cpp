#include "load_library.h"

#ifndef WIN32
    #include <dlfcn.h>
#endif

namespace owlcat
{
	void* library::get_method_by_name_internal(const char* name)
	{
#ifdef OS_WINDOWS
		return GetProcAddress(m_module, name);
#else
		return dlsym(m_module, name);
#endif
	}

	void* library::get_method_by_addr_internal(uint64_t addr)
	{
#ifdef OS_WINDOWS
		return (void*)((char*)m_module + addr);
#else
		return (void*)((char*)m_module + addr);
#endif
	}

	std::unique_ptr<library> library::load_library(const char* path)
	{
#ifdef OS_WINDOWS
		auto module = LoadLibraryA(path);
		if (module == nullptr)
			return nullptr;
		return std::unique_ptr<library>(new library(module));
#else
		auto module = dlopen(path, RTLD_LAZY);
		if (module == nullptr)
			return nullptr;
		return std::unique_ptr<library>(new library(module));
#endif
	}

	std::unique_ptr<library> library::get_library(const char* name)
	{
#ifdef OS_WINDOWS
		auto module = GetModuleHandleA(name);
		if (module == nullptr)
			return nullptr;
		return std::unique_ptr<library>(new library(module));
#else
		auto module = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
		if (module == nullptr)
			return nullptr;
		return std::unique_ptr<library>(new library(module));
#endif
	}
}
