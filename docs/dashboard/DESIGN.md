# llama-server live monitoring - Overall design

Status: APPROVED BASELINE

Date: 2026-02-14

This is the overall/source-of-truth design for the live monitoring feature. It defines the shared model and producer used by BOTH consumers:

- the **TUI** (plain-text stdout printer) - detailed design + implementation in IMPLEMENTATION.md
- the **web dashboard** (SSE tab in the built-in UI) - detailed design + implementation in WEB.md

Any divergence from this design (or the detail docs) requires approval and a doc update first.

## Document control / change process

- These docs (DESIGN.md + IMPLEMENTATION.md + WEB.md) are the single source of truth for this feature.
- Every design or implementation change/compromise requires explicit approval from the owner before any code changes.
- The order is always: agree on the change -> update the docs -> then change code.
- Each approved change appends a row to the change log at the end of the affected doc.

## 1. Goals

- Provide a live, read-only view of every sequence (active in a slot, idle, or cached/evicted) plus the whole resource picture: model identity, throughput, latency, memory (GPU/CPU/RAM), and real GPU/CPU/disk utilization.
- Two surfaces with one shared data source:
  - a terminal: plain-text, pipe-safe, no terminal control (`--tui N`).
  - the built-in web UI: a Dashboard tab with full scrollable per-sequence text, live push (no polling), scroll-follow, and cached sequences in a flat list. On by default, no switch.
- Per-sequence metadata everywhere: created time, last modified, cache hit rate.
- Keep it non-invasive: no behavior change when the TUI is off, no engine impact.

## 2. Non-goals

- No interactive control (no keys in TUI; the web dashboard is read-only for now).
- No graphs, sparklines, or historical time-series plots.
- No remote monitoring: both surfaces are in-process and reflect the local server.
- No exposing model internals beyond what is already reachable or cheaply computable.
- No long polling for token-speed data (50 tok/s makes it dumb).
- No changes to inference throughput: the engine thread must not pay meaningful cost.

## 3. Constraints

- Runs inside the llama-server process (personal fork; keep changes clean and reviewable).
- TUI: stdout is reserved for it; all logs stay on stderr. Active only when `--tui N` (N > 0); `0`/omitted = off, stdout empty.
- Web dashboard: part of the built-in web UI. **On by default, no enable/disable switch.**
- Server-side: no new external dependencies (nvidia-smi, /proc read directly). Frontend may use modern libraries (virtualized lists, etc.).
- The feature is at its final form from the start; fields without a source render `-`/n/a.

## 4. Core principles (locked)

1. **Two consumers, one producer**: the stdout printer and the web SSE both read the same per-sequence event stream.
2. TUI = plain-text timely-output program (no ncurses, no ANSI, no width/height). Web = push-based (SSE), never polling.
3. Engine impact negligible: status publish throttled (<= 10 Hz); token deltas emitted per decode step (batched).
4. One final format each. Unretrievable fields render `-` and are filled as sources land.
5. Zero input handling in the TUI; web dashboard read-only.
6. Information density over decoration; duplicates are merged.

## 5. Sequence model (shared)

A **sequence** is the unit shown by both consumers.

- Identity: a run of one server slot's context. When a slot is reused for an unrelated task (prefix match below `slot_prompt_similarity`, default `0.1`), a NEW sequence is created and the old one becomes cached (if saved) or dies.
- Lifecycle:
  - `created` / `active` - growing (new tokens coming).
  - `idle` - in a slot, KV resident (split mode), not growing.
  - `evicted` - cleared from the slot, kept in the RAM prompt cache (static text).
  - `gone` - dropped entirely (cache entry pruned, or never saved).
- **Created time**: stamped when a run starts and its prefix match is below `slot_prompt_similarity` (or the slot was empty), i.e. "prefix matching failed" -> new context.
- **Last modified**: last token written (active) / last save or access (evicted).
- **Cache hit rate (per sequence)**: `n_prompt_cached / (n_prompt_processed + n_prompt_cached)`. It travels with the sequence: saved into the cache entry on eviction, restored on load, cleared when the sequence fully dies.

## 6. Shared event producer

- Per-sequence **append-only token log** (full text) with a monotonic `seq` (token position / event id).
- **Events**: text delta (batched per decode step), state change (created, phase, idle, evicted->cache, restored, died), metadata update.
- The engine thread emits events after each `update_slots()` decode step; status/metadata is also published at <= 10 Hz.
- **Snapshot coherence** (no token loss between snapshot and first delta): a consumer subscribes to the log first, then reads the snapshot, both under the same mutex; plus per-cell `seq` so a consumer can detect a gap and re-sync a cell.
- Bounded memory: per-cell ring with a cap; no allocation per event on the hot path.

## 7. Consumers overview

