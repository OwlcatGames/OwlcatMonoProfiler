#include "db_queries.h"
#include "persistent_storage.h"

namespace owlcat
{
    namespace queries
    {
        query_id_t id_insert_frame_stats = "insert_frame_stats";
        query_id_t id_select_min_max_frame = "select_min_max_frame";
        query_id_t id_select_frame_event_range = "select_frame_event_range";
        query_id_t id_select_stats = "select_stats";
        query_id_t id_insert_type = "insert_type";
        query_id_t id_insert_callstack = "insert_callstack";
        query_id_t id_select_types = "select_types";
        query_id_t id_select_callstacks = "select_callstacks";
        query_id_t id_select_last_good_size = "select_last_good_size";
        query_id_t id_insert_memstats = "insert_memstats";
        query_id_t id_select_memstats = "select_memstats";
        query_id_t id_insert_gc_event = "insert_gc_event";
        query_id_t id_select_gc_events = "select_gc_events";
        query_id_t id_insert_module = "insert_module";
        query_id_t id_select_modules = "select_modules";

        bool insert_type(db_t& db, const std::string& type, uint64_t id)
        {
            return db.query(queries::id_insert_type,
                {
                    { "id", id },
                    { "type", type }
                });
        }

        bool insert_callstack(db_t& db, const std::string& callstack, uint64_t id)
        {
            return db.query(queries::id_insert_callstack,
                {
                    { "id", id },
                    { "stack", callstack }
                });
        }

        bool insert_frame_stats(db_t& db, uint64_t frame, uint64_t allocs, uint64_t frees, int64_t size, uint64_t first_event_offset, uint64_t end_event_offset)
        {
            return db.query(queries::id_insert_frame_stats,
                {
                    {"frame", frame},
                    {"allocs", allocs},
                    {"frees", frees},
                    {"size", size},
                    {"first_offset", first_event_offset},
                    {"end_offset", end_event_offset},
                });
        }

        cursor_t select_min_max_frame(db_t& db)
        {
            return db.query_data(queries::id_select_min_max_frame, {});
        }

        cursor_t select_frame_event_range(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            return db.query_data(queries::id_select_frame_event_range, { {"from", from_frame}, {"to", to_frame} });
        }

        cursor_t select_stats(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            return db.query_data(queries::id_select_stats, { {"from", from_frame}, {"to", to_frame} });
        }

        bool insert_memstats(db_t& db, uint64_t frame, uint64_t working_set, uint64_t committed, uint64_t gc_heap)
        {
            return db.query(queries::id_insert_memstats,
                {
                    {"frame", frame},
                    {"working_set", working_set},
                    {"committed", committed},
                    {"gc_heap", gc_heap},
                });
        }

        cursor_t select_memstats(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            return db.query_data(queries::id_select_memstats, { {"from", from_frame}, {"to", to_frame} });
        }

        bool insert_gc_event(db_t& db, uint64_t frame, uint64_t freed_objects, uint64_t freed_bytes)
        {
            return db.query(queries::id_insert_gc_event,
                {
                    {"frame", frame},
                    {"freed_objects", freed_objects},
                    {"freed_bytes", freed_bytes},
                });
        }

        cursor_t select_gc_events(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            return db.query_data(queries::id_select_gc_events, { {"from", from_frame}, {"to", to_frame} });
        }

        cursor_t select_types(db_t& db)
        {
            return db.query_data(queries::id_select_types, {});
        }

        cursor_t select_callstacks(db_t& db)
        {
            return db.query_data(queries::id_select_callstacks, {});
        }

        bool insert_module(db_t& db, const std::string& name, const std::string& path, const std::string& pdb_guid, uint64_t pdb_age, const std::string& pdb_name)
        {
            return db.query(queries::id_insert_module,
                {
                    { "name", name },
                    { "path", path },
                    { "guid", pdb_guid },
                    { "age", pdb_age },
                    { "pdb_name", pdb_name },
                });
        }

