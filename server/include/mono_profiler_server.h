#pragma once

namespace owlcat
{
	/*
		Class that starts and stops the profiler,
		and also handles network connections.
	*/
	class __declspec(dllexport) mono_profiler_server
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
