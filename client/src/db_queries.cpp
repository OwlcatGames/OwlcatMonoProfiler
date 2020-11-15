#include "db_queries.h"
#include "persistent_storage.h"

namespace owlcat
{
    namespace queries
    {
        query_id_t id_insert_alloc_event = "insert_alloc_event";
        query_id_t id_insert_free_event = "insert_free_event";
        query_id_t id_insert_frame_stats = "insert_frame_stats";
        query_id_t id_select_min_max_frame = "select_min_max_frame";
        query_id_t id_select_events = "select_events";
        query_id_t id_select_events_count = "select_events_count";
        query_id_t id_select_callstack_for_type = "select_callstack_for_type";
        query_id_t id_select_stats = "select_stats";
        query_id_t id_select_live_objects = "select_live_objects";
        query_id_t id_insert_type = "insert_type";
        query_id_t id_insert_callstack = "insert_callstack";
        query_id_t id_select_types = "select_types";
        query_id_t id_select_callstacks = "select_callstacks";
        query_id_t id_select_last_good_size = "select_last_good_size";

        bool insert_alloc_event(db_t& db, uint64_t frame, uint64_t addr, uint64_t size, uint64_t type_id, uint64_t callstack_id)
        {
            return db.query(queries::id_insert_alloc_event,
                {
                    {"type_id", type_id},
                    {"address", addr},
                    {"size", size},
                    {"frame", frame},
                    {"callstack_id", callstack_id},
                });
        }

        bool insert_free_event(db_t& db, uint64_t frame, uint64_t addr, uint32_t size)
        {
            return db.query(queries::id_insert_free_event,
                {
                    {"address", addr},
                    {"frame", frame},
                    {"size", size},
                });
        }

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

        bool insert_frame_stats(db_t& db, uint64_t frame, uint64_t allocs, uint64_t frees, int64_t size)
        {
            return db.query(queries::id_insert_frame_stats,
                {
                    {"frame", frame},
                    {"allocs", allocs},
                    {"frees", frees},
                    {"size", size},
                });
        }

        cursor_t select_min_max_frame(db_t& db)
        {
            return db.query_data(queries::id_select_min_max_frame, {});
        }

        cursor_t select_events(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            return db.query_data(queries::id_select_events, { {"from", from_frame}, {"to", to_frame} });
        }

        size_t select_events_count(db_t& db, uint64_t from_frame, uint64_t to_frame)
        {
            auto cursor = db.query_data(queries::id_select_events_count, { {"from", from_frame}, {"to", to_frame} });
            if (!cursor.next())
                return 0;

            return cursor.get_int64("count");
        }

        cursor_t select_callstack_for_type(db_t& db, uint64_t from_frame, uint64_t to_frame, uint64_t type)
        {
            return db.query_data(queries::id_select_callstack_for_type, { {"from", from_frame}, {"to", to_frame}, {"type", type} });
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

            register_query(queries::id_insert_alloc_event,
                "INSERT INTO ProfilerEvents (event_type_id, type_id, address, size, frame, callstack_id)"
                "VALUES (1, $type_id, $address, $size, $frame, $callstack_id)"
            );
            register_query(queries::id_insert_free_event,
                "INSERT INTO ProfilerEvents (event_type_id, address, frame, size)"
                "VALUES (2, $address, $frame, $size)"
            );
            register_query(queries::id_insert_frame_stats,
                "INSERT OR REPLACE INTO FrameStats (frame, allocs, frees, size)"
                "VALUES ($frame, $allocs, $frees, $size)"
            );            
            register_query(queries::id_select_min_max_frame,
                "SELECT MIN(frame) as min_frame, MAX(frame) AS max_frame FROM ProfilerEvents"
            );
            register_query(queries::id_select_events,
                "SELECT event_type_id, event_id, type_id, address, size, frame, callstack_id "
                "FROM ProfilerEvents "
                "WHERE frame >= $from AND frame <= $to "
                "ORDER BY frame ASC"
            );
            register_query(queries::id_select_events_count,
                "SELECT COUNT(*) AS count "
                "FROM ProfilerEvents "
                "WHERE frame >= $from AND frame <= $to "
            );
            register_query(queries::id_select_callstack_for_type,
                "SELECT event_id, ProfilerEvents.type_id, ObjectTypes.name, address, size, frame, ProfilerEvents.callstack_id, callstack "
                "FROM ProfilerEvents "
                "LEFT JOIN ObjectTypes ON ObjectTypes.type_id=ProfilerEvents.type_id "
                "LEFT JOIN Callstacks ON Callstacks.callstack_id=ProfilerEvents.callstack_id "
                "WHERE frame >= $from AND frame <= $to AND ProfilerEvents.type_id = $type"
            );
            register_query(queries::id_select_stats,
                "SELECT frame, allocs, frees, size "
                "FROM FrameStats "
                "WHERE frame >= $from AND frame <= $to "
                "ORDER BY frame"
            );
            //register_query(queries::id_select_live_objects,
            //    //"WITH last_event_per_address AS(SELECT address, MAX(event_id) OVER(PARTITION BY address) AS event_id FROM ProfilerEvents) "
            //    //"SELECT * FROM last_event_per_address LEFT JOIN ProfilerEvents WHERE ProfilerEvents.event_id = last_event_per_address.event_id AND event_type_id = 1 "
            //    "WITH last_event_per_address AS(SELECT address, MAX(event_id) OVER(PARTITION BY address) AS event_id FROM ProfilerEvents) "
            //    "SELECT ProfilerEvents.event_id, ProfilerEvents.type_id, ObjectTypes.name, ProfilerEvents.address, size, frame, ProfilerEvents.callstack_id, callstack "
            //    "FROM last_event_per_address "
            //    "LEFT JOIN ProfilerEvents "
            //    "LEFT JOIN ObjectTypes ON ObjectTypes.type_id=ProfilerEvents.type_id "
            //    "LEFT JOIN Callstacks ON Callstacks.callstack_id=ProfilerEvents.callstack_id "
            //    "WHERE ProfilerEvents.event_id = last_event_per_address.event_id AND event_type_id = 1 "
            //);

            return all_ok;
        }
    }
}