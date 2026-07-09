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

		// Starts listening on the given port. The profiler itself is not started until the
		// connecting client sends its configuration (CMD_CONFIGURE) - this is how the
		// capture mode and native-hook config reach the in-game DLL. When wait_for_connection
		// is true, this blocks until a client has connected AND configured the profiler.
		void start(bool wait_for_connection, int port);
		void stop();

		void on_frame();
	};
}
