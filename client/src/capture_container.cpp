#include "capture_container.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace owlcat
{
	namespace capture_container
	{
		const char* entry_database = "database";
		const char* entry_events = "events";

		/*
			On-disk format, version 1:

			header_t, then header_t.entry_count of entry_t, then the entries' data
			at the offsets recorded in their entry_t.
		*/
		namespace format
		{
			static const uint32_t current_version = 1;
			static const char magic[8] = { 'O', 'W', 'L', 'C', 'A', 'P', 'T', 0 };

#pragma pack(push, 1)
			struct header_t
			{
				char magic[8];
				uint32_t version;
				uint32_t entry_count;
				// Reserved for future use, zeroed
				uint8_t reserved[16];
			};

			struct entry_t
			{
				// Entry name, zero-terminated
				char name[32];
				uint64_t offset;
				uint64_t size;
			};
#pragma pack(pop)

			static_assert(sizeof(header_t) == 32, "unexpected container header size");
			static_assert(sizeof(entry_t) == 48, "unexpected container entry size");
		}

		using namespace format;

		// Copies count bytes between two files using a scratch buffer. Returns false on any error.
		static bool copy_bytes(FILE* from, FILE* to, uint64_t count)
		{
			std::vector<uint8_t> buffer(1024 * 1024);
			while (count > 0)
			{
				const size_t block = (size_t)(count < buffer.size() ? count : buffer.size());
				if (fread(buffer.data(), 1, block, from) != block)
					return false;
				if (fwrite(buffer.data(), 1, block, to) != block)
					return false;
				count -= block;
			}
			return true;
		}

		static uint64_t file_size(FILE* file)
		{
			_fseeki64(file, 0, SEEK_END);
			uint64_t size = (uint64_t)_ftelli64(file);
			_fseeki64(file, 0, SEEK_SET);
			return size;
		}

		bool pack(const std::string& container_path, const std::map<std::string, std::string>& files)
		{
			FILE* container = fopen(container_path.c_str(), "wb");
			if (container == nullptr)
				return false;

			bool ok = true;

			header_t header = {};
			memcpy(header.magic, magic, sizeof(magic));
			header.version = current_version;
			header.entry_count = (uint32_t)files.size();

			std::vector<entry_t> entries;
			uint64_t data_offset = sizeof(header_t) + files.size() * sizeof(entry_t);

			// Write the header and a placeholder directory: sizes are filled in
			// while copying, and the directory is rewritten at the end
			ok = ok && fwrite(&header, sizeof(header), 1, container) == 1;
			ok = ok && _fseeki64(container, data_offset, SEEK_SET) == 0;

			for (auto& file : files)
			{
				entry_t entry = {};
				strncpy(entry.name, file.first.c_str(), sizeof(entry.name) - 1);
				entry.offset = data_offset;

				FILE* source = ok ? fopen(file.second.c_str(), "rb") : nullptr;
				if (source == nullptr)
				{
					ok = false;
					break;
				}

				entry.size = file_size(source);
				ok = copy_bytes(source, container, entry.size);
				fclose(source);

				data_offset += entry.size;
				entries.push_back(entry);
			}

			// Write the directory
			ok = ok && _fseeki64(container, sizeof(header_t), SEEK_SET) == 0;
			ok = ok && fwrite(entries.data(), sizeof(entry_t), entries.size(), container) == entries.size();

			fclose(container);

			if (!ok)
				remove(container_path.c_str());

			return ok;
		}

		bool unpack(const std::string& container_path, const std::string& destination_dir, std::map<std::string, std::string>& extracted_files)
		{
			extracted_files.clear();

			FILE* container = fopen(container_path.c_str(), "rb");
			if (container == nullptr)
				return false;

			header_t header = {};
			if (fread(&header, sizeof(header), 1, container) != 1 ||
				memcmp(header.magic, magic, sizeof(magic)) != 0)
			{
				fclose(container);
				return false;
			}

			if (header.version != 1)
			{
				printf("Capture container '%s' has unsupported version %u\n", container_path.c_str(), header.version);
				fclose(container);
				return false;
			}

			std::vector<entry_t> entries(header.entry_count);
			if (fread(entries.data(), sizeof(entry_t), entries.size(), container) != entries.size())
			{
				fclose(container);
				return false;
			}

			bool ok = true;
			for (auto& entry : entries)
			{
				entry.name[sizeof(entry.name) - 1] = 0;
				const std::string destination = destination_dir + "\\" + entry.name;

				FILE* out = fopen(destination.c_str(), "wb");
				if (out == nullptr)
				{
					ok = false;
					break;
				}

				ok = _fseeki64(container, entry.offset, SEEK_SET) == 0 && copy_bytes(container, out, entry.size);
				fclose(out);

				if (!ok)
					break;

				extracted_files[entry.name] = destination;
			}

			fclose(container);

			if (!ok)
				extracted_files.clear();

			return ok;
		}
	}
}
