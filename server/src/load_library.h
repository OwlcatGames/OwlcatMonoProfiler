#pragma once

#include <cstdint>
#include <memory>

#if defined(WIN32) || defined(WIN64)
	#include "Windows.h"
	using module_t = HMODULE;
	#define OS_WINDOWS
#endif

namespace owlcat
{
	/*
	    A class that serves as an interface to platform-dependent functions for working
		with dynamic libraries (DLL on Windows, .so on *nix)
	*/
	class library
	{
	private:
		module_t m_module;

		library(module_t module) : m_module(module) {}

		void* get_method_by_name_internal(const char* name);
		void* get_method_by_addr_internal(uint64_t addr);

	public:
		static std::unique_ptr<library> load_library(const char* path);
		static std::unique_ptr<library> get_library(const char* name);

		template<typename T>
		T get_method_by_name(const char* name)
		{
			return (T)get_method_by_name_internal(name);
		}

		template<typename T>
		T get_method_by_addr(uint64_t addr)
		{
			return (T)get_method_by_addr_internal(addr);
		}
	};
}
