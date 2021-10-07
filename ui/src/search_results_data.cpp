#include "search_results_data.h"
#include "common_ui.h"

void search_results_types_model::init(search_results_data* data)
{
    m_data = data;
}

int search_results_types_model::rowCount(const QModelIndex&) const
{
    return (int)m_rows.size();
}

int search_results_types_model::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant search_results_types_model::data(const QModelIndex& index, int role) const
{
    if (role != Qt::DisplayRole && role != role::Type)
        return QVariant();

    int row = index.row();
    int col = index.column();
    if (row < 0 || row >= m_rows.size()) return QVariant();
    if (col < 0 || col >= 3) return QVariant();

    if (role == role::Type)
        return m_rows[row].type;

    if (col == 0)
        return m_data->get_type_name(m_rows[row].type);

    return QVariant();
}

QVariant search_results_types_model::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    if (section == 0)
        return "Type";

    return QVariant();
}

void search_results_types_model::sort(int column, Qt::SortOrder order)
{
    std::function<bool(row_data&, row_data&)> comparer;
    if (column == 0)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return strcmp(m_data->get_type_name(r1.type), m_data->get_type_name(r2.type)) < 0;
            else
                return strcmp(m_data->get_type_name(r1.type), m_data->get_type_name(r2.type)) > 0;
        };
    }
    else
        return;

    std::sort(m_rows.begin(), m_rows.end(), comparer);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void search_results_types_model::update()
{
    clear();

    if (m_data == nullptr || m_data->types.size() == 0)
        return;

    beginInsertRows(QModelIndex(), 0, (int)m_data->types.size() - 1);

    for (auto& t : m_data->types)
    {
        m_rows.push_back({ t.type });
    }

    endInsertRows();

    sort(2, Qt::DescendingOrder);
    emit dataChanged(QModelIndex(), QModelIndex());
}

void search_results_types_model::clear()
{
    if (m_rows.empty())
        return;

    beginRemoveRows(QModelIndex(), 0, (int)m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    emit dataChanged(QModelIndex(), QModelIndex());
}

void search_results_callstacks_model::init(search_results_data* data)
{
    m_data = data;
}

int search_results_callstacks_model::rowCount(const QModelIndex&) const
{
    return (int)m_rows.size();
}

int search_results_callstacks_model::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant search_results_callstacks_model::data(const QModelIndex& index, int role) const
{
    if (role != Qt::DisplayRole && role != role::Callstack)
        return QVariant();

    int row = index.row();
    int col = index.column();
    if (row < 0 || row >= m_rows.size()) return QVariant();
    if (col < 0 || col >= 3) return QVariant();

    if (role == role::Callstack)
        return m_rows[row].callstack;

    if (col == 0)
        return m_data->get_callstack(m_rows[row].callstack);

    return QVariant();
}

QVariant search_results_callstacks_model::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    if (section == 0)
        return "Callstack";

    return QVariant();
}

void search_results_callstacks_model::sort(int column, Qt::SortOrder order)
{
    std::function<bool(row_data&, row_data&)> comparer;
    if (column == 0)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return strcmp(m_data->get_callstack(r1.callstack), m_data->get_callstack(r2.callstack)) < 0;
            else
                return strcmp(m_data->get_callstack(r1.callstack), m_data->get_callstack(r2.callstack)) > 0;
        };
    }
    else
        return;

    std::sort(m_rows.begin(), m_rows.end(), comparer);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void search_results_callstacks_model::update(uint64_t type, std::function<void(size_t cur, size_t max)> callback)
{
    clear();

    auto iter = std::find_if(m_data->types.begin(), m_data->types.end(), [&](auto& t) {return t.type == type; });
    if (iter == m_data->types.end())
        return;

    if (iter->callstacks.size() == 0)
        return;

    size_t max = iter->callstacks.size();

    beginInsertRows(QModelIndex(), 0, (int)iter->callstacks.size() - 1);

    size_t counter = 0;
    for (auto& t : iter->callstacks)
    {
        m_rows.push_back({ t.callstack });
        if (callback != nullptr)
            callback(counter++, max);
    }

    endInsertRows();

    sort(2, Qt::DescendingOrder);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void search_results_callstacks_model::clear()
{
    if (m_rows.empty())
        return;

    beginRemoveRows(QModelIndex(), 0, (int)m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    emit dataChanged(QModelIndex(), QModelIndex());
}

const std::vector<uint64_t>* search_results_callstacks_model::get_addresses(const QModelIndex& index)
{
    int row = index.row();
    int col = index.column();
    if (row < 0 || row >= m_rows.size()) return nullptr;
    if (col < 0 || col >= 3) return nullptr;

    return &m_rows[row].addresses;
}

void search_results_data::init(owlcat::mono_profiler_client_data* data)
{
    m_data = data;
}

void search_results_data::search_address_list(const std::vector<uint64_t>& addresses, owlcat::progress_func_t progress_func)
{
    if (m_data == nullptr)
        return;

    types.clear();

    std::unordered_map<uint64_t, uint64_t> type_indices;
    for(auto addr : addresses)
    {
        uint64_t type_id = std::numeric_limits<uint64_t>::max(), stack_id = std::numeric_limits<uint64_t>::max();
        m_data->get_allocation_type_and_stack(addr, type_id, stack_id);
        if (type_id == std::numeric_limits<uint64_t>::max())
            continue;

        auto type_index_iter = type_indices.find(type_id);
        if (type_index_iter == type_indices.end())
        {
            types.push_back({ type_id });
            type_index_iter = type_indices.insert(std::make_pair(type_id, types.size() - 1)).first;
        }

        auto& type = types[type_index_iter->second];

        auto callstack_iter = std::find_if(type.callstacks.begin(), type.callstacks.end(), [stack_id](auto& callstack) {return callstack.callstack == stack_id; });
        if (callstack_iter == type.callstacks.end())
            type.callstacks.push_back({ stack_id });
        else
            callstack_iter->addresses.push_back(stack_id);
    }
}
