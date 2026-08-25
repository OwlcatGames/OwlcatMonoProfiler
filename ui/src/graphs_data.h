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
    // Whole-process committed memory per frame (see SRV_MEMSTATS). Overlaid on the size graph;
    // the gap above the tracked-size line is native allocator pool overhead.
    std::vector<uint64_t> m_committed_points;
    std::vector<uint64_t> m_working_set_points;
    std::vector<uint64_t> m_gc_heap_points;
    // Managed collections in the visible range, as reported by the server
    std::vector<owlcat::gc_event_t> m_gc_events;

public:
    int first_visible_frame = -1;
    int last_visible_frame = -1;

    uint64_t min_frame = 0;
    uint64_t max_frame = 0;

    uint64_t max_allocs = 0;
    uint64_t max_frees = 0;
    int64_t max_size = 0;
    uint64_t max_committed = 0;

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

    // Committed-memory line (process-wide), same x-alignment as the size line
    size_t get_committed_size();
    QPointF get_committed(uint64_t frame);

    // Searches for a frame where there were any deallocations that is closest to the specified frame (for "Snap to GC" option)
    uint64_t get_closest_gc_frame(uint64_t frame) const;

    /*
        Managed collections in the WHOLE capture, for the events strip.

        Deliberately not limited to the visible range: collections are rare (a
        26 000-frame capture can hold three), so a window-limited query shows
        nothing almost all of the time. The table has one row per GC, so reading
        all of it costs nothing, and the strip can point at the ones off-screen.

        These come from the server (SRV_GC), which knows exactly when its
        pseudo-GC ran and what it swept - nothing here is inferred from free
        counts. That matters because a native capture frees explicitly on nearly
        every frame, so no per-frame heuristic can tell a collection from
        ordinary native churn. Captures recorded before SRV_GC existed, and
        native-only captures, simply have none.
    */
    const std::vector<owlcat::gc_event_t>& get_gc_events() const { return m_gc_events; }

    bool get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id);
    //bool get_allocations_type_and_stack(const std::vector<uint64_t>& addresses, std::vector<owlcat::search_result_t>& results);
};