### 7.1 TUI (stdout printer) - see IMPLEMENTATION.md

Plain-text frames at 1 Hz, `--tui N` lines per frame, tagged `[SERVER]`/`[SEQ n]` lines, per-slot status + tail lines. Reads the same per-slot data (formatted as a frame).

### 7.2 Web dashboard - see WEB.md

Dashboard tab in the built-in UI, SSE, on by default (no switch). Full scrollable per-cell text with immediate token push, terminal-emulator scroll-follow, HxW grid of active cells, cached sequences in a flat list, per-cell created/modified/hit-rate. Virtualized (lazy) rendering; no "load more" button.

## 8. Data model (shared)

### 8.1 Global

| Field | Meaning |
|---|---|
| model desc + alias | what is loaded |
| ctx / ctx_seq / ctx_train | context configuration |
| kv mode | unified or split |
| kv type, flash attention | KV quantization + FA config |
| status | RUN / SLEEPING (model unloaded) |
| busy n/m | slots processing / total |
| queue | deferred tasks waiting for a slot |
| engine % | fraction of wall time the engine thread spends in update_slots |
| uptime | server uptime |
| prompt/gen tps | global throughput gauges (live window) |
| req/s | user requests per second (5 s window) |
| spec acc % | speculative decoding acceptance (when enabled) |
| cache hit % | prefix-cache hit ratio |
| pp ms/t, tg ms/t, ftok | per-token latencies and time-to-first-token |
| kv gpu / kv cpu | KV context memory per backend |
| weights | model weight memory per backend |
| cache | prompt/RAM cache bytes and limit |
| rss | process resident set |
| used cells | global KV occupancy (unified) or sum (split) |
| lifetime counters | total prompt/gen/cached/decode since server start |

### 8.2 Per-sequence

| Field | Meaning |
|---|---|
| id | sequence id (== slot id for active, cache-entry id for evicted) |
| state | active / idle / evicted / gone |
| phase | IDL / PF / DEC (active or idle only) |
| loc | G (GPU KV), C (CPU KV), M (mixed), R (RAM cache only), - (none) |
| kv | used/reserved cells (== sequence length for causal models) |
| cch | cached tokens (prefix cache hits) |
| pp/t, tg/t | cumulative prompt / generation speed |
| pp5/t, tg5/t | sliding-window (~5 s) speeds |
| created / modified | timestamps (see section 5) |
| hit rate | per-sequence cache hit rate (see section 5) |
| text | full sequence text (active: growing; evicted: static) |

### 8.3 Semantics (locked definitions)

- Phase maps from the server slot state machine: idle, wait-other/started -> PF, processing-prompt -> PF, done-prompt -> PF (final), generating -> DEC.
- Occupied = slot holds KV cells (seq_pos_max >= 0) regardless of busyness. Active implies occupied; idle may be occupied (split mode, KV resident) or empty.
- len and kv are the same quantity for causal models (1 token = 1 KV cell).

## 9. Data availability tiers

- S1 (in-process, available): everything in section 8 except external probes and a few placeholders.
- N: real GPU component usage (nvidia-smi), CPU%, disk IO, request lifecycle phases, process RSS - implemented.
- B (backlog): see section 12.
- Any field without an implemented source renders `-`/n/a.

## 10. Backlog

Done (tiers N + B for the TUI): nvidia-smi GPU, /proc CPU/IO, request lifecycle phases, RSS, `[TOTAL]`, KV type, sleep-state publish, req/s, sliding-window speeds, RAM-cache `R` loc, multi-GPU.

Remaining / web-specific backlog:

- Web: sequence "resume" (later; read-only for now).
- Web: gap-detection re-sync handler on seq mismatch.
- Web: mobile/small-screen layout for the HxW grid.
- Shared: unify the TUI frame formatter and the web event emitter further (both already read the shared producer).

Excluded (documented non-goals):

- Per-op profiling / eval_callback breakdown (not multiseq-safe).
- Interactive control, graphs, remote monitoring.

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline (TUI design) | owner |
| 2026-02-14 | Open questions resolved (TUI): always ASCII; tails for all sequences; even height share; proportional tail token budget. | owner |
| 2026-02-14 | TUI major redesign: plain-text timely-output program, `--tui N`, tagged lines, no terminal control. | owner |
| 2026-02-14 | Tier B complete (TUI): `[TOTAL]`, KV type, sleep publish, req/s, pp5/tg5, RAM-cache R, multi-GPU; single-write + width-aware wrap fix. | owner |
| 2026-02-14 | Restructure into overall design + TUI detail (IMPLEMENTATION.md) + web detail (WEB.md). Add shared sequence model (identity/created/modified/hit-rate), shared event producer, web dashboard consumer (SSE, on by default, no switch). | owner |
