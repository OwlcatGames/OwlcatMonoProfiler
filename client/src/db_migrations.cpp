#include <vector>
#include <string>
#include <algorithm>

#include "db_migrations.h"

namespace owlcat
{
    /*
        Description of database migration. Migrations are our mode of versioning: when the client tries
        to load a database created by an older version, it will attempt to migrate it to the current format.
    */
    struct migration_t
    {
        struct migration_entry_t
        {
            migration_entry_t(const char* q) : query(q) {}

            std::string query;
        };

        std::string id;
        std::vector<migration_entry_t> queries;
        int index = -1;
    };

    static const int base_migration_index = 0;
    static std::vector<migration_t> all_migrations =
    {
        //----------------------------------------------------------------
        {
            "Create basic tables",
            {
                {
                    "CREATE TABLE ProfilerEvents("
                    "event_id INTEGER PRIMARY KEY NOT NULL,"
                    "event_type_id INT NOT NULL,"
                    "type_id INT,"
                    "address INT NOT NULL,"
                    "size INT,"
                    "frame INT NOT NULL,"
                    "callstack_id INT"
                    ")"
                },
                {
                    "CREATE TABLE FrameStats("
                    "frame INTEGER PRIMARY KEY NOT NULL,"
                    "allocs INT NOT NULL,"
                    "frees INT NOT NULL,"
                    "size INT NOT NULL"
                    ")"
                },
                {
                    "CREATE TABLE ObjectTypes("
                    "type_id INTEGER PRIMARY KEY NOT NULL,"
                    "name TEXT NOT NULL UNIQUE"
                    ")"
                },
                {
                    "CREATE TABLE Callstacks("
                    "callstack_id INTEGER PRIMARY KEY NOT NULL,"
                    "callstack TEXT NOT NULL UNIQUE"
                    ")"
                },
                {
                    "CREATE INDEX frame_index ON ProfilerEvents("
                    "    frame ASC"
                    ");"
                },
            }
        },
        ////----------------------------------------------------------------
        //// This speeds up Go To Callstack significantly, but slows down
        //// insertation too much. Investigate a better way
        //{
        //    "Create index on addresses",
        //    {
        //        {
        //            "CREATE INDEX address_index ON ProfilerEvents("
        //            "    address ASC"
        //            ");"
        //        },
        //    }
        //},
    };

    // Important: queries are not registred before this call, so we can't use named queries here, unless we register them ourselves
    bool upgrade_database(persistent_storage::persistent_storage& db)
    {
        if (!db.is_open())
            return false;

        if (!db.query_immediate(
            "CREATE TABLE IF NOT EXISTS db_migrations ("
            "identifier VARCHAR(128) PRIMARY KEY NOT NULL,"
            "position INT);", {}))
            return false;

        if (!db.register_query("select_db_migrations", "SELECT identifier FROM db_migrations ORDER BY position;"))
            return false;

        std::vector<std::string> completed_migrations_ids;
        auto completed_migrations = db.query_data("select_db_migrations", {});
        while (completed_migrations.next())
        {
            std::string id = completed_migrations.get_string("identifier");
            completed_migrations_ids.push_back(id);
        }

        int migration_index = base_migration_index;
        auto incomplete_migrations = all_migrations;
        for (auto& migration : incomplete_migrations)
            migration.index = migration_index++;

        auto new_end = std::remove_if(incomplete_migrations.begin(), incomplete_migrations.end(), [&](const migration_t& m) { return std::find(completed_migrations_ids.begin(), completed_migrations_ids.end(), m.id) != completed_migrations_ids.end(); });
        incomplete_migrations.erase(new_end, incomplete_migrations.end());

        for (auto& migration : incomplete_migrations)
        {
            persistent_storage::transaction t(db, persistent_storage::transaction_behaviour::rollback);
            for (const auto& migration_entry : migration.queries)
            {
                bool ok = false;
                ok = db.query_immediate(migration_entry.query, {});

                if (!ok)
                {
                    printf("DB ERROR: %s", db.get_last_error().c_str());
                    t.rollback();
                    return false;
                }
            }

            if (!db.query_immediate("INSERT INTO db_migrations (identifier, position) VALUES (:id, :position);", { {"id", migration.id}, {"position", migration.index} }))
            {
                t.rollback();
                return false;
            }

            if (!t.commit())
                return false;
        }

        return true;
    }
}