        cursor_t select_modules(db_t& db)
        {
            return db.query_data(queries::id_select_modules, {});
        }

        cursor_t select_last_good_size(db_t& db, uint64_t from_frame)
        {
            return db.query_data(queries::id_select_last_good_size, { {"from", from_frame} });
        }

        bool register_queries(persistent_storage::persistent_storage& db)
        {
            if (!db.is_open())
                return false;

            bool all_ok = true;

            auto register_query = [&db, &all_ok](const char* id, const char* text)
            {
                bool result = db.register_query(id, text);
                if (!result)
                    printf("Failed to register query %s: %s", id, db.get_last_error().c_str());
                all_ok = all_ok && result;
            };

            register_query(queries::id_insert_type,
                "INSERT INTO ObjectTypes (type_id, name)"
                "VALUES ($id, $type)"
            );
            register_query(queries::id_insert_callstack,
                "INSERT INTO Callstacks (callstack_id, callstack)"
                "VALUES ($id, $stack)"
            );
            register_query(queries::id_select_types,
                "SELECT type_id, name FROM ObjectTypes"
            );
            register_query(queries::id_select_callstacks,
                "SELECT callstack_id, callstack FROM Callstacks"
            );
            register_query(queries::id_select_last_good_size,
                "SELECT size "
                "FROM FrameStats "
                "WHERE frame <= $from ORDER BY frame DESC LIMIT 1"
            );
            register_query(queries::id_insert_frame_stats,
                "INSERT OR REPLACE INTO FrameStats (frame, allocs, frees, size, first_event_offset, end_event_offset)"
                "VALUES ($frame, $allocs, $frees, $size, $first_offset, $end_offset)"
            );
            register_query(queries::id_select_min_max_frame,
                "SELECT MIN(frame) as min_frame, MAX(frame) AS max_frame FROM FrameStats"
            );
            // Offsets grow monotonically with frames, so the range of a set of frames
            // is (min of first offsets, max of end offsets)
            register_query(queries::id_select_frame_event_range,
                "SELECT MIN(first_event_offset) AS begin_offset, MAX(end_event_offset) AS end_offset "
                "FROM FrameStats "
                "WHERE frame >= $from AND frame <= $to"
            );
            register_query(queries::id_select_stats,
                "SELECT frame, allocs, frees, size "
                "FROM FrameStats "
                "WHERE frame >= $from AND frame <= $to "
                "ORDER BY frame"
            );
            // REPLACE: the server re-sends definitions when a client reconnects, and a
            // module can be re-reported with a new identity after a profiler restart
            register_query(queries::id_insert_module,
                "INSERT OR REPLACE INTO Modules (name, path, pdb_guid, pdb_age, pdb_name)"
                "VALUES ($name, $path, $guid, $age, $pdb_name)"
            );
            register_query(queries::id_select_modules,
                "SELECT name, path, pdb_guid, pdb_age, pdb_name FROM Modules"
            );
            register_query(queries::id_insert_memstats,
                "INSERT OR REPLACE INTO MemStats (frame, working_set, committed, gc_heap)"
                "VALUES ($frame, $working_set, $committed, $gc_heap)"
            );
            register_query(queries::id_insert_gc_event,
                "INSERT OR REPLACE INTO GcEvents (frame, freed_objects, freed_bytes)"
                "VALUES ($frame, $freed_objects, $freed_bytes)"
            );
            register_query(queries::id_select_gc_events,
                "SELECT frame, freed_objects, freed_bytes "
                "FROM GcEvents "
                "WHERE frame >= $from AND frame <= $to "
                "ORDER BY frame"
            );

            register_query(queries::id_select_memstats,
                "SELECT frame, working_set, committed, gc_heap "
                "FROM MemStats "
                "WHERE frame >= $from AND frame <= $to "
                "ORDER BY frame"
            );

            return all_ok;
        }
    }
}
