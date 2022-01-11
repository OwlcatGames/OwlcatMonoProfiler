#pragma once

#include "load_library.h"
#include "logger.h"

namespace owlcat
{
	/*
		This class works together with "library" class to allow finding and calling
		functions by name or address.
	*/
	template<typename T>
	class mono_func
	{
		const char* m_name;
		uint64_t m_addr;
		T m_ptr;

	public:
		mono_func() : m_name(nullptr), m_addr(0), m_ptr(nullptr) {}

		mono_func(const char* name) : m_name(name), m_addr(0), m_ptr(nullptr) {}
		mono_func(const char* name, uint64_t addr) : m_name(name), m_addr(addr), m_ptr(nullptr) {}

		/*
		    Performs a search for the function in the specified dynamic library module
		*/
		bool init(library* module, logger& log)
		{
			if (m_addr == 0)
				m_ptr = module->get_method_by_name<T>(m_name);
			else if (m_name != nullptr)
				m_ptr = module->get_method_by_addr<T>(m_addr);

			if (m_ptr == nullptr)
			{
				char error[256];
				sprintf(error, "Failed to find function with name %s or address %X", m_name, m_addr);
				log.log_str(error);
			}

			return m_ptr != nullptr;
		}

		/*
		    Performs a search for the function in the specified dynamic library module using mangled name
			(can be used with exported C++ function and methods with some reservations)
		*/
		//bool init_mangled(library* module)
		//{
		//	m_ptr = module->get_method_by_name_mangled<T>(m_name);
		//	return m_ptr != nullptr;
		//}

		/*
		    The next set of operators invoke the function with a varying number of parameters.
			I know this can be written better, but this will do for now.
			No checking is done, so if init() failed, the call will crash.
		*/
		template<typename U = T>
		std::enable_if_t<std::is_invocable_v<U>, std::invoke_result_t<U>>
			operator()()
		{
			return m_ptr();
		}

		template<typename U = T, typename A1>
		std::enable_if_t<std::is_invocable_v<U, A1>, std::invoke_result_t<U, A1>>
			operator()(A1 a1)
		{
			return m_ptr(a1);
		}

		template<typename U = T, typename A1, typename A2>
		std::enable_if_t<std::is_invocable_v<U, A1, A2>, std::invoke_result_t<U, A1, A2>>
			operator()(A1 a1, A2 a2)
		{
			return m_ptr(a1, a2);
		}

		template<typename U = T, typename A1, typename A2, typename A3>
		std::enable_if_t<std::is_invocable_v<U, A1, A2, A3>, std::invoke_result_t<U, A1, A2, A3>>
			operator()(A1 a1, A2 a2, A3 a3)
		{
			return m_ptr(a1, a2, a3);
		}

		template<typename U = T, typename A1, typename A2, typename A3, typename A4>
		std::enable_if_t<std::is_invocable_v<U, A1, A2, A3, A4>, std::invoke_result_t<U, A1, A2, A3, A4>>
			operator()(A1 a1, A2 a2, A3 a3, A4 a4)
		{
			return m_ptr(a1, a2, a3, a4);
		}

		template<typename U = T, typename A1, typename A2, typename A3, typename A4, typename A5>
		std::enable_if_t<std::is_invocable_v<U, A1, A2, A3, A4, A5>, std::invoke_result_t<U, A1, A2, A3, A4, A5>>
			operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
		{
			return m_ptr(a1, a2, a3, a4, a5);
		}

		template<typename U = T, typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
		std::enable_if_t<std::is_invocable_v<U, A1, A2, A3, A4, A5, A6>, std::invoke_result_t<U, A1, A2, A3, A4, A5, A6>>
			operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
		{
			return m_ptr(a1, a2, a3, a4, a5, a6);
		}
	};
}
