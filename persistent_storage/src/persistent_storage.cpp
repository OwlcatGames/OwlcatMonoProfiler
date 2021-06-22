#include "persistent_storage.h"

#include "sqlite3.h"

#include <string.h>
#include <iostream>
#include <filesystem>

namespace persistent_storage{

static void logger( void *pArg, int iErrCode, const char *zMsg )
{
    printf("sqlite error %i: %s\n", iErrCode, (zMsg ? zMsg : "(null)"));
}

// TODO: Replace with better logging
#define LOG(channel) std::cout

struct persistent_storage::details
{
    ~details() {}

    sqlite3* m_db = nullptr;
    struct statement_t
    {
        sqlite3_stmt* sqlite_statement;
        std::map<std::string, int> statement_params;
    };
    std::map<std::string, statement_t> m_statements;

    sqlite3_stmt* prepare_statement(statement_t& stmt_info, const std::map<std::string, db_value>& params, const char* queryID = nullptr)
    {
        if (!m_db)
            return nullptr;

        auto stmt = stmt_info.sqlite_statement;

        sqlite3_reset(stmt);
        
        for (const auto& pair : params)
        {
            auto param_iter = stmt_info.statement_params.find(pair.first);
            //const int param_index = sqlite3_bind_parameter_index(stmt, (std::string(":") + pair.first).c_str());
            if (param_iter == stmt_info.statement_params.end())
            {
                LOG(ERROR) << "Parameter " << pair.first << " does not exist in query " << (queryID ? queryID : "");
                return nullptr;
            }
            const int param_index = param_iter->second;
            //statement_params.erase(pair.first);

            int error = 0;
            if (auto pval = pair.second.get_if<int32_t>())
                error = sqlite3_bind_int(stmt, param_index, *pval);
            else if (auto pval = pair.second.get_if<int64_t>())
                error = sqlite3_bind_int64(stmt, param_index, *pval);
            if (auto pval = pair.second.get_if<uint32_t>())
                error = sqlite3_bind_int(stmt, param_index, *pval);
            else if (auto pval = pair.second.get_if<uint64_t>())
                error = sqlite3_bind_int64(stmt, param_index, *pval);
            else if (auto pval = pair.second.get_if<std::string>()) // Text is expected to be UTF8
                error = sqlite3_bind_text(stmt, param_index, pval->c_str(), (int)pval->size(), nullptr);
            else if (auto pval = pair.second.get_if<std::vector<uint8_t>>())
                error = sqlite3_bind_blob(stmt, param_index, pval->data(), (int)pval->size(), nullptr);

            if (error != 0)
                return nullptr;
        }

        //if (!statement_params.empty())
        //{
        //    std::string error = "Missing query parameters: ";
        //    for(auto& pair : statement_params)
        //    {
        //        if (!error.empty()) error += ", ";
        //        error += pair.first;
        //    }
        //    LOG(ERROR) << error;
        //    return nullptr;
        //}

        return stmt;
    }

    void populate_statement_params(statement_t& stmt_info)
    {
        for (int i = 1; i <= sqlite3_bind_parameter_count(stmt_info.sqlite_statement); ++i)
        {
            // Snip the preceding symbol, as it can be anything
            std::string name = sqlite3_bind_parameter_name(stmt_info.sqlite_statement, i) + 1;
            stmt_info.statement_params[name] = i;
        }
    }
    
    sqlite3_stmt* prepare_statement_from_id(const std::string& queryID, const std::map<std::string, db_value>& params)
    {
        if (!m_db)
            return nullptr;

        auto iter = m_statements.find(queryID);
        if (iter == m_statements.end())
        {
            LOG(ERROR) << "Trying to run unknown query " << queryID;
            return nullptr;
        }        

        return prepare_statement(iter->second, params, queryID.c_str());
    }

    sqlite3_stmt* prepare_statement_from_text(const std::string& text, const std::map<std::string, db_value>& params)
    {
        if (!m_db)
            return nullptr;

        statement_t stmt;
        const int error = sqlite3_prepare_v2(m_db, text.c_str(), (int)text.size(), &stmt.sqlite_statement, nullptr);
        if (error != 0)
            return nullptr;

        populate_statement_params(stmt);

        return prepare_statement(stmt, params);
    }
};

struct cursor::details
{
    ~details() {}

