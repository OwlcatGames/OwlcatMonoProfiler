/*
	owl_query: a query tool over saved .owl captures, designed for programmatic
	consumers (AI agents, scripts, CI). All results are JSON; errors go to
	stderr (one-shot mode) or come back as HTTP error responses (serve mode).

	Opening a capture is expensive: the container is unpacked into a temporary
	directory (the event log can be many gigabytes), and every live-objects
	query replays a range of the event log. So the intended usage for anything
	non-trivial is SERVE MODE: `owl_query <capture.owl> serve` opens the
	capture ONCE and answers queries over local HTTP until shut down. One-shot
	mode (a command instead of `serve`) remains for small captures and scripts.

	Serve mode also keeps the last live-objects replay cached in memory, so a
	sequence of queries over the same frame range (top-types, then callstacks,
	then objects - the typical drill-down) replays the event log once.

	The tool uses only the public mono_profiler_client_data interface - the same
	queries the UI runs - so results always match what the UI would show. Since
	raw captures can hold millions of events, every command aggregates: the raw
	event stream is never dumped.

	See doc/owl_query.md for the command reference and typical workflows.
*/
#include "mono_profiler_client.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "ws2_32.lib")
#endif

using namespace owlcat;

static void fail(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(1);
}

// printf-style append to a string (all JSON is built through this)
static void appendf(std::string& out, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char stack_buf[1024];
	int len = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
	va_end(args);
	if (len < 0)
		return;
	if ((size_t)len < sizeof(stack_buf))
	{
		out.append(stack_buf, len);
		return;
	}
	std::vector<char> heap_buf(len + 1);
	va_start(args, fmt);
	vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args);
	va_end(args);
	out.append(heap_buf.data(), len);
}

// JSON string escaping (quotes, backslashes, control characters)
static std::string jesc(const char* s)
{
	std::string r;
	if (s == nullptr)
		return r;
	for (const char* p = s; *p; ++p)
	{
		unsigned char c = (unsigned char)*p;
		switch (c)
		{
		case '"': r += "\\\""; break;
		case '\\': r += "\\\\"; break;
		case '\n': r += "\\n"; break;
		case '\r': r += "\\r"; break;
		case '\t': r += "\\t"; break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				r += buf;
			}
			else
				r += (char)c;
		}
	}
	return r;
}

static std::string to_lower(const std::string& s)
{
	std::string r = s;
	std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)tolower(c); });
	return r;
}

static bool contains_ci(const char* haystack, const std::string& needle_lower)
{
	if (needle_lower.empty())
		return true;
	if (haystack == nullptr)
		return false;
	return to_lower(haystack).find(needle_lower) != std::string::npos;
}

// ---------------- Query context and parameters ----------------

// A user error in a query (bad range, missing argument). One-shot mode prints
// it to stderr and exits; serve mode turns it into an HTTP 400 response.
struct query_error : std::runtime_error
{
	explicit query_error(const std::string& what) : std::runtime_error(what) {}
};

/*
	Everything a query needs: the open capture, its frame boundaries, and the
	replay cache. The cache holds the (unfiltered) live-objects replays of the
	last TWO requested frame ranges: replaying is the expensive part of a
	query, the typical drill-down runs several commands over the same range,
	and growth compares two ranges - two slots make its fetch order irrelevant
	(with one slot, fetching the second window evicted the first, which could
	silently discard a very expensive replay).
*/
struct query_context
{
	mono_profiler_client* client = nullptr;
	mono_profiler_client_data* data = nullptr;
	uint64_t min_frame = 0;
	uint64_t max_frame = 0;

	struct cache_slot
	{
		bool valid = false;
		uint64_t from = 0, to = 0;
		uint64_t stamp = 0;
		std::vector<live_object> objects;
	};
	cache_slot slots[2];
	uint64_t next_stamp = 0;

	// Returns live objects for the range, replaying the event log only on a
	// cache miss. The reference stays valid across ONE subsequent get_live
	// with a different range (the other slot is evicted), which is what
	// growth relies on to hold both of its windows at once.
	const std::vector<live_object>& get_live(uint64_t from, uint64_t to)
	{
		for (auto& slot : slots)
		{
			if (slot.valid && slot.from == from && slot.to == to)
			{
				slot.stamp = ++next_stamp;
				return slot.objects;
			}
		}

		cache_slot& victim =
			!slots[0].valid ? slots[0] :
			!slots[1].valid ? slots[1] :
			slots[0].stamp < slots[1].stamp ? slots[0] : slots[1];

		// Release the evicted replay's memory before building the new one
		std::vector<live_object>().swap(victim.objects);
		data->get_live_objects(victim.objects, (int)from, (int)to, nullptr);
		victim.from = from;
		victim.to = to;
		victim.valid = true;
		victim.stamp = ++next_stamp;
		return victim.objects;
	}
};

