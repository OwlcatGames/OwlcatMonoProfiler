# owl_query — programmatic access to captures

`owl_query` is a command-line tool that answers questions about a saved `.owl`
capture as JSON. It exists so that *programs* — AI agents, scripts, CI jobs —
can analyze profiling data without the UI. It links the same client library the
UI uses (`mono_profiler_client`), so its numbers always match what the UI shows.

Captures can contain millions of events, so every command **aggregates**; the
raw event stream is never dumped.

## Serve mode — use this for anything non-trivial

**Opening a capture is expensive**: the container is unpacked into a temp
directory (the event log can be many gigabytes), and live-objects queries
replay event-log ranges. One-shot invocations pay the unpack cost *every
time*. Serve mode opens the capture **once** and answers queries over local
HTTP until shut down:

```
owl_query <capture.owl> serve [--port N]     # default port 8890, binds 127.0.0.1 only
```

It prints a ready line (`{ "serving": ..., "port": ..., "frames": ... }`) once
the capture is open — launch it in the background and wait for that line.
Then query with any HTTP client; commands are paths, options are query
parameters (underscored: `base_from`, not `base-from`):

```
curl "http://127.0.0.1:8890/summary"
curl "http://127.0.0.1:8890/top-types?from=1000&to=2000&top=10"
curl "http://127.0.0.1:8890/growth?base_from=100&base_to=200&from=900&to=1000"
curl "http://127.0.0.1:8890/shutdown"        # stop the server when done
```

Bad queries return HTTP 400 with `{ "error": ... }`. Requests are handled
sequentially — a long replay delays the next query, it is not lost.

The server also caches the **last two live-objects replays** (keyed by frame
range), so the standard drill-down — `top-types`, then `callstacks`, then
`objects` over the *same* `from`/`to` — replays the event log once, not three
times, and `growth` can reuse a window you already queried. Reuse identical
ranges when drilling down to benefit.

## One-shot mode

For small captures and quick scripts the same commands work as a single
invocation (JSON on stdout, errors on stderr, non-zero exit):

```
owl_query <capture.owl> <command> [options]
```

All commands accept `--from N --to M` (HTTP: `?from=N&to=M`) to restrict the
frame range (default: the whole capture). "Live objects" over a range means:
allocated within the range and **not** freed within it — the same definition
the UI's object table uses. Note that these are leak *candidates*: an object
allocated near the end of the range may simply not have been collected yet.

## Commands

### `summary`
Capture overview. Start here.

```json
{
  "command": "summary",
  "frames": { "min": 0, "max": 35210 },
  "events": { "allocs": 48123456, "frees": 47000123 },
  "tracked_heap": { "final_bytes": 812345678, "peak_bytes": 901234567 },
  "process_memory": { "available": true, "final_committed_bytes": ..., "peak_committed_bytes": ..., "final_working_set_bytes": ..., "final_gc_heap_bytes": ... },
  "types": 18234,
  "callstacks": 412345
}
```

`tracked_heap` is the running total of everything the profiler recorded
(managed allocations, plus native hooks if native capture was on).
`process_memory.available` is `false` for captures made before the per-frame
memory series was added.

### `frames [--buckets N]`
The size/allocation graph as data: the frame range split into at most N buckets
(default 100), each with summed `allocs`/`frees` and the `tracked_heap_bytes`
running total at the bucket's last frame (plus committed/working-set/GC-heap
bytes when available). Use it to locate spikes and growth regions, then zoom in
with `--from/--to`.

### `top-types [--top N] [--sort bytes|count] [--type <substr>]`
Live objects over the range grouped by type, sorted by bytes (default) or
count. `--type` filters by case-insensitive substring of the type name. The
`other` entry aggregates everything past the top N, so totals always add up.

### `growth --base-from A --base-to B [--top N]`
The leak-hunting command: compares the live objects of two frame ranges
(baseline `A..B` vs. the main `--from/--to` range) per type, sorted by
`delta_bytes` descending. Comparable windows (e.g. two runs of the same level)
make the deltas meaningful.

### `callstacks [--top N] [--type <substr>]`
Top allocation callstacks by live bytes over the range, each with its object
count, byte total, and the stack as an array of lines. Managed frames are
resolved (`Class.Method`); native frames appear as `Module.dll+0xRVA` —
owl_query does not run PDB symbolication (the UI does).

### `objects [--top N] [--type <substr>]`
The largest individual live objects: address, size, allocation frame, type and
callstack id. Useful to drill from a suspicious type down to concrete
instances.

## Native-frame symbolication

Native frames arrive as `Module.dll+0xRVA`. Given a PDB search path
(';'-separated directories — typically the game build directory holding the
exe, `UnityPlayer*.pdb`, and for IL2CPP `GameAssembly.pdb`), they resolve to
`Module.dll!Function+0xNN (file:line)`:

- One-shot: `--symbols "D:\game_build;D:\extra_pdbs"` — the command waits for
  resolution to finish, and warns on stderr about modules with no symbols.
- Serve mode: pass `--symbols` at startup, or set/change at runtime with
  `GET /symbols?paths=<url-encoded dirs>`. Resolution is asynchronous: poll
  `GET /symbols/status` → `{ "pending": N, "unresolved_modules": [...] }`
  until `pending` is 0 before querying `/callstacks` (a full re-resolve of a
  ~400k-callstack capture takes about a minute; the big cost is loading large
  PDBs once). `/callstacks` responses also carry a `symbolication_pending`
  field whenever output may still be raw.
- `unresolved_modules` lists modules whose symbols were not found under the
  current path (meaningful once `pending` is 0). Windows system DLLs
  (`ntdll`, `KERNEL32`, drivers…) are expected here and harmless; entries
  that matter are the game exe, `UnityPlayer.dll`, `GameAssembly.dll`,
  `mono-2.0-bdwgc.dll`, and game plugins.
- Symbolication also upgrades IL2CPP captures made with native capture on,
  where managed frames were degraded to `GameAssembly.dll+offset`.

## Typical agent workflow

1. Start `serve` in the background; wait for the ready line.
2. `/summary` — get the frame range and overall shape.
3. `/frames?buckets=200` — find where the heap grows.
4. `/top-types?from=<growth start>&to=<growth end>` — what accumulates there.
5. `/growth?base_from=<early>&base_to=...&from=<late>&to=...` — confirm which types actually grow between comparable windows.
6. `/callstacks?type=<suspect>&from=...&to=...` — find the allocation sites responsible (same range as step 4 hits the replay cache).
7. `/shutdown` when done.

## Caveats

- Replaying live objects for a large frame range reads that range of the event
  log; on multi-gigabyte captures a full-range query can take a while even in
  serve mode. Narrow the range when possible, and reuse ranges to hit the cache.
- Callstack ids and type ids are stable within one capture, but not across
  captures.
- The replay cache holds two ranges' live objects in memory (tens of bytes per
  object); on captures with tens of millions of simultaneously-live objects
  that is a gigabyte or two — fine on a dev machine, worth knowing about.
  The replay itself transiently needs several more GB for its address map on
  billion-event ranges.
