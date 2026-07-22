---
name: analyze-owl-capture
description: Analyze a saved .owl memory capture (find leaks, growth, top allocators) using the owl_query serve-mode HTTP API. Use when asked to analyze a capture, investigate memory growth/leaks in a profiling session, or answer questions about what a game allocated.
---

# Analyzing an .owl capture

`owl_query` answers aggregate questions about a capture as JSON. Full reference:
[doc/owl_query.md](../../../doc/owl_query.md). Binary:
`build\owl_query\Release\owl_query.exe` (build target `owl_query` if missing).

## Rules

1. **Always use serve mode** — opening a capture unpacks gigabytes to %TEMP%;
   one-shot mode pays that on every query.
   - Start in background: `owl_query <capture.owl> serve --port 8890`
   - Wait for the ready line `{ "serving": ... }` on stdout (a 250 GB capture
     takes ~2 min to open).
   - Query with any HTTP client: `curl "http://127.0.0.1:8890/top-types?from=A&to=B"`
   - **Always finish with** `curl http://127.0.0.1:8890/shutdown` — it releases
     the multi-GB temp extraction.
2. **Replays cost time; ranges are your budget.** Any live-objects query
   (`/top-types`, `/growth`, `/callstacks`, `/objects`) replays its frame
   range at roughly 6–8 M events/s (measured: ~80 M events ≈ 12 s; 6.5 B
   events ≈ 13 min). `/summary` and `/frames` are SQL-backed and instant.
   Narrow the range before reaching for replay-backed queries.
3. **Reuse ranges.** The server caches the last TWO replays by exact
   (from,to). Drill down with identical ranges: `top-types` → `callstacks` →
   `objects` over the same from/to costs one replay. `growth` reuses a cached
   window too.
4. Requests are sequential: a long replay delays the next query — use a
   generous HTTP timeout instead of retrying (retries just queue up).

## Workflow: "where does memory go / what leaks?"

1. `/summary` — frame range, event counts, heap peaks. Sanity-check scale
   before anything else.
2. `/frames?buckets=200` — locate where `tracked_heap_bytes` (or
   `committed_bytes`) grows or spikes.
3. `/top-types?from=A&to=B` over the growth region — what accumulates.
   "Live" = allocated in range, not freed in range → leak *candidates*.
4. `/growth?base_from=..&base_to=..&from=..&to=..` — compare two *comparable*
   windows (same level, same activity); types with positive `delta_bytes`
   that persist across windows are the real suspects.
5. `/callstacks?type=<suspect>&from=A&to=B` (same range as step 3 — cache
   hit) — the allocation sites responsible.
6. `/objects?type=<suspect>` for concrete instances if needed.
7. `/shutdown`.

## Symbolicating native frames

Native frames show as `Module.dll+0xRVA` until a PDB search path is set.
When the analysis leads into native memory (`Unity Heap`, `HeapAlloc`,
`VirtualAlloc` types, or stacks dominated by raw `UnityPlayer.dll+0x...`
frames), symbolicate before drawing conclusions:

1. **Ask the user where the build's PDBs are** — do NOT guess or scan the
   disk. Typically it's the game build directory (containing the exe,
   `UnityPlayer*.pdb`, and for IL2CPP `GameAssembly.pdb`). Ask for the build
   that *matches the capture*: mismatched PDBs resolve to wrong names.
2. `curl "http://127.0.0.1:8890/symbols?paths=<url-encoded dirs>"`
   (';'-separated). Poll `/symbols/status` every few seconds until
   `pending` is 0 (about a minute for a large capture — big PDBs load once).
3. Re-run `/callstacks` — same range hits the replay cache, and the text is
   now resolved. If a response carries `symbolication_pending`, it raced the
   resolver: wait and re-request.
4. Check `unresolved_modules` in `/symbols/status`. Windows system DLLs
   (`ntdll.dll`, `KERNEL32.DLL`, video drivers…) being listed is normal —
   ignore them. If a module that matters is listed — the game exe,
   `UnityPlayer.dll`, `GameAssembly.dll`, `mono-2.0-bdwgc.dll`, or a game
   plugin — **tell the user which modules stayed unresolved and ask where
   their PDBs are** rather than analyzing raw addresses.

## Reading the output

- Type names mix managed (`System.String`) and native hook labels
  (`Unity Heap`, `HeapAlloc`, `VirtualAlloc (commit)`). `VirtualAlloc` sizes
  are reserve/commit ranges — expect few, huge "objects".
- Native stack frames appear as `Module.dll+0xRVA` until symbolicated (see
  above).
- `type_id`/`callstack_id` are stable within one capture only.
- Numbers match the UI exactly (same client library, same queries).