// Query arguments as key -> value, the shared currency of both frontends:
// CLI flags (--base-from 10) and HTTP query strings (base_from=10) both parse
// into this map with underscore keys.
using arg_map = std::map<std::string, std::string>;

static uint64_t arg_u64(const arg_map& args, const char* name, uint64_t def, bool* was_set = nullptr)
{
	auto iter = args.find(name);
	if (was_set != nullptr)
		*was_set = (iter != args.end());
	if (iter == args.end())
		return def;
	char* end = nullptr;
	uint64_t value = strtoull(iter->second.c_str(), &end, 10);
	if (end == iter->second.c_str() || *end != 0)
		throw query_error(std::string("Argument '") + name + "' is not a number: " + iter->second);
	return value;
}

static std::string arg_str(const arg_map& args, const char* name)
{
	auto iter = args.find(name);
	return iter != args.end() ? iter->second : std::string();
}

struct query_params
{
	uint64_t from = 0, to = 0;
	bool has_base = false;
	uint64_t base_from = 0, base_to = 0;
	uint64_t top = 0;
	std::string type_filter;        // original text, for echoing back
	std::string type_filter_lower;  // for matching
	bool sort_by_count = false;
	uint64_t buckets = 0;
};

static query_params parse_params(const query_context& ctx, const arg_map& args, uint64_t default_top)
{
	query_params p;
	p.from = arg_u64(args, "from", ctx.min_frame);
	p.to = arg_u64(args, "to", ctx.max_frame);
	if (p.from < ctx.min_frame) p.from = ctx.min_frame;
	if (p.to > ctx.max_frame) p.to = ctx.max_frame;
	if (p.from > p.to)
		throw query_error("Invalid frame range " + std::to_string(p.from) + ".." + std::to_string(p.to) +
			" (capture has " + std::to_string(ctx.min_frame) + ".." + std::to_string(ctx.max_frame) + ")");

	bool base_from_set = false, base_to_set = false;
	p.base_from = arg_u64(args, "base_from", 0, &base_from_set);
	p.base_to = arg_u64(args, "base_to", 0, &base_to_set);
	p.has_base = base_from_set && base_to_set;
	if (base_from_set != base_to_set)
		throw query_error("base_from and base_to must be given together");
	if (p.has_base && p.base_from > p.base_to)
		throw query_error("Invalid base frame range");

	p.top = arg_u64(args, "top", default_top);
	p.type_filter = arg_str(args, "type");
	p.type_filter_lower = to_lower(p.type_filter);
	p.sort_by_count = arg_str(args, "sort") == "count";
	p.buckets = arg_u64(args, "buckets", 100);
	if (p.buckets == 0)
		p.buckets = 1;
	return p;
}

// ---------------- Shared aggregation helpers ----------------

struct type_agg
{
	uint64_t objects = 0;
	uint64_t bytes = 0;
};

static std::unordered_map<uint64_t, type_agg> aggregate_by_type(const std::vector<live_object>& objs)
{
	std::unordered_map<uint64_t, type_agg> result;
	result.reserve(1024);
	for (const auto& o : objs)
	{
		auto& agg = result[o.type_id];
		agg.objects += 1;
		agg.bytes += o.size;
	}
	return result;
}

// Copies only the objects whose type name matches the filter. The name check
// is done once per unique type id, not per object.
static std::vector<live_object> filter_by_type(query_context& ctx, const std::vector<live_object>& objs, const std::string& filter_lower)
{
	std::unordered_map<uint64_t, bool> type_matches;
	auto matches = [&](uint64_t type_id)
	{
		auto iter = type_matches.find(type_id);
		if (iter == type_matches.end())
			iter = type_matches.emplace(type_id, contains_ci(ctx.data->get_type_name(type_id), filter_lower)).first;
		return iter->second;
	};

	std::vector<live_object> result;
	for (const auto& o : objs)
		if (matches(o.type_id))
			result.push_back(o);
	return result;
}

