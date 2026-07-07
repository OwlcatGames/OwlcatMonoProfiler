#pragma once

#include <string>
#include <map>

namespace owlcat
{
	/*
		The capture container packs the files that make up one capture (the SQLite
		database and the event log) into a single .owl file on save.

		FORMAT VERSIONING RULES: same as the event log (see event_log.h) - pack always
		writes the current version, unpack dispatches on the version in the header and
		must keep support for every version ever shipped.
	*/
	namespace capture_container
	{
		// Names of the container entries used for captures
		extern const char* entry_database;
		extern const char* entry_events;

		// Packs the given files (entry name -> source path) into a single container
		// file, overwriting it if it exists
		bool pack(const std::string& container_path, const std::map<std::string, std::string>& files);

		// Extracts all entries of a container into the given directory (which must
		// exist). Returns entry name -> extracted file path.
		bool unpack(const std::string& container_path, const std::string& destination_dir, std::map<std::string, std::string>& extracted_files);
	}
}