    sqlite3_stmt* m_stmt = nullptr;
    bool m_columns_populated = false;
    //std::map<std::string, db_value> m_column_values;
    struct column_t { std::string name; int type; db_value value; };
    std::vector<column_t> m_columns;
    int m_result = 0;
};

persistent_storage::persistent_storage()
{}

persistent_storage::~persistent_storage()
{
    close();
}

bool persistent_storage::open(const std::string& path, bool create)
{
    m_details = std::make_unique<details>();

    sqlite3_initialize();
    sqlite3_config(SQLITE_CONFIG_LOG, logger, 0);
    // TODO: Examine possible use of SQLITE_CONFIG_SINGLETHREAD for read-only connections
    sqlite3_config(SQLITE_CONFIG_MULTITHREAD, 0, 0);
    //sqlite3_config(SQLITE_CONFIG_SINGLETHREAD, 1, 0);

    const int error = sqlite3_open_v2(path.c_str(), &m_details->m_db, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), 0);
    if (error != 0)
    {
        sqlite3_close(m_details->m_db);
        m_details.reset();        
        return false;
    }

    return true;
}

void persistent_storage::close()
{
    if (!m_details)
        return;    

    for(auto& pair : m_details->m_statements)
        sqlite3_finalize(pair.second.sqlite_statement);
    m_details->m_statements.clear();

    if (m_details->m_db)
        sqlite3_close(m_details->m_db);
    m_details->m_db = nullptr;

    m_details.reset();

    sqlite3_shutdown();
}

bool persistent_storage::is_open() const
{
    return m_details && m_details->m_db;
}

bool persistent_storage::save(const std::string& path)
{
    if (!is_open())
        return false;

    if (std::filesystem::exists(path))
        std::filesystem::remove(path);

    sqlite3* fileDB;
    if (sqlite3_open(path.c_str(), &fileDB) != SQLITE_OK)
        return false;

    auto backup = sqlite3_backup_init(fileDB, "main", m_details->m_db, "main");
    if (backup == nullptr)
        return false;
    auto backup_res = sqlite3_backup_step(backup, -1);
    if (backup_res != SQLITE_OK && backup_res != SQLITE_DONE)
        return false;
    if (sqlite3_backup_finish(backup) != SQLITE_OK)
        return false;

    if (sqlite3_close(fileDB) != SQLITE_OK)
        return false;

    return true;
}

