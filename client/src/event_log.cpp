#include "event_log.h"

#include <cstring>
#include <vector>
#include <algorithm>

namespace owlcat
{
	/*
		On-disk format. The file starts with header_t, followed by tightly packed
		records of the version given in the header.

		Version history:
		- 1: record_v1_t
	*/
	namespace event_log_format
	{
		static const uint32_t current_version = 1;
		static const char magic[8] = { 'O', 'W', 'L', 'E', 'V', 'T', 'S', 0 };

#pragma pack(push, 1)
		struct header_t
		{
			char magic[8];
			uint32_t version;
			// Size of one record, for sanity checking
			uint32_t record_size;
			// Reserved for future use, zeroed
			uint8_t reserved[16];
		};

		struct record_v1_t
		{
			uint64_t frame;
			uint64_t addr;
			uint64_t type_id;
			uint64_t callstack_id;
			uint32_t size;
			// 1 = allocation, 2 = free (same values as the old ProfilerEvents table)
			uint32_t event_type;
		};
#pragma pack(pop)

		static_assert(sizeof(header_t) == 32, "unexpected event log header size");
		static_assert(sizeof(record_v1_t) == 40, "unexpected event log record size");
	}

	using namespace event_log_format;

	// ---------------- Writer (current version) ----------------

	event_log_writer::~event_log_writer()
	{
		close();
	}

	bool event_log_writer::create(const std::string& path)
	{
		close();

		// _SH_DENYNO: readers must be able to open the file while we're writing it
		m_file = _fsopen(path.c_str(), "wb", _SH_DENYNO);
		if (m_file == nullptr)
			return false;

		setvbuf(m_file, nullptr, _IOFBF, 1024 * 1024);

		header_t header = {};
		memcpy(header.magic, magic, sizeof(magic));
		header.version = current_version;
		header.record_size = sizeof(record_v1_t);

		if (fwrite(&header, sizeof(header), 1, m_file) != 1)
		{
			close();
			return false;
		}

		m_position = sizeof(header_t);
		return true;
	}

	void event_log_writer::close()
	{
		if (m_file != nullptr)
		{
			fclose(m_file);
			m_file = nullptr;
		}
		m_position = 0;
	}

	bool event_log_writer::is_open() const
	{
		return m_file != nullptr;
	}

	void event_log_writer::append(uint64_t frame, uint64_t addr, uint64_t type_id, uint64_t callstack_id, uint32_t size, bool is_alloc)
	{
		record_v1_t record;
		record.frame = frame;
		record.addr = addr;
		record.type_id = type_id;
		record.callstack_id = callstack_id;
		record.size = size;
		record.event_type = is_alloc ? 1 : 2;

		fwrite(&record, sizeof(record), 1, m_file);
		m_position += sizeof(record);
	}

	bool event_log_writer::flush()
	{
		return fflush(m_file) == 0;
	}

	// ---------------- Reader, format version 1 ----------------

	class event_log_reader_v1 : public event_log_reader
	{
	public:
		event_log_reader_v1(FILE* file)
			: m_file(file)
		{
			_fseeki64(m_file, 0, SEEK_END);
			const uint64_t file_size = (uint64_t)_ftelli64(m_file);
			const uint64_t events = file_size > sizeof(header_t) ? (file_size - sizeof(header_t)) / sizeof(record_v1_t) : 0;
			m_end_offset = sizeof(header_t) + events * sizeof(record_v1_t);
		}

		~event_log_reader_v1()
		{
			fclose(m_file);
		}

		uint64_t begin_offset() const override
		{
			return sizeof(header_t);
		}

		uint64_t end_offset() const override
		{
			return m_end_offset;
		}

		uint64_t count_events(uint64_t begin_offset, uint64_t end_offset) const override
		{
			if (end_offset <= begin_offset)
				return 0;

			return (end_offset - begin_offset) / sizeof(record_v1_t);
		}

		bool read_range(uint64_t begin_offset, uint64_t end_offset, const std::function<bool(const event_view&)>& callback) override
		{
			uint64_t remaining = count_events(begin_offset, end_offset);
			if (remaining == 0)
				return true;

			if (_fseeki64(m_file, begin_offset, SEEK_SET) != 0)
				return false;

			std::vector<record_v1_t> block(block_records);
			while (remaining > 0)
			{
				const size_t count = (size_t)std::min<uint64_t>(remaining, block_records);
				if (fread(block.data(), sizeof(record_v1_t), count, m_file) != count)
					return false;

				for (size_t i = 0; i < count; ++i)
				{
					if (!callback(to_view(block[i])))
						return true;
				}

				remaining -= count;
			}

			return true;
		}

		bool find_last_allocation(uint64_t address, uint64_t end_offset, event_view& result) override
		{
			if (end_offset <= sizeof(header_t))
				return false;

			// Scan block-sized windows from the end of the range towards the beginning:
			// the latest matching event within the first window that has one is the answer
			std::vector<record_v1_t> block(block_records);

			uint64_t window_end = count_events(sizeof(header_t), end_offset);
			while (window_end > 0)
			{
				const uint64_t window_begin = window_end > block_records ? window_end - block_records : 0;
				const size_t count = (size_t)(window_end - window_begin);

				if (_fseeki64(m_file, sizeof(header_t) + window_begin * sizeof(record_v1_t), SEEK_SET) != 0)
					return false;
				if (fread(block.data(), sizeof(record_v1_t), count, m_file) != count)
					return false;

				for (size_t i = count; i > 0; --i)
				{
					const record_v1_t& record = block[i - 1];
					if (record.event_type == 1 && record.addr == address)
					{
						result = to_view(record);
						return true;
					}
				}

				window_end = window_begin;
			}

			return false;
		}

	private:
		static const size_t block_records = 64 * 1024;

		static event_view to_view(const record_v1_t& record)
		{
			event_view view;
			view.frame = record.frame;
			view.addr = record.addr;
			view.type_id = record.type_id;
			view.callstack_id = record.callstack_id;
			view.size = record.size;
			view.is_alloc = record.event_type == 1;
			return view;
		}

		FILE* m_file;
		uint64_t m_end_offset = 0;
	};

	// ---------------- Version dispatch ----------------

	std::unique_ptr<event_log_reader> event_log_reader::open(const std::string& path)
	{
		FILE* file = _fsopen(path.c_str(), "rb", _SH_DENYNO);
		if (file == nullptr)
			return nullptr;

		header_t header = {};
		if (fread(&header, sizeof(header), 1, file) != 1 ||
			memcmp(header.magic, magic, sizeof(magic)) != 0)
		{
			fclose(file);
			return nullptr;
		}

		switch (header.version)
		{
		case 1:
			if (header.record_size != sizeof(record_v1_t))
				break;
			return std::make_unique<event_log_reader_v1>(file);
		}

		printf("Event log '%s' has unsupported version %u\n", path.c_str(), header.version);
		fclose(file);
		return nullptr;
	}
}