static const char* type_name_or_unknown(query_context& ctx, uint64_t type_id)
{
	const char* name = ctx.data->get_type_name(type_id);
	return name != nullptr ? name : "<unknown>";
}

// ---------------- Commands ----------------

static std::string cmd_summary(query_context& ctx)
{
	std::vector<uint64_t> allocs, frees, sizes;
	uint64_t max_allocs = 0, max_frees = 0;
	int64_t max_size = 0;
	ctx.data->get_frame_stats(allocs, frees, max_allocs, max_frees, sizes, max_size, ctx.min_frame, ctx.max_frame);

	uint64_t total_allocs = 0, total_frees = 0;
	for (auto v : allocs) total_allocs += v;
	for (auto v : frees) total_frees += v;

	std::vector<uint64_t> committed, working_set, gc_heap;
	uint64_t max_committed = 0;
	ctx.data->get_memory_series(committed, working_set, gc_heap, max_committed, ctx.min_frame, ctx.max_frame);

	std::string out;
	out += "{\n";
	out += "  \"command\": \"summary\",\n";
	appendf(out, "  \"frames\": { \"min\": %" PRIu64 ", \"max\": %" PRIu64 " },\n", ctx.min_frame, ctx.max_frame);
	appendf(out, "  \"events\": { \"allocs\": %" PRIu64 ", \"frees\": %" PRIu64 " },\n", total_allocs, total_frees);
	// "Tracked heap" = running total of everything the profiler recorded
	// (managed allocations plus native hooks, if native capture was on)
	appendf(out, "  \"tracked_heap\": { \"final_bytes\": %" PRIu64 ", \"peak_bytes\": %" PRId64 " },\n",
		sizes.empty() ? 0 : sizes.back(), max_size);
	if (!committed.empty())
	{
		appendf(out, "  \"process_memory\": { \"available\": true, \"final_committed_bytes\": %" PRIu64 ", \"peak_committed_bytes\": %" PRIu64 ", \"final_working_set_bytes\": %" PRIu64 ", \"final_gc_heap_bytes\": %" PRIu64 " },\n",
			committed.back(), max_committed,
			working_set.empty() ? 0 : working_set.back(),
			gc_heap.empty() ? 0 : gc_heap.back());
	}
	else
	{
		out += "  \"process_memory\": { \"available\": false },\n";
	}
	appendf(out, "  \"types\": %zu,\n", ctx.data->get_types_count());
	appendf(out, "  \"callstacks\": %zu\n", ctx.data->get_callstacks_count());
	out += "}\n";
	return out;
}

static std::string cmd_top_types(query_context& ctx, const query_params& p)
{
	const auto& all = ctx.get_live(p.from, p.to);
	std::vector<live_object> filtered;
	const std::vector<live_object>* objects = &all;
	if (!p.type_filter_lower.empty())
	{
		filtered = filter_by_type(ctx, all, p.type_filter_lower);
		objects = &filtered;
	}

	auto by_type = aggregate_by_type(*objects);
	std::vector<std::pair<uint64_t, type_agg>> sorted(by_type.begin(), by_type.end());
	std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b)
	{
		return p.sort_by_count ? a.second.objects > b.second.objects : a.second.bytes > b.second.bytes;
	});

	uint64_t total_objects = 0, total_bytes = 0;
	for (const auto& pair : sorted)
	{
		total_objects += pair.second.objects;
		total_bytes += pair.second.bytes;
	}

	std::string out;
	out += "{\n";
	out += "  \"command\": \"top-types\",\n";
	appendf(out, "  \"from\": %" PRIu64 ", \"to\": %" PRIu64 ",\n", p.from, p.to);
	if (!p.type_filter.empty())
		appendf(out, "  \"filter\": \"%s\",\n", jesc(p.type_filter.c_str()).c_str());
	appendf(out, "  \"total\": { \"types\": %zu, \"objects\": %" PRIu64 ", \"bytes\": %" PRIu64 " },\n", sorted.size(), total_objects, total_bytes);
	out += "  \"types\": [\n";
	size_t shown = std::min(sorted.size(), (size_t)p.top);
	for (size_t i = 0; i < shown; ++i)
	{
		appendf(out, "    { \"type_id\": %" PRIu64 ", \"name\": \"%s\", \"objects\": %" PRIu64 ", \"bytes\": %" PRIu64 " }%s\n",
			sorted[i].first, jesc(type_name_or_unknown(ctx, sorted[i].first)).c_str(),
			sorted[i].second.objects, sorted[i].second.bytes,
			i + 1 < shown ? "," : "");
	}
	out += "  ],\n";

	uint64_t other_objects = 0, other_bytes = 0;
	for (size_t i = shown; i < sorted.size(); ++i)
	{
		other_objects += sorted[i].second.objects;
		other_bytes += sorted[i].second.bytes;
	}
	appendf(out, "  \"other\": { \"types\": %zu, \"objects\": %" PRIu64 ", \"bytes\": %" PRIu64 " }\n", sorted.size() - shown, other_objects, other_bytes);
	out += "}\n";
	return out;
}