bool persistent_storage::register_query(const std::string& queryID, const std::string& text)
{
    if (queryID.empty())
    {
        LOG(WARNING) << "Trying to register query with empty ID and text '" << text << ";";
        return false;
    }

    if (text.empty())
    {
        LOG(WARNING) << "Trying to register query " << queryID << "with empty text";
        return false;
    }

    if (!m_details || !m_details->m_db)
    {
        LOG(WARNING) << "Trying to register query " << queryID << " before storage is ready";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const int error = sqlite3_prepare_v2(m_details->m_db, text.c_str(), (int)text.size(), &stmt, nullptr);
    if (error != 0)
    {
        LOG(ERROR) << "SQL error while registering query " << queryID << ": " << sqlite3_errmsg(m_details->m_db);
        return false;
    }

    auto& stmt_info = m_details->m_statements[queryID];
    stmt_info.sqlite_statement = stmt;

    m_details->populate_statement_params(stmt_info);

    return true;
}

bool persistent_storage::query(const std::string& queryID, const std::map<std::string, db_value>& params)
{
    // Reimplement some logic here for optimization - avoid unnecessar cursor creation

    if (!m_details || !m_details->m_db)
    {
        LOG(WARNING) << "Trying to run query " << queryID << " before storage is ready";
        return {};
    }

    //cursor result;
    // Save params into cursor: we need to store string/vector references alive
    //result.m_saved_params = params;

    // Use **m_saved_params** here, or references to strings/vectors might become invalidated!
    auto* stmt = m_details->prepare_statement_from_id(queryID, params);
    if (!stmt)
        return false;

    return sqlite3_step(stmt) == SQLITE_DONE;

    //auto cursor = query_data(queryID, params);
    //if (!cursor.is_valid())
    //    return false;

    //cursor.next();

    //return cursor.is_done();
}

cursor persistent_storage::query_data(const std::string& queryID, const std::map<std::string, db_value>& params)
{
    if (!m_details || !m_details->m_db)
    {
        LOG(WARNING) << "Trying to run query " << queryID << " before storage is ready";
        return {};
    }

    cursor result;
    // Save params into cursor: we need to store string/vector references alive
    result.m_saved_params = params;

    // Use **m_saved_params** here, or references to strings/vectors might become invalidated!
    auto* stmt = m_details->prepare_statement_from_id(queryID, result.m_saved_params);
    if (!stmt)
        return {};
    
    result.m_details->m_stmt = stmt;
    return result;
}

bool persistent_storage::query_immediate(const std::string& query_text, const std::map<std::string, db_value>& params)
{
    if (!m_details || !m_details->m_db)
    {
        LOG(WARNING) << "Trying to run immediate query " << query_text << " before storage is ready";
        return false;
    }

    auto* stmt = m_details->prepare_statement_from_text(query_text, params);
    if (!stmt)
        return false;

    cursor c;
    c.m_details->m_stmt = stmt;
    c.next();
    if (!c.is_done())
    {
        sqlite3_finalize(stmt);
        return false;
    }
    
    sqlite3_finalize(stmt);

    return true;
}

cursor persistent_storage::query_data_immediate(const std::string& query_text, const std::map<std::string, db_value>& params)
{
    if (!m_details || !m_details->m_db)
    {
        LOG(WARNING) << "Trying to run immediate query " << query_text << " before storage is ready";
        return {};
    }

    cursor result;
    // Save params into cursor: we need to store string/vector references alive
    result.m_saved_params = params;

    // Use **m_saved_params** here, or references to strings/vectors might become invalidated!
    auto* stmt = m_details->prepare_statement_from_text(query_text, result.m_saved_params);
    if (!stmt)
        return {};
    
    result.m_details->m_stmt = stmt;
    return result;
}

uint64_t persistent_storage::get_last_inserted_id() const
{
    if (!m_details || !m_details->m_db)
        return 0;

    return sqlite3_last_insert_rowid(m_details->m_db);
}

std::string persistent_storage::get_last_error() const
{
    if (!m_details || !m_details->m_db)
        return "";

    return sqlite3_errmsg(m_details->m_db);
}

bool persistent_storage::pragma(const char* text)
{
    return sqlite3_exec(m_details->m_db, text, 0, 0, 0) == SQLITE_OK;
    //int log, fr;
    //sqlite3_wal_checkpoint_v2(m_details->m_db, 0, SQLITE_CHECKPOINT_FULL, &log, &fr);
}

cursor::cursor()
{
    m_details = std::make_unique<details>();
}

cursor::~cursor()
{
}

cursor::cursor(cursor&& other)
{
    m_details = std::move(other.m_details);
    m_saved_params = std::move(other.m_saved_params);
}

bool cursor::is_valid() const
{
    return m_details && m_details->m_stmt != nullptr;
}

bool cursor::next()
{
    if (!m_details || !m_details->m_stmt)
        return false;

    m_details->m_columns_populated = false;

    m_details->m_result = sqlite3_step(m_details->m_stmt);
    return (m_details->m_result == SQLITE_ROW);
}

bool cursor::is_done() const
{
    if (!m_details || !m_details->m_stmt)
        return true;    

    return m_details->m_result == SQLITE_DONE;
}

bool cursor::has_error() const
{
    return !m_details || (m_details->m_result != SQLITE_OK && m_details->m_result != SQLITE_DONE && m_details->m_result != SQLITE_ROW);
}

int32_t cursor::get_int(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get integer field " << column_name << " from a query where it doesn't exist";
        return 0;
    }

    auto* pval = value->get_if<int64_t>();
    // SQLite only stores 64-bit integers, so downsize it
    return pval ? (int32_t)(*pval) : 0;
}

int64_t cursor::get_int64(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get integer64 field " << column_name << " from a query where it doesn't exist";
        return 0;
    }

    auto* pval = value->get_if<int64_t>();
    return pval ? *pval : 0;
}

