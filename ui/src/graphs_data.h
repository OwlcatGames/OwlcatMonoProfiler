#pragma once
#include <vector>
#include "qwt_samples.h"

#include "mono_profiler_client.h"

/*
    Class that supplies data for Allocations cound and Memory size graphs
*/
class graphs_data
{
private:
    owlcat::mono_profiler_client_data* m_data;

    std::vector<uint64_t> m_alloc_count;
    std::vector<uint64_t> m_frees_count;
    std::vector<uint64_t> m_size_points;

public:
    int first_visible_frame = -1;
    int last_visible_frame = -1;

    uint64_t min_frame = 0;
    uint64_t max_frame = 0;

    uint64_t max_allocs = 0;
    uint64_t max_frees = 0;
    int64_t max_size = 0;

    graphs_data(owlcat::mono_profiler_client* client);

    void init();
    void clear();

    void update_boundaries();
    void update_region(int from, int to);

    size_t get_allocations_count_size();
    QwtIntervalSample get_allocations_count(uint64_t frame);

    size_t get_frees_count_size();
    QwtIntervalSample get_frees_count(uint64_t frame);

    size_t get_sizes_size();
    QPointF get_size(uint64_t frame);

    // Searches for a frame where there were any deallocations that is closest to the specified frame (for "Snap to GC" option)
    uint64_t get_closest_gc_frame(uint64_t frame) const;

    bool get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id);
    //bool get_allocations_type_and_stack(const std::vector<uint64_t>& addresses, std::vector<owlcat::search_result_t>& results);
};
