#include "graphs_data.h"

graphs_data::graphs_data(owlcat::mono_profiler_client* client)
    : m_data(client->get_data())
{
}

void graphs_data::init()
{
    update_boundaries();
}

void graphs_data::clear()
{
    min_frame = 0;
    max_frame = 0;
    max_allocs = 0;
    max_frees = 0;
    max_size = 0;
    m_alloc_count.clear();
    m_frees_count.clear();
    m_size_points.clear();
    first_visible_frame = -1;
    last_visible_frame = -1;
}

void graphs_data::update_boundaries()
{
    m_data->get_frame_boundaries(min_frame, max_frame);
}

void graphs_data::update_region(int from, int to)
{
    first_visible_frame = from;
    last_visible_frame = to;
    m_data->get_frame_stats(m_alloc_count, m_frees_count, max_allocs, max_frees, m_size_points, max_size, from, to);
}

size_t graphs_data::get_allocations_count_size() { return m_alloc_count.size(); }

QwtIntervalSample graphs_data::get_allocations_count(uint64_t frame)
{
    if (frame >= m_alloc_count.size())
        return QwtIntervalSample();

    return QwtIntervalSample(m_alloc_count[frame], frame + first_visible_frame, frame + first_visible_frame + 1);
}

size_t graphs_data::get_frees_count_size() { return m_frees_count.size(); }
QwtIntervalSample graphs_data::get_frees_count(uint64_t frame)
{
    if (frame >= m_frees_count.size())
        return QwtIntervalSample();

    return QwtIntervalSample(m_frees_count[frame], frame + first_visible_frame, frame + first_visible_frame + 1);
}

size_t graphs_data::get_sizes_size() { return m_size_points.size(); }
QPointF graphs_data::get_size(uint64_t frame)
{
    if (frame >= m_size_points.size())
        return QPointF();

    return QPointF(frame + first_visible_frame, m_size_points[frame]);
}

uint64_t graphs_data::get_closest_gc_frame(uint64_t frame) const
{
    if (frame <= first_visible_frame || frame >= last_visible_frame)
        return frame;

    bool f1found = false, f2found = false;
    uint64_t f1 = frame;
    uint64_t f2 = frame;
    while (f1 > first_visible_frame)
    {
        if (m_frees_count[f1 - first_visible_frame] > 0)
        {
            f1found = true;
            break;
        }
        --f1;
    }
    while (f2 < last_visible_frame)
    {
        if (m_frees_count[f2 - first_visible_frame] > 0)
        {
            f2found = true;
            break;
        }
        ++f2;
    }

    if (!f1found && !f2found)
        return frame;

    if (f1found != f2found)
        return f1found ? f1 : f2;

    if (frame - f1 > f2 - frame)
        return f2;

    return f1;
}

bool graphs_data::get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id)
{
    return m_data->get_allocation_type_and_stack(address, type_id, stack_id);
}