static std::string cmd_growth(query_context& ctx, const query_params& p)
{
	if (!p.has_base)
		throw query_error("growth requires base_from and base_to (the baseline frame range to compare against)");

	// The two-slot cache holds both windows at once, so both references stay valid
	const auto& base_objects = ctx.get_live(p.base_from, p.base_to);
	auto base_agg = aggregate_by_type(base_objects);
	uint64_t base_object_count = base_objects.size();
	uint64_t base_total = 0;
	for (const auto& o : base_objects)
		base_total += o.size;

	const auto& range_objects = ctx.get_live(p.from, p.to);
	auto range_agg = aggregate_by_type(range_objects);
	uint64_t range_total = 0;
	for (const auto& o : range_objects)
		range_total += o.size;

	// Union of type ids from both windows, with per-type deltas
	struct growth_entry
	{
		uint64_t type_id;
		type_agg base, range;
		int64_t delta_bytes() const { return (int64_t)range.bytes - (int64_t)base.bytes; }
	};
	std::unordered_map<uint64_t, growth_entry> merged;
	for (const auto& pair : base_agg)
		merged[pair.first] = growth_entry{ pair.first, pair.second, {} };
	for (const auto& pair : range_agg)
	{
		auto& e = merged[pair.first];
		e.type_id = pair.first;
		e.range = pair.second;
	}

	std::vector<growth_entry> sorted;
	sorted.reserve(merged.size());
	for (const auto& pair : merged)
		if (pair.second.delta_bytes() != 0 || pair.second.base.objects != pair.second.range.objects)
			sorted.push_back(pair.second);
	std::sort(sorted.begin(), sorted.end(), [](const growth_entry& a, const growth_entry& b) { return a.delta_bytes() > b.delta_bytes(); });

	std::string out;
	out += "{\n";
	out += "  \"command\": \"growth\",\n";
	appendf(out, "  \"base\": { \"from\": %" PRIu64 ", \"to\": %" PRIu64 ", \"objects\": %" PRIu64 ", \"bytes\": %" PRIu64 " },\n", p.base_from, p.base_to, base_object_count, base_total);
	appendf(out, "  \"range\": { \"from\": %" PRIu64 ", \"to\": %" PRIu64 ", \"objects\": %zu, \"bytes\": %" PRIu64 " },\n", p.from, p.to, range_objects.size(), range_total);
	appendf(out, "  \"delta_bytes\": %" PRId64 ",\n", (int64_t)range_total - (int64_t)base_total);
	out += "  \"types\": [\n";
	size_t shown = std::min(sorted.size(), (size_t)p.top);
	for (size_t i = 0; i < shown; ++i)
	{
		const auto& e = sorted[i];
		appendf(out, "    { \"type_id\": %" PRIu64 ", \"name\": \"%s\", \"base_objects\": %" PRIu64 ", \"base_bytes\": %" PRIu64 ", \"range_objects\": %" PRIu64 ", \"range_bytes\": %" PRIu64 ", \"delta_objects\": %" PRId64 ", \"delta_bytes\": %" PRId64 " }%s\n",
			e.type_id, jesc(type_name_or_unknown(ctx, e.type_id)).c_str(),
			e.base.objects, e.base.bytes, e.range.objects, e.range.bytes,
			(int64_t)e.range.objects - (int64_t)e.base.objects, e.delta_bytes(),
			i + 1 < shown ? "," : "");
	}
	out += "  ]\n";
	out += "}\n";
	return out;
}

