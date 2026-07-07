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

        cursor_t select_types(db_t& db)
        {
            return db.query_data(queries::id_select_types, {});
        }

        cursor_t select_callstacks(db_t& db)
        {
            return db.query_data(queries::id_select_callstacks, {});
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

            return all_ok;
        }
    }
}
