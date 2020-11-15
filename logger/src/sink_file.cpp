#include "logger.h"

namespace owlcat
{
	sink_file::sink_file(const char* file)
	{
		m_file = fopen(file, "w");
	}

	sink_file::~sink_file()
	{
		fclose(m_file);
	}

	void sink_file::log_str(const char* str)
	{
		if (m_file != nullptr)
		{
			fprintf(m_file, "%s\n", str);
			fflush(m_file);
		}
	}
}