static std::string cmd_callstacks(query_context& ctx, const query_params& p)
{
	const auto& all = ctx.get_live(p.from, p.to);
	std::vector<live_object> filtered;
	const std::vector<live_object>* objects = &all;
	if (!p.type_filter_lower.empty())
	{
		filtered = filter_by_type(ctx, all, p.type_filter_lower);
		objects = &filtered;
	}

	std::unordered_map<uint64_t, type_agg> by_stack;
	by_stack.reserve(1024);
	for (const auto& o : *objects)
	{
		auto& agg = by_stack[o.callstack_id];
		agg.objects += 1;
		agg.bytes += o.size;
	}

	std::vector<std::pair<uint64_t, type_agg>> sorted(by_stack.begin(), by_stack.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });

	std::string out;
	out += "{\n";
	out += "  \"command\": \"callstacks\",\n";
	// Warn consumers that stack text may still be raw (Module.dll+0xRVA): the
	// background symbolication hasn't finished for the current symbol path
	size_t sym_pending = ctx.client != nullptr ? ctx.client->get_symbolication_pending() : 0;
	if (sym_pending > 0)
		appendf(out, "  \"symbolication_pending\": %zu,\n", sym_pending);
	appendf(out, "  \"from\": %" PRIu64 ", \"to\": %" PRIu64 ",\n", p.from, p.to);
	if (!p.type_filter.empty())
		appendf(out, "  \"filter\": \"%s\",\n", jesc(p.type_filter.c_str()).c_str());
	appendf(out, "  \"total\": { \"callstacks\": %zu, \"objects\": %zu },\n", sorted.size(), objects->size());
	out += "  \"callstacks\": [\n";
	size_t shown = std::min(sorted.size(), (size_t)p.top);
	for (size_t i = 0; i < shown; ++i)
	{
		appendf(out, "    { \"callstack_id\": %" PRIu64 ", \"objects\": %" PRIu64 ", \"bytes\": %" PRIu64 ", \"stack\": [",
			sorted[i].first, sorted[i].second.objects, sorted[i].second.bytes);

		// Callstack text is one string with newline-separated lines
		std::string text = ctx.data->get_callstack(sorted[i].first);
		bool first_line = true;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos)
				eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			if (!line.empty())
			{
				appendf(out, "%s\n      \"%s\"", first_line ? "" : ",", jesc(line.c_str()).c_str());
				first_line = false;
			}
			pos = eol + 1;
		}
		appendf(out, "\n    ] }%s\n", i + 1 < shown ? "," : "");
	}
	out += "  ]\n";
	out += "}\n";
	return out;
}

static std::string cmd_objects(query_context& ctx, const query_params& p)
{
	// Sorting needs a mutable copy; take the filter path or copy the cache
	std::vector<live_object> objects;
	{
		const auto& all = ctx.get_live(p.from, p.to);
		objects = p.type_filter_lower.empty() ? all : filter_by_type(ctx, all, p.type_filter_lower);
	}

	std::sort(objects.begin(), objects.end(), [](const live_object& a, const live_object& b) { return a.size > b.size; });

	std::string out;
	out += "{\n";
	out += "  \"command\": \"objects\",\n";
	appendf(out, "  \"from\": %" PRIu64 ", \"to\": %" PRIu64 ",\n", p.from, p.to);
	if (!p.type_filter.empty())
		appendf(out, "  \"filter\": \"%s\",\n", jesc(p.type_filter.c_str()).c_str());
	appendf(out, "  \"matched\": %zu,\n", objects.size());
	out += "  \"objects\": [\n";
	size_t shown = std::min(objects.size(), (size_t)p.top);
	for (size_t i = 0; i < shown; ++i)
	{
		const auto& o = objects[i];
		appendf(out, "    { \"address\": \"0x%" PRIx64 "\", \"bytes\": %" PRIu64 ", \"alloc_frame\": %" PRIu64 ", \"type\": \"%s\", \"callstack_id\": %" PRIu64 " }%s\n",
			o.addr, o.size, o.frame, jesc(type_name_or_unknown(ctx, o.type_id)).c_str(), o.callstack_id,
			i + 1 < shown ? "," : "");
	}
	out += "  ]\n";
	out += "}\n";
	return out;
}

