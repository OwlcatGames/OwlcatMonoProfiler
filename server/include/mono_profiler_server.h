#pragma once

namespace owlcat
{
#ifdef WIN32
    #define DLL_EXPORT __declspec(dllexport)
#else
    #define DLL_EXPORT __attribute__ ((dllexport))
#endif
	/*
		Class that starts and stops the profiler,
		and also handles network connections.
	*/
	class DLL_EXPORT mono_profiler_server
	{
		class details;

		details *m_details;

	public:
		mono_profiler_server();
		~mono_profiler_server();

		void start(bool wait_for_connection, int port);
		void stop();

		void on_frame();
	};
}
