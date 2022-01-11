#pragma once

#include <vector>
#include <stdio.h>

/*
	A very basic logger.
*/
namespace owlcat
{
	class sink
	{
	public:
		virtual void log_str(const char* str) = 0;
	};

	class sink_file : public sink
	{
		FILE* m_file;
	public:
		sink_file(const char* file);
		~sink_file();

		virtual void log_str(const char* str) override;
	};

	class logger
	{
		/*
			Logger user retains responsibility to destroy sinks
		*/
		std::vector<sink*> m_sinks;
	public:
		void add_sink(sink* sink);
		void log_str(const char* str);
	};
}