static std::string cmd_frames(query_context& ctx, const query_params& p)
{
	std::vector<uint64_t> allocs, frees, sizes;
	uint64_t max_allocs = 0, max_frees = 0;
	int64_t max_size = 0;
	ctx.data->get_frame_stats(allocs, frees, max_allocs, max_frees, sizes, max_size, p.from, p.to);

	std::vector<uint64_t> committed, working_set, gc_heap;
	uint64_t max_committed = 0;
	ctx.data->get_memory_series(committed, working_set, gc_heap, max_committed, p.from, p.to);

	uint64_t frame_count = p.to - p.from + 1;
	uint64_t buckets = std::min(p.buckets, frame_count);

	std::string out;
	out += "{\n";
	out += "  \"command\": \"frames\",\n";
	appendf(out, "  \"from\": %" PRIu64 ", \"to\": %" PRIu64 ", \"buckets\": %" PRIu64 ",\n", p.from, p.to, buckets);
	appendf(out, "  \"peak\": { \"allocs\": %" PRIu64 ", \"frees\": %" PRIu64 ", \"tracked_heap_bytes\": %" PRId64 " },\n", max_allocs, max_frees, max_size);
	out += "  \"series\": [\n";
	for (uint64_t b = 0; b < buckets; ++b)
	{
		uint64_t bucket_begin = p.from + b * frame_count / buckets;
		uint64_t bucket_end = p.from + (b + 1) * frame_count / buckets - 1;

		uint64_t bucket_allocs = 0, bucket_frees = 0;
		for (uint64_t f = bucket_begin; f <= bucket_end; ++f)
		{
			size_t idx = (size_t)(f - p.from);
			if (idx < allocs.size()) bucket_allocs += allocs[idx];
			if (idx < frees.size()) bucket_frees += frees[idx];
		}

		size_t end_idx = (size_t)(bucket_end - p.from);
		uint64_t heap_end = end_idx < sizes.size() ? sizes[end_idx] : 0;

		appendf(out, "    { \"from\": %" PRIu64 ", \"to\": %" PRIu64 ", \"allocs\": %" PRIu64 ", \"frees\": %" PRIu64 ", \"tracked_heap_bytes\": %" PRIu64,
			bucket_begin, bucket_end, bucket_allocs, bucket_frees, heap_end);
		if (end_idx < committed.size())
			appendf(out, ", \"committed_bytes\": %" PRIu64 ", \"working_set_bytes\": %" PRIu64 ", \"gc_heap_bytes\": %" PRIu64,
				committed[end_idx],
				end_idx < working_set.size() ? working_set[end_idx] : 0,
				end_idx < gc_heap.size() ? gc_heap[end_idx] : 0);
		appendf(out, " }%s\n", b + 1 < buckets ? "," : "");
	}
	out += "  ]\n";
	out += "}\n";
	return out;
}

// Dispatches a query by command name. Throws query_error for user mistakes.
static std::string run_query(query_context& ctx, const std::string& command, const arg_map& args)
{
	if (command == "summary")
		return cmd_summary(ctx);
	if (command == "top-types")
		return cmd_top_types(ctx, parse_params(ctx, args, 20));
	if (command == "growth")
		return cmd_growth(ctx, parse_params(ctx, args, 20));
	if (command == "callstacks")
		return cmd_callstacks(ctx, parse_params(ctx, args, 10));
	if (command == "objects")
		return cmd_objects(ctx, parse_params(ctx, args, 50));
	if (command == "frames")
		return cmd_frames(ctx, parse_params(ctx, args, 20));
	throw query_error("Unknown command: " + command);
}

// ---------------- Serve mode (local HTTP) ----------------

#if defined(_WIN32)

static bool send_all(SOCKET s, const char* data, size_t size)
{
	while (size > 0)
	{
		int sent = send(s, data, (int)std::min(size, (size_t)1 << 20), 0);
		if (sent <= 0)
			return false;
		data += sent;
		size -= sent;
	}
	return true;
}

static void send_response(SOCKET s, int status, const char* status_text, const std::string& body)
{
	std::string head;
	appendf(head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, status_text, body.size());
	if (send_all(s, head.data(), head.size()))
		send_all(s, body.data(), body.size());
}

static std::string url_decode(const std::string& s)
{
	std::string r;
	r.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '+')
			r += ' ';
		else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
		{
			char hex[3] = { s[i + 1], s[i + 2], 0 };
			r += (char)strtoul(hex, nullptr, 16);
			i += 2;
		}
		else
			r += s[i];
	}
	return r;
}