uint32_t cursor::get_uint(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get unsigned integer field " << column_name << " from a query where it doesn't exist";
        return 0;
    }

    auto* pval = value->get_if<int64_t>();
    // SQLite only stores 64-bit integers, so downsize it
    return pval ? (uint32_t)(*pval) : 0;
}

uint64_t cursor::get_uint64(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get unsigned integer 64 field " << column_name << " from a query where it doesn't exist";
        return 0;
    }

    auto* pval = value->get_if<int64_t>();
    return pval ? (uint64_t)*pval : 0;
}

std::string cursor::get_string(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get string field " << column_name << " from a query where it doesn't exist";
        return "";
    }

    auto* pval = value->get_if<std::string>();
    return pval ? *pval : "";
}

std::vector<uint8_t> cursor::get_blob(const std::string& column_name) const
{
    auto* value = get_value(column_name);
    if (!value)
    {
        LOG(ERROR) << "Trying to get blob field " << column_name << " from a query where it doesn't exist";
        return {};
    }

    auto* pval = value->get_if<std::vector<uint8_t>>();
    return pval ? *pval : std::vector<uint8_t>();
}

void cursor::populate_columns() const
{
    if (!m_details || !m_details->m_stmt)
        return;

    if (m_details->m_columns.empty())
    {
        for (int i = 0; i < sqlite3_column_count(m_details->m_stmt); ++i)
        {
            const char* column_name = sqlite3_column_name(m_details->m_stmt, i);
            if (!column_name)
                return;

            auto column_type = sqlite3_column_type(m_details->m_stmt, i);

            m_details->m_columns.push_back({ column_name, column_type, {} });
        }
    }
    else // Update column types every time because they can change (e.g. NULL column can become non-NULL, and type is take from the current row's value :( )
    {
        for (int i = 0; i < sqlite3_column_count(m_details->m_stmt); ++i)
        {
            auto column_type = sqlite3_column_type(m_details->m_stmt, i);

            m_details->m_columns[i].type = column_type;
        }
    }

    for(int i = 0; i < sqlite3_column_count(m_details->m_stmt); ++i)
    {
        auto& column = m_details->m_columns[i];

        if (column.type == SQLITE_INTEGER)
            column.value = (int64_t)sqlite3_column_int64(m_details->m_stmt, i);
        // Column "initial type" should never be null?
        if (column.type == SQLITE_NULL)
            column.value = db_value();
        else if (column.type == SQLITE_TEXT)
        {
            const unsigned char* text = sqlite3_column_text(m_details->m_stmt, i);
            column.value = text ? std::string((const char*)text) : std::string();
        }
        else if (column.type == SQLITE_BLOB)
        {
            const unsigned char* data = (const unsigned char*)sqlite3_column_blob(m_details->m_stmt, i);
            int data_size = sqlite3_column_bytes(m_details->m_stmt, i);
            std::vector<uint8_t> data_vector(data_size);
            if (data && data_size)
                memcpy(&data_vector[0], data, data_size);
            column.value = data_vector;
        }
    }

    m_details->m_columns_populated = true;
}

const db_value* cursor::get_value(const std::string column_name) const
{
    if (!m_details || !m_details->m_stmt)
        return nullptr;

    if (!m_details->m_columns_populated)
        populate_columns();

    auto column = std::find_if(m_details->m_columns.begin(), m_details->m_columns.end(), [&](auto& c) {return c.name == column_name; });
    if (column == m_details->m_columns.end())
        return nullptr;

    return &column->value;
}

transaction::transaction(persistent_storage& storage, transaction_behaviour default_behaviour)
    : m_storage(storage)
    , m_default_behaviour(default_behaviour)
{
    sqlite3_exec(m_storage.m_details->m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
}

transaction::~transaction()
{
    if (m_default_behaviour == transaction_behaviour::commit)
        commit();
    else
        rollback();
}

bool transaction::commit()
{
    if (m_done)
        return false;

    sqlite3_exec(m_storage.m_details->m_db, "END TRANSACTION;", nullptr, nullptr, nullptr);
    m_done = true;
    return true;
}

bool transaction::rollback()
{
    if (m_done)
        return false;

    sqlite3_exec(m_storage.m_details->m_db, "ROLLBACK TRANSACTION;", nullptr, nullptr, nullptr);
    m_done = true;
    return true;
}

}
