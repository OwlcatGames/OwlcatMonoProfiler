#pragma once
#include <vector>

#include <qabstractitemmodel.h>

#include "mono_profiler_client.h"

/*
    A base class that provides data for models used by search results lists
*/
struct search_results_data
{
private:
    owlcat::mono_profiler_client_data* m_data = nullptr;

public:
    void init(owlcat::mono_profiler_client_data* data);

    void search_address_list(const std::vector<uint64_t>& addresses, owlcat::progress_func_t progress_func);

    const char* get_type_name(uint64_t type_id) const { return m_data->get_type_name(type_id); }
    const char* get_callstack(uint64_t callstack_id) const { return m_data->get_callstack(callstack_id); }

    struct callstack_data
    {
        uint64_t callstack;
        //uint64_t count;
        //uint64_t size;

        std::vector<uint64_t> addresses;
    };

    struct type_data
    {
        uint64_t type;
        //uint64_t count;
        //uint64_t size;

        std::vector<callstack_data> callstacks;
    };
    std::vector<type_data> types;
};

/*
    Model used by "search result types" list
*/
class search_results_types_model : public QAbstractTableModel
{
    search_results_data* m_data = nullptr;
    struct row_data
    {
        uint64_t type;
        //uint64_t count;
        //uint64_t size;
    };
    std::vector<row_data> m_rows;
    //uint64_t m_total_size = 0;

public:
    enum role
    {
        Type = Qt::UserRole + 0,
        //Size = Qt::UserRole + 1,
    };

public:
    void init(search_results_data* data);

    int rowCount(const QModelIndex& index) const override;
    int columnCount(const QModelIndex& index) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    void update();
    void clear();

    //uint64_t get_total_size() const { return m_total_size; }
};

/*
    Model used by "live objects by callstack for type" list
*/
class search_results_callstacks_model : public QAbstractTableModel
{
public:
    enum role
    {
        Callstack = Qt::UserRole + 0,
    };

private:
    search_results_data* m_data;
    struct row_data
    {
        uint64_t callstack;
        //uint64_t count;
        //uint64_t size;

        std::vector<uint64_t> addresses;
    };
    std::vector<row_data> m_rows;
public:
    void init(search_results_data* data);

    int rowCount(const QModelIndex& index) const override;
    int columnCount(const QModelIndex& index) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    void update(uint64_t type, std::function<void(size_t cur, size_t max)> callback);
    void clear();

    const std::vector<uint64_t>* get_addresses(const QModelIndex& index);
};