static arg_map parse_query_string(const std::string& query)
{
	arg_map args;
	size_t pos = 0;
	while (pos < query.size())
	{
		size_t amp = query.find('&', pos);
		if (amp == std::string::npos)
			amp = query.size();
		std::string pair = query.substr(pos, amp - pos);
		size_t eq = pair.find('=');
		if (eq != std::string::npos)
			args[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
		pos = amp + 1;
	}
	return args;
}

/*
	A deliberately minimal HTTP server: 127.0.0.1 only, GET only, one request
	per connection, requests handled sequentially (queries are CPU-bound and
	share one capture, so concurrency would only interleave replays). This is
	an analysis sidecar for a local agent, not a web service.
*/
static int serve(query_context& ctx, const std::string& capture_path, int port)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		fail("WSAStartup failed");

	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
		fail("Failed to create socket");

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0)
		fail("Failed to bind 127.0.0.1:%d (is another owl_query serving?)", port);
	if (listen(listener, 8) != 0)
		fail("listen failed");

	// The ready line: agents launch serve in the background and wait for this
	printf("{ \"serving\": \"%s\", \"port\": %d, \"frames\": { \"min\": %" PRIu64 ", \"max\": %" PRIu64 " }, \"endpoints\": [\"/summary\", \"/frames\", \"/top-types\", \"/growth\", \"/callstacks\", \"/objects\", \"/symbols\", \"/symbols/status\", \"/shutdown\"] }\n",
		jesc(capture_path.c_str()).c_str(), port, ctx.min_frame, ctx.max_frame);
	fflush(stdout);

	bool running = true;
	while (running)
	{
		SOCKET conn = accept(listener, nullptr, nullptr);
		if (conn == INVALID_SOCKET)
			continue;

		DWORD timeout_ms = 5000;
		setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

		std::string request;
		char buf[4096];
		while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
		{
			int received = recv(conn, buf, sizeof(buf), 0);
			if (received <= 0)
				break;
			request.append(buf, received);
		}

		// Only the request line matters: METHOD SP /path?query SP HTTP/1.1
		size_t line_end = request.find("\r\n");
		size_t sp1 = request.find(' ');
		size_t sp2 = sp1 != std::string::npos ? request.find(' ', sp1 + 1) : std::string::npos;
		if (line_end == std::string::npos || sp1 == std::string::npos || sp2 == std::string::npos || sp2 > line_end)
		{
			send_response(conn, 400, "Bad Request", "{ \"error\": \"Malformed request\" }\n");
			closesocket(conn);
			continue;
		}

		std::string method = request.substr(0, sp1);
		std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);
		if (method != "GET")
		{
			send_response(conn, 405, "Method Not Allowed", "{ \"error\": \"Only GET is supported\" }\n");
			closesocket(conn);
			continue;
		}

		size_t question = target.find('?');
		std::string path = target.substr(0, question);
		arg_map args = question != std::string::npos ? parse_query_string(target.substr(question + 1)) : arg_map();

		if (path == "/shutdown")
		{
			send_response(conn, 200, "OK", "{ \"ok\": true }\n");
			running = false;
		}
		else if (path == "/symbols")
		{
			// Sets the PDB search path (';'-separated directories, URL-encoded) and
			// starts re-resolving all callstacks in the background. Poll
			// /symbols/status until pending is 0 before querying /callstacks.
			ctx.client->set_symbol_paths(arg_str(args, "paths"));
			std::string body;
			appendf(body, "{ \"ok\": true, \"pending\": %zu }\n", ctx.client->get_symbolication_pending());
			send_response(conn, 200, "OK", body);
		}
		else if (path == "/symbols/status")
		{
			std::string body;
			appendf(body, "{ \"pending\": %zu, \"unresolved_modules\": [", ctx.client->get_symbolication_pending());
			auto unresolved = ctx.client->get_unresolved_symbol_modules();
			for (size_t i = 0; i < unresolved.size(); ++i)
				appendf(body, "%s\"%s\"", i > 0 ? ", " : "", jesc(unresolved[i].c_str()).c_str());
			body += "] }\n";
			send_response(conn, 200, "OK", body);
		}
		else if (path.size() > 1 && path[0] == '/')
		{
			try
			{
				std::string body = run_query(ctx, path.substr(1), args);
				send_response(conn, 200, "OK", body);
			}
			catch (const query_error& e)
			{
				send_response(conn, 400, "Bad Request", "{ \"error\": \"" + jesc(e.what()) + "\" }\n");
			}
		}
		else
		{
			send_response(conn, 404, "Not Found", "{ \"error\": \"Unknown endpoint\" }\n");
		}

		closesocket(conn);
	}

	closesocket(listener);
	WSACleanup();
	return 0;
}

