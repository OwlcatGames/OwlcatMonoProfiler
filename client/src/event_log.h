#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <memory>
#include <functional>

namespace owlcat
{
	/*
		The event log: a flat, append-only file storing every profiling event of a capture,
		in the order of arrival. The database only stores, per frame, the byte range of the
		frame's events in this file (see FrameStats), so any frame range of the log can be
		located with one query and read with one sequential scan - including while the
		capture is still running.

		FORMAT VERSIONING RULES:
		- The writer always writes the CURRENT version of the format (event_log_writer).
		- A reader exists for every version ever shipped. When the format changes:
		    1. bump the version constant and adjust the writer,
		    2. COPY the newest event_log_reader_vN implementation in event_log.cpp to
		       event_log_reader_vN+1 and adjust the copy, leaving the old one frozen,
		    3. add the new version to the dispatch in event_log_reader::open.
		  Old captures then remain readable forever.
	*/

	// A decoded event, independent of the on-disk format version
	struct event_view
	{
		uint64_t frame;
		uint64_t addr;
		uint64_t type_id;
		uint64_t callstack_id;
		uint32_t size;
		bool is_alloc;
	};

	/*
		Writes the current version of the event log format. Single writer; readers may
		read the file concurrently, but must only rely on byte ranges that were published
		(stored in the database) after a flush.
	*/
	class event_log_writer
	{
	public:
		~event_log_writer();

		// Creates a new event log file, overwriting an existing one
		bool create(const std::string& path);
		void close();
		bool is_open() const;

		// Appends one event. The event is not guaranteed to be visible to readers
		// until flush is called.
		void append(uint64_t frame, uint64_t addr, uint64_t type_id, uint64_t callstack_id, uint32_t size, bool is_alloc);

		// Makes all appended events visible to readers of the same file
		bool flush();

		// Byte offset at which the next appended event will start. Use together with
		// flush to publish the byte range of a frame's events.
		uint64_t position() const { return m_position; }

	private:
		FILE* m_file = nullptr;
		uint64_t m_position = 0;
	};

	/*
		Reads an event log of any supported format version. Create with open, which
		returns the reader matching the version stored in the file.

		A reader instance is not thread-safe; create one per query. Opening is cheap.
	*/
	class event_log_reader
	{
	public:
		virtual ~event_log_reader() {}

		// Opens the log and returns a reader for the format version in its header,
		// or null if the file is missing, damaged, or of an unknown version
		static std::unique_ptr<event_log_reader> open(const std::string& path);

		// Byte offset of the first event in the log
		virtual uint64_t begin_offset() const = 0;
		// Byte offset just past the last complete event that was in the file when the
		// reader was opened. Note: during a live capture, prefer the ranges published
		// to the database - the file may end with a partially flushed event.
		virtual uint64_t end_offset() const = 0;

		// Number of events stored between two byte offsets
		virtual uint64_t count_events(uint64_t begin_offset, uint64_t end_offset) const = 0;

		// Calls the callback for every event in [begin_offset, end_offset), in order.
		// Stops early if the callback returns false. Returns false on read errors.
		virtual bool read_range(uint64_t begin_offset, uint64_t end_offset, const std::function<bool(const event_view&)>& callback) = 0;

		// Finds the latest allocation event of the given address before end_offset.
		// Returns false if there is none.
		virtual bool find_last_allocation(uint64_t address, uint64_t end_offset, event_view& result) = 0;
	};
}
