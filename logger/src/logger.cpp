#include "logger.h"

namespace owlcat
{
	void logger::add_sink(sink* sink)
	{
		m_sinks.push_back(sink);
	}

	void logger::log_str(const char* str)
	{
		for (auto s : m_sinks)
		{
			s->log_str(str);
		}
	}
}