#endif // _WIN32

// ---------------- Entry point ----------------

static void usage()
{
	fprintf(stderr,
		"owl_query - JSON queries over saved .owl captures (for agents, scripts, CI)\n"
		"\n"
		"Usage: owl_query <capture.owl> <command> [options]\n"
		"\n"
		"Opening a capture unpacks it and can take a while for large captures, so for\n"
		"repeated queries use serve mode, which opens the capture once:\n"
		"\n"
		"  owl_query <capture.owl> serve [--port N]     Serve queries over local HTTP\n"
		"    (default port 8890; then e.g. curl \"http://127.0.0.1:8890/top-types?from=0&to=100\";\n"
		"     query keys use underscores: base_from; stop with /shutdown)\n"
		"\n"
		"Commands (one-shot or as HTTP endpoints):\n"
		"  summary                       Capture overview: frame range, event counts, heap peaks\n"
		"  frames                        Bucketed per-frame series (allocs, frees, heap size)\n"
		"                                  --buckets N (default 100)\n"
		"  top-types                     Live objects grouped by type over a frame range\n"
		"                                  --top N (default 20), --sort bytes|count, --type <substr>\n"
		"  growth                        Compare live objects of two frame ranges by type\n"
		"                                  --base-from A --base-to B (required), --top N\n"
		"  callstacks                    Top allocation callstacks by live bytes\n"
		"                                  --top N (default 10), --type <substr>\n"
		"  objects                       Largest individual live objects\n"
		"                                  --top N (default 50), --type <substr>\n"
		"\n"
		"Common options:\n"
		"  --from N --to M               Frame range (default: the whole capture)\n"
		"  --symbols \"dir1;dir2\"         PDB search path for native frames. One-shot mode\n"
		"                                waits for resolution; serve mode exposes /symbols\n"
		"                                (set/change path) and /symbols/status (pending count\n"
		"                                + modules whose PDBs were not found)\n"
		"\n"
		"'Live objects' over a range = allocated within the range and not freed within it.\n");
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		usage();
		return 1;
	}

	const char* path = argv[1];
	const char* command = argv[2];

	// CLI flags (--base-from 10) parse into the same underscore-keyed map as
	// HTTP query strings (base_from=10)
	arg_map args;
	for (int i = 3; i < argc - 1; i += 2)
	{
		if (strncmp(argv[i], "--", 2) != 0)
			fail("Expected an option, got: %s", argv[i]);
		std::string key = argv[i] + 2;
		std::replace(key.begin(), key.end(), '-', '_');
		args[key] = argv[i + 1];
	}

	mono_profiler_client client;
	if (!client.open_data(path))
		fail("Failed to open %s", path);

	query_context ctx;
	ctx.client = &client;
	ctx.data = client.get_data();
	ctx.data->get_frame_boundaries(ctx.min_frame, ctx.max_frame);

	// PDB search path for native frames; in serve mode it can also be set (or
	// changed) later via /symbols
	std::string symbols = arg_str(args, "symbols");
	if (!symbols.empty())
		client.set_symbol_paths(symbols);

	if (strcmp(command, "serve") == 0)
	{
#if defined(_WIN32)
		int port = (int)arg_u64(args, "port", 8890);
		return serve(ctx, path, port);
#else
		fail("serve mode is only implemented on Windows");
#endif
	}

	// One-shot output must be deterministic: wait for background symbolication
	// to settle before running the command
	if (!symbols.empty())
	{
		while (client.get_symbolication_pending() > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		auto unresolved = client.get_unresolved_symbol_modules();
		for (const auto& module : unresolved)
			fprintf(stderr, "warning: no symbols found for %s\n", module.c_str());
	}

	try
	{
		std::string result = run_query(ctx, command, args);
		fwrite(result.data(), 1, result.size(), stdout);
		return 0;
	}
	catch (const query_error& e)
	{
		usage();
		fail("%s", e.what());
	}
	return 1;
}
