#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace persistent_storage {

/**
    \brief A class that represents a value returned from the database
*/
class db_value
{
public:
    db_value()
        : m_type(empty)
    {}

    db_value(const int32_t& value) : m_type(t_i32) { m_pod_value.i32 = value; }
    db_value(const uint32_t& value) : m_type(t_ui32) { m_pod_value.ui32 = value; }
    db_value(const int64_t& value) : m_type(t_i64) { m_pod_value.i64 = value; }
    db_value(const uint64_t& value) : m_type(t_ui64) { m_pod_value.ui64 = value; }
    db_value(const char* value) : m_type(t_str) { m_string_value = value; }
    db_value(const std::string& value) : m_type(t_str) { m_string_value = value; }
    db_value(const std::vector<uint8_t>& value) : m_type(t_vec) { m_vector_value = value; }

    template<typename T>
    const T* get_if() const { return nullptr; }

private:
    enum
    {
        t_i32,
        t_ui32,
        t_i64,
        t_ui64,
        t_str,
        t_vec,
        empty
    } m_type = empty;

    union
    {
        int32_t i32;
        uint32_t ui32;
        int64_t i64;
        uint64_t ui64;
    } m_pod_value;
    std::string m_string_value;
    std::vector<uint8_t> m_vector_value;
};

template<> inline const int32_t* db_value::get_if() const { return m_type == t_i32 ? &m_pod_value.i32 : nullptr; }
template<> inline const uint32_t* db_value::get_if() const { return m_type == t_ui32 ? &m_pod_value.ui32 : nullptr; }
template<> inline const int64_t* db_value::get_if() const { return m_type == t_i64 ? &m_pod_value.i64 : nullptr; }
template<> inline const uint64_t* db_value::get_if() const { return m_type == t_ui64 ? &m_pod_value.ui64 : nullptr; }
template<> inline const std::string* db_value::get_if() const { return m_type == t_str ? &m_string_value : nullptr; }
template<> inline const std::vector<uint8_t>* db_value::get_if() const { return m_type == t_vec ? &m_vector_value : nullptr; }

/**
    \brief Database cursor that can be used to iterate over the results returned by a query
*/
class cursor
{
public:
    cursor();
    ~cursor();

    cursor(cursor&& other);

    cursor(const cursor& other) = delete;
    void operator=(const cursor& other) = delete;

    bool is_valid() const;
    bool next();
    bool is_done() const;
    bool has_error() const;

    int32_t get_int(const std::string& column_name) const;
    int64_t get_int64(const std::string& column_name) const;
    uint32_t get_uint(const std::string& column_name) const;
    uint64_t get_uint64(const std::string& column_name) const;
    std::string get_string(const std::string& column_name) const;    
    std::vector<uint8_t> get_blob(const std::string& column_name) const;

private:    
    void populate_columns() const;
    const db_value* get_value(const std::string column_name) const;

    struct details;

    friend class persistent_storage;
    std::unique_ptr<details> m_details;

    // Necessary, because we must keep references to string/vector params alive
    // while the cursor is alive
    std::map<std::string, db_value> m_saved_params;
};

/**
    \brief Database wrapper class (Sqlite)
*/
class persistent_storage
{
public:
    persistent_storage();
    ~persistent_storage();

    /**
        \brief Opens Sqlite database from the specified path. If create is specified, creates it if it's not present
    */
    bool open(const std::string& path, bool create);
    /**
        \brief Closes the database
    */
    void close();
    /**
        \brief Returns whether the class has an open database and can be used
    */
    bool is_open() const;

    /**
        \brief Saves database to the specified file. Can be used with memory and temporary databases
    */
    bool save(const std::string& path);

    /**
        \brief Registers a named query (prepared statement)
    */
    bool register_query(const std::string& queryID, const std::string& text);
    /**
        \brief Runs a query where the user do not care about results, but only about the sucess of the query (e.g. INSERT, DELETE or UPDATE)
    */
    bool query(const std::string& queryID, const std::map<std::string, db_value>& params);
    /**
        \brief Runs a query where the user is interested in data returned (e.g. SELECT)
    */
    cursor query_data(const std::string& queryID, const std::map<std::string, db_value>& params);

    /**
        \brief Runs an unnamed query, compiling a statement on the fly

        \important Not recommended for use outside of basic DB maintenance operations
    */
    bool query_immediate(const std::string& query_text, const std::map<std::string, db_value>& params);

    /**
        \brief Runs an unnamed query, compiling a statement on the fly, returning a cursor

        \important Not recommended for use outside of basic DB maintenance operations
    */
    cursor query_data_immediate(const std::string& query_text, const std::map<std::string, db_value>& params);

    /**
        \brief Returns the last row ID generated by an INSERT operation
    */
    uint64_t get_last_inserted_id() const;

    /**
        \bried Returns the last error reported by the underlying implementation, if any
    */
    std::string get_last_error() const;

    /**
        \bried Sets database pragmas
    */
    bool pragma(const char* text);

private:
    struct details;

    friend class transaction;
    std::unique_ptr<details> m_details;
};

enum class transaction_behaviour { rollback, commit };
/**
    \brief RAII wrapper that allows execution of transactions

    As long as the created instance is alive, all transactions will be queued, and
    will only be executed when the instance is destroyed (if transaction_behaviour::commit is specified),
    or if commit() function is called.
*/
class transaction
{
public:
    /**
        \brief Begins a new transaction with the specified behaviour in case of instance destruction

        if transaction_behaviour::rollback is specified, then queries executed before instance's destruction will be cancelled, unless commit() was called
        if transaction_behaviour::commit is specified, then queries executed before instance's destruction will be executed, unless rollback() was called
    */
    transaction(persistent_storage& storage, transaction_behaviour default_behaviour);
    ~transaction();

    /**
        \brief Cancels executing of all queries in the transaction
    */
    bool rollback();
    /**
        \brief Executes all queries in the transaction
    */
    bool commit();

private:
    bool m_done = false;
    transaction_behaviour m_default_behaviour;
    persistent_storage& m_storage;
};

}
