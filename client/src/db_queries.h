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

        bool insert_alloc_event(db_t& db, uint64_t frame, uint64_t addr, uint64_t size, uint64_t type_id, uint64_t callstack_id);
        bool insert_free_event(db_t& db, uint64_t frame, uint64_t addr, uint32_t size);
        bool insert_type(db_t& db, const std::string& type, uint64_t id);
        bool insert_callstack(db_t& db, const std::string& callstack, uint64_t id);
        bool insert_frame_stats(db_t& db, uint64_t frame, uint64_t allocs, uint64_t frees, int64_t size);

        cursor_t select_min_max_frame(db_t& db);
        cursor_t select_events(db_t& db, uint64_t from_frame, uint64_t to_frame);
        size_t select_events_count(db_t& db, uint64_t from_frame, uint64_t to_frame);
        cursor_t select_callstack_for_type(db_t& db, uint64_t from_frame, uint64_t to_frame, uint64_t type);
        cursor_t select_stats(db_t& db, uint64_t from_frame, uint64_t to_frame);
        cursor_t select_types(db_t& db);
        cursor_t select_callstacks(db_t& db);
        cursor_t select_last_good_size(db_t& db, uint64_t from_frame);
        cursor_t select_allocation_type_and_stack(db_t& db, uint64_t address);

        bool register_queries(persistent_storage::persistent_storage& db);
    }
}
