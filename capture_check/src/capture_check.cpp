/*
	A diagnostic tool that checks the integrity of a saved capture (.owl file):
	- reconciles the size graph's running total with a full live-objects replay,
	- audits the raw event stream for anomalies (duplicate allocations at a live
	  address, unmatched frees, frees whose size differs from the allocation).

	The graph total and the replay total should agree to within the anomaly noise
	(the pseudo-GC has known races around object reallocation); a large difference
	means events were lost or double-counted somewhere in the pipeline.
*/
#include "mono_profiler_client.h"
#include "event_log.h"

#include <cstdio>
#include <cinttypes>
#include <unordered_map>
#include <vector>

using namespace owlcat;

static double mb(uint64_t bytes) { return bytes / (1024.0 * 1024.0); }

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		printf("Usage: capture_check <capture.owl>\n");
		return 1;
	}

	const char* path = argv[1];

	mono_profiler_client client;
	if (!client.open_data(path))
	{
		printf("Failed to open %s\n", path);
		return 1;
	}

	auto* data = client.get_data();

	uint64_t min_frame = 0, max_frame = 0;
	data->get_frame_boundaries(min_frame, max_frame);
	printf("Frames: %" PRIu64 " .. %" PRIu64 "\n", min_frame, max_frame);

	// 1. What the size graph shows at the end (the FrameStats running total)
	std::vector<uint64_t> allocs, frees, sizes;
	uint64_t max_allocs = 0, max_frees = 0;
	int64_t max_size = 0;
	data->get_frame_stats(allocs, frees, max_allocs, max_frees, sizes, max_size, min_frame, max_frame);
	uint64_t graph_total = sizes.empty() ? 0 : sizes.back();
	printf("Graph running total at last frame: %" PRIu64 " bytes (%.1f Mb), peak %.1f Mb\n",
		graph_total, mb(graph_total), max_size / (1024.0 * 1024.0));

	// 2. What the live objects table shows for the full range
	std::vector<live_object> objects;
	data->get_live_objects(objects, (int)min_frame, (int)max_frame, nullptr);
	uint64_t live_total = 0;
	for (auto& o : objects)
		live_total += o.size;
	printf("Live objects over full range: %zu objects, %" PRIu64 " bytes (%.1f Mb)\n",
		objects.size(), live_total, mb(live_total));

	// 3. Raw event stream audit
	auto reader = event_log_reader::open(client.get_event_log_path());
	if (reader == nullptr)
	{
		printf("Failed to open the event log\n");
		return 1;
	}

	uint64_t sum_alloc_bytes = 0, sum_free_bytes = 0;
	uint64_t alloc_events = 0, free_events = 0;
	uint64_t dup_allocs = 0, dup_alloc_bytes_lost = 0;
	uint64_t unmatched_frees = 0, unmatched_free_bytes = 0;
	uint64_t mismatched_free_size = 0;

	std::unordered_map<uint64_t, uint32_t> live; // addr -> size
	live.reserve(4 * 1024 * 1024);

	reader->read_range(reader->begin_offset(), reader->end_offset(), [&](const event_view& e)
	{
		if (e.is_alloc)
		{
			++alloc_events;
			sum_alloc_bytes += e.size;

			auto iter = live.find(e.addr);
			if (iter != live.end())
			{
				// A second allocation at a live address: the server should have sent
				// a free for the old object first
				++dup_allocs;
				dup_alloc_bytes_lost += e.size;
			}
			else
			{
				live.emplace(e.addr, e.size);
			}
		}
		else
		{
			++free_events;
			sum_free_bytes += e.size;

			auto iter = live.find(e.addr);
			if (iter == live.end())
			{
				++unmatched_frees;
				unmatched_free_bytes += e.size;
			}
			else
			{
				if (iter->second != e.size)
					++mismatched_free_size;
				live.erase(iter);
			}
		}
		return true;
	});

	uint64_t audit_live_total = 0;
	for (auto& pair : live)
		audit_live_total += pair.second;

	int64_t net = (int64_t)sum_alloc_bytes - (int64_t)sum_free_bytes;

	printf("\n--- Raw event stream audit ---\n");
	printf("Alloc events: %" PRIu64 " (%.1f Mb total)\n", alloc_events, mb(sum_alloc_bytes));
	printf("Free events:  %" PRIu64 " (%.1f Mb total)\n", free_events, mb(sum_free_bytes));
	printf("Net (allocs - frees): %.1f Mb  <- the graph value at the end of capture\n", net / (1024.0 * 1024.0));
	printf("Live at end (replay): %zu objects, %.1f Mb  <- the table value for the full range\n", live.size(), mb(audit_live_total));
	printf("\nAnomalies:\n");
	printf("Duplicate allocs at a live address: %" PRIu64 " (%.1f Mb)\n", dup_allocs, mb(dup_alloc_bytes_lost));
	printf("Unmatched frees: %" PRIu64 " (%.1f Mb)\n", unmatched_frees, mb(unmatched_free_bytes));
	printf("Frees with size != alloc size: %" PRIu64 "\n", mismatched_free_size);

	// A large difference between the graph and the replay that is not explained by
	// the anomalies means events were lost or double-counted somewhere
	const int64_t discrepancy = net - (int64_t)audit_live_total;
	const int64_t explained = (int64_t)dup_alloc_bytes_lost + (int64_t)unmatched_free_bytes;
	if (discrepancy > explained + 1024 * 1024 || discrepancy < -(explained + 1024 * 1024))
	{
		printf("\nWARNING: graph and replay differ by %.1f Mb beyond the explained anomalies!\n", (discrepancy - explained) / (1024.0 * 1024.0));
		return 2;
	}

	printf("\nOK: graph and replay are consistent\n");
	return 0;
}
