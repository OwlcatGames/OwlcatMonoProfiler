#pragma once

#include <string>
#include <map>
#include <functional>
#include <cstdint>

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
		// The optional progress callback receives (copied_bytes, total_bytes) as
		// the copy proceeds; returning false from it aborts the unpack, which then
		// returns false. Partially extracted files are left behind - the caller
		// owns the destination directory and its cleanup.
		bool unpack(const std::string& container_path, const std::string& destination_dir, std::map<std::string, std::string>& extracted_files, const std::function<bool(uint64_t copied, uint64_t total)>& progress = nullptr);
	}
}
