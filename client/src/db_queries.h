#pragma once

#include <cstdint>
#include <string>

namespace persistent_storage
{
    class persistent_storage;
    class cursor;
};

namespace owlcat
{
    namespace queries
    {
        using db_t = persistent_storage::persistent_storage;
        using cursor_t = persistent_storage::cursor;

        using query_id_t = const char*;

        bool insert_type(db_t& db, const std::string& type, uint64_t id);
        bool insert_callstack(db_t& db, const std::string& callstack, uint64_t id);
        // Also records the byte range of the frame's events in the event log file
        bool insert_frame_stats(db_t& db, uint64_t frame, uint64_t allocs, uint64_t frees, int64_t size, uint64_t first_event_offset, uint64_t end_event_offset);

        cursor_t select_min_max_frame(db_t& db);
        // Returns the byte range (begin_offset, end_offset) of the events of the given
        // frame range in the event log file
        cursor_t select_frame_event_range(db_t& db, uint64_t from_frame, uint64_t to_frame);
        cursor_t select_stats(db_t& db, uint64_t from_frame, uint64_t to_frame);
        // Per-frame whole-process memory snapshot (see SRV_MEMSTATS)
        bool insert_memstats(db_t& db, uint64_t frame, uint64_t working_set, uint64_t committed, uint64_t gc_heap);
        cursor_t select_memstats(db_t& db, uint64_t from_frame, uint64_t to_frame);
        cursor_t select_types(db_t& db);
        cursor_t select_callstacks(db_t& db);
        cursor_t select_last_good_size(db_t& db, uint64_t from_frame);

        bool register_queries(persistent_storage::persistent_storage& db);
    }
}
