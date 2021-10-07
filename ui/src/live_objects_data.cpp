#include "live_objects_data.h"
#include "common_ui.h"

bool live_objects_data::export_to_csv(const std::string& file)
{
    FILE* f = fopen(file.c_str(), "w");
    if (f == nullptr)
        return false;

    fprintf(f, "Type;Count;Size\n");

    for (auto& t : types)
    {
        fprintf(f, "\"%s\";%I64u;%I64u\n", m_data->get_type_name(t.type), t.count, t.size);
    }

    fclose(f);

    return true;
}

void live_objects_by_type_model::init(live_objects_data* data)
{
    m_data = data;
}

int live_objects_by_type_model::rowCount(const QModelIndex&) const
{
    return (int)m_rows.size();
}

int live_objects_by_type_model::columnCount(const QModelIndex&) const
{
    return 3;
}

QVariant live_objects_by_type_model::data(const QModelIndex& index, int role) const
{
    if (role != Qt::DisplayRole && role != role::Type && role != role::Size)
        return QVariant();

    int row = index.row();
    int col = index.column();
    if (row < 0 || row >= m_rows.size()) return QVariant();
    if (col < 0 || col >= 3) return QVariant();

    if (role == role::Type)
        return m_rows[row].type;
    if (role == role::Size)
        return m_rows[row].size;

    if (col == 0)
        return m_data->get_type_name(m_rows[row].type);
    else if (col == 1)
        return m_rows[row].count;
    else
        return size_to_string(m_rows[row].size);
}

QVariant live_objects_by_type_model::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    if (section == 0)
        return "Type";
    if (section == 1)
        return "Count";
    if (section == 2)
        return "Size";

    return QVariant();
}

void live_objects_by_type_model::sort(int column, Qt::SortOrder order)
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
    else if (column == 1)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return r1.count < r2.count;
            else
                return r1.count > r2.count;
        };
    }
    else if (column == 2)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return r1.size < r2.size;
            else
                return r1.size > r2.size;
        };
    }
    else
        return;

    std::sort(m_rows.begin(), m_rows.end(), comparer);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void live_objects_by_type_model::update()
{
    clear();

    if (m_data == nullptr || m_data->types.size() == 0)
        return;

    beginInsertRows(QModelIndex(), 0, (int)m_data->types.size() - 1);

    for (auto& t : m_data->types)
    {
        m_rows.push_back({ t.type, t.count, t.size });
        m_total_size += t.size;
    }

    endInsertRows();

    sort(2, Qt::DescendingOrder);
    emit dataChanged(QModelIndex(), QModelIndex());
}

void live_objects_by_type_model::clear()
{
    m_total_size = 0;

    if (m_rows.empty())
        return;

    beginRemoveRows(QModelIndex(), 0, (int)m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    emit dataChanged(QModelIndex(), QModelIndex());
}

QModelIndex live_objects_by_type_model::find_type(uint64_t type) const
{    
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        if (m_rows[i].type == type)
            return index(i, 0);
    }

    return QModelIndex();
}

void live_callstacks_by_type_model::init(live_objects_data* data)
{
    m_data = data;
}

int live_callstacks_by_type_model::rowCount(const QModelIndex&) const
{
    return (int)m_rows.size();
}

int live_callstacks_by_type_model::columnCount(const QModelIndex&) const
{
    return 3;
}

QVariant live_callstacks_by_type_model::data(const QModelIndex& index, int role) const
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
    else if (col == 1)
        return m_rows[row].count;
    else
        return size_to_string(m_rows[row].size);
}

QVariant live_callstacks_by_type_model::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    if (section == 0)
        return "Callstack";
    if (section == 1)
        return "Count";
    if (section == 2)
        return "Size";

    return QVariant();
}

void live_callstacks_by_type_model::sort(int column, Qt::SortOrder order)
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
    else if (column == 1)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return r1.count < r2.count;
            else
                return r1.count > r2.count;
        };
    }
    else if (column == 2)
    {
        comparer = [=](auto& r1, auto& r2) -> bool
        {
            if (order == Qt::AscendingOrder)
                return r1.size < r2.size;
            else
                return r1.size > r2.size;
        };
    }
    else
        return;

    std::sort(m_rows.begin(), m_rows.end(), comparer);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void live_callstacks_by_type_model::update(uint64_t type, std::function<void(size_t cur, size_t max)> callback)
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
        m_rows.push_back({ t.callstack, t.count, t.size, t.addresses });
        if (callback != nullptr)
            callback(counter++, max);
    }

    endInsertRows();

    sort(2, Qt::DescendingOrder);

    emit dataChanged(QModelIndex(), QModelIndex());
}

void live_callstacks_by_type_model::clear()
{
    if (m_rows.empty())
        return;

    beginRemoveRows(QModelIndex(), 0, (int)m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    emit dataChanged(QModelIndex(), QModelIndex());
}

QModelIndex live_callstacks_by_type_model::find_callstack(uint64_t callstack) const
{
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        if (m_rows[i].callstack == callstack)
            return index(i, 0);
    }

    return QModelIndex();
}

const std::vector<uint64_t>* live_callstacks_by_type_model::get_addresses(const QModelIndex& index)
{
    int row = index.row();
    int col = index.column();
    if (row < 0 || row >= m_rows.size()) return nullptr;
    if (col < 0 || col >= 3) return nullptr;

    return &m_rows[row].addresses;
}

void live_objects_data::init(owlcat::mono_profiler_client_data* data)
{
    m_data = data;
}

void live_objects_data::update(uint64_t from_frame, uint64_t to_frame, owlcat::progress_func_t progress_func)
{
    if (m_data == nullptr)
        return;

    //auto t1 = std::chrono::system_clock::now();
    std::vector<owlcat::live_object> result;
    m_data->get_live_objects(result, from_frame, to_frame, progress_func);
    //auto t2 = std::chrono::system_clock::now();

    types.clear();

    std::unordered_map<uint64_t, uint64_t> type_indices;
    for (auto& o : result)
    {
        auto type_index_iter = type_indices.find(o.type_id);
        if (type_index_iter == type_indices.end())
        {
            types.push_back({ o.type_id, 0 });
            type_index_iter = type_indices.insert(std::make_pair(o.type_id, types.size() - 1)).first;
        }

        auto& type = types[type_index_iter->second];
        ++type.count;
        type.size += o.size;

        auto callstack_iter = std::find_if(type.callstacks.begin(), type.callstacks.end(), [&o](auto& callstack) {return callstack.callstack == o.callstack_id; });
        if (callstack_iter == type.callstacks.end())
            type.callstacks.push_back({ o.callstack_id, 1, o.size, {o.addr} });
        else
        {
            ++callstack_iter->count;
            callstack_iter->size += o.size;
            callstack_iter->addresses.push_back(o.addr);
        }
    }
}
