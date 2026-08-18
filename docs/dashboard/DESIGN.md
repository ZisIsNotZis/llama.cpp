# llama-server TUI - Design

Status: APPROVED BASELINE

Date: 2026-02-14

This document is the source of truth for the TUI feature design. It describes WHAT and WHY, not how to implement it (see IMPLEMENTATION.md).

## Document control / change process

- These docs (DESIGN.md + IMPLEMENTATION.md) are the single source of truth for this feature.
- Every design or implementation change/compromise requires explicit approval from the owner before any code changes.
- The order is always: agree on the change -> update the docs -> then change code.
- Each approved change appends a row to the change log at the end of the affected doc.

## 1. Goals

- Provide a live, read-only, btop-style dashboard for llama-server shown directly in the terminal running the server.
- Show all sequences (server slots), both active and idle, with their memory footprint and phase.
- Show the whole resource picture: model identity, throughput, latency, memory (GPU/CPU/RAM), and, over time, real GPU/CPU/disk utilization.
- Show the tail of each active sequence as a multi-line auto-wrapping window that grows to fill the terminal.
- Give a real answer to "is the GPU actually busy and where is the bottleneck" for a running llama-server.
- Keep the feature opt-in and non-invasive: no behavior change when not enabled or when stdout is not a terminal.

## 2. Non-goals

- No interactive input at all: no keys, no mouse, no selection, no configuration inside the TUI.
- No graphs, sparklines, or historical time-series plots (explicitly excluded).
- No remote monitoring: the TUI is in-process and only reflects the local server.
- No exposing model internals beyond what is already reachable; we only surface data llama-server already has or can cheaply compute.
- No changes to inference throughput: the engine thread must not pay meaningful cost.

## 3. Constraints

- Runs inside the llama-server process (personal fork; no maintainer approval required, but keep the change clean and reviewable).
- stdout is reserved for the TUI. All logs stay on stderr (this is already the case today; see IMPLEMENTATION.md).
- TUI only active when stdout is a TTY and `--tui` is enabled (or auto default). Otherwise stdout stays empty and current behavior is unchanged.
- No new external dependencies. GPU/CPU/disk data that requires external tools (nvidia-smi, /proc) is sourced by reading them directly; nothing is added to the build.
- The TUI is at its final version from the start: the full layout always renders. Fields whose data source is not implemented yet show a `-` placeholder.

## 4. Core principles (locked)

1. Full-screen btop-style layout, auto-maximizing to the current terminal size.
2. The sequences/tail area owns all space not needed by the compact top band; it resizes with the terminal (re-queried each refresh tick, no signal handler needed for correctness; see backlog for immediate redraw).
3. One final layout. No "demo version" distinction. Unretrievable fields render `-` and are filled in as data sources land.
4. Zero input handling. Ctrl-C keeps killing the whole server (existing signal path); the TUI restores the terminal during shutdown.
5. Engine thread impact must be negligible: snapshot publish is throttled (target <= 10 Hz), TUI repaints at 1 Hz.
6. Information density over decoration. Everything shown must carry distinct meaning; duplicates are merged.

## 5. User experience

- btop-like: a compact top band (identity/status, memory, throughput, GPU, system) plus a dominant SEQUENCES box.
- Live tail windows per active sequence that auto-wrap and expand to available screen space.
- Color-coded, read-only, no cursor, no flicker (full frame redraw each tick inside the alternate screen buffer).
- ASCII-safe fallback for unusual terminals; unicode box drawing preferred (btop look).

## 6. Screen layout (target)

```
╭─ llama-server ────────────────────────────────────────────────────────────╮
│ LLaMA 3.2 3B Q8_0  (app)   ctx 32k/128k   split   KV f16/f16   FA  on    │
│ RUN   busy 3/4   queue 2   engine 78%   ref 1.0s   up 0:31:12            │
├─ MEMORY ─────────────────────────┬─ THROUGHPUT ──────────────────────────┤
│ kv gpu 5.1G   kv cpu 0M          │ prompt 312 t/s     gen 45.2 t/s      │
│ cache 512M    rss 4.2G           │ spec acc 82%       hit 68%           │
│ used 12.4k/32k cells  39%        │ pp 24ms/t  tg 22ms/t  ftok 1.24s     │
├─ GPU 0 · RTX 4090 ───────────────┴─ SYSTEM ──────────────────────────────┤
│ SM 63%  mem 41%  pwr 245W  temp 68C  pclk 1.9G  mclk 1.6G               │
│ pcie rx 1.2G/s  tx 80M/s         cpu 320%  ioR 45M/s  ioW 1M/s          │
├─ SEQUENCES · 4 ──────────────────────────────────────────────────────────┤
│  #  ph   kv       loc  len      cch   pp/t   tg/t                      │
│  0  PF   ████░░░  G    2048/8192 900  312.4   -                        │
│  ╭ tail ────────────────────────────────────────────────────────────╮   │
│  │ Once upon a time in a land far away, there lived a brave knight  │   │
│  │ who traveled across mountains and rivers to find the legendary   │   │
│  │ golden sword hidden deep within the enchanted forest...          │   │
│  ╰──────────────────────────────────────────────────────────────────╯   │
│  1  DEC  ███████  G    8123/8192  -    -       45.2                  │
│  ╭ tail ────────────────────────────────────────────────────────────╮   │
│  │ ...whispers. He met many creatures along the way including       │   │
│  │ dragons and fairies and wizards who helped him on his noble      │   │
│  ╰──────────────────────────────────────────────────────────────────╯   │
│  2  IDL  ███░░░░  G    2048/8192  2048  -       -    idle 12s        │
│  3  IDL  -        R    -         -     -       -    RAM-cached       │
╰──────────────────────────────────────────────────────────────────────────╯
```

## 7. Layout and resize rules

- Top band is fixed height (~7 rows): title row, status row, MEMORY|THROUGHPUT (3 rows), GPU|SYSTEM (2 rows).
- All remaining rows belong to SEQUENCES.
- SEQUENCES box: 1 header row, then blocks:
  - active (PF/DEC): summary line + tail box of T rows, T = floor(available / n_active), clamped to [1, cap].
  - idle: one compact line each, no tail box, listed after active. If they do not fit, drop with a "+N idle" note.
- Tail window: shows the most recent T x W characters of the sequence, auto-wrapped; scrolls as new tokens arrive.
- Width: two-column panels when W >= ~100. Narrower terminals stack the top panels full-width (rule in this doc; initial implementation may assume two columns and truncate).
- Resize: terminal size is re-queried every refresh tick, so resize is reflected on the next tick automatically. Immediate redraw on SIGWINCH is backlog (nice to have).

## 8. Data model

### 8.1 Global (top band)

| Field | Meaning |
|---|---|
| model desc + alias | what is loaded |
| ctx / ctx_seq / ctx_train | context configuration |
| kv mode | unified or split |
| kv type, flash attention | KV quantization + FA config (placeholder until a source exists) |
| status | RUN / SLEEPING (model unloaded) |
| busy n/m | slots processing / total |
| queue | deferred tasks waiting for a slot |
| engine % | fraction of wall time the engine thread spends in update_slots (llama-side utilization) |
| uptime, refresh | server uptime, refresh interval |
| prompt/gen tps | global throughput gauges (live window) |
| spec acc % | speculative decoding acceptance (only when enabled) |
| cache hit % | prefix-cache hit ratio |
| pp ms/t, tg ms/t, ftok | per-token latencies and time-to-first-token |
| kv gpu / kv cpu | KV context memory per backend |
| weights | model weight memory per backend |
| cache | prompt/RAM cache bytes and limit |
| rss | process resident set |
| used cells | global KV occupancy (unified) or sum (split) |

### 8.2 Per-slot (SEQUENCES)

| Field | Meaning |
|---|---|
| id | slot id (== llama seq id) |
| ph | phase: IDL / PF / DEC (idle, prefill, decode) |
| kv | used/reserved cells + mini bar |
| loc | where the slot KV lives: G (GPU), C (CPU), G/C (both), R (RAM-cache only), - (none) |
| len | used/reserved cells (== sequence length for causal models) |
| cch | cached tokens (prefix cache hits for this task); doubles as prefill progress "PF p/t c:hits" |
| pp/t, tg/t | prompt / generation speed (tokens/s) |
| tail | rolling window of latest text |
| idle info | for idle slots: LRU age, or "RAM-cached" when resident KV was cleared |

### 8.3 Semantics (locked definitions)

- Phase maps from the server slot state machine: idle, wait-other/started -> PF, processing-prompt -> PF, done-prompt -> PF (final), generating -> DEC.
- Occupied = slot holds KV cells (seq_pos_max >= 0) regardless of busyness. Active implies occupied; idle may be occupied (split mode, KV resident) or empty.
- loc is per-slot but KV may span GPU+CPU simultaneously when the model is split across layers; G/C is allowed for mixed.
- len and kv are the same quantity for causal models (1 token = 1 KV cell); they are merged into one column. No separate length column.

## 9. Data availability tiers and placeholders

- S1 (available now, in-process): everything in section 8 except GPU/SYSTEM external probes and kv type.
- N (next, needs external probes or small instrumentation): real GPU component usage (nvidia-smi), CPU%, disk IO, request lifecycle phases (queue -> prefill -> decode).
- B (backlog): HTTP req/s, cumulative lifetime counters (footer), per-op profiling, immediate resize redraw, KV type getter.
- Any field without an implemented source renders `-` or `n/a`. The layout never shrinks; only values are placeholder.

## 10. Merging and dedup rules

- Sequence length == KV used cells for causal models -> one "kv/len" column.
- Busy state is conveyed by phase per slot; a single "busy n/m" sits in the status row. No per-slot busy column.
- Cache hits double as prefill progress in the same field.
- All RAM numbers (rss, cache, kv-on-cpu) live in one MEMORY panel. All GPU/VRAM numbers live in one GPU panel. No KV memory shown in two places.
- Global gauges + spec + hit rate -> THROUGHPUT. Latency + engine -> SYSTEM.
- Cumulative lifetime counters are backlog only (potential footer), never in the main view.

## 11. Refresh and timing

- TUI repaint: 1 Hz fixed.
- Snapshot publish from the engine thread: throttled to <= 10 Hz, bounded size, no allocation per tick (see IMPLEMENTATION.md).
- No time-based smoothing in v1; per-slot speeds are cumulative for the current task (moving window is backlog).

## 12. Backlog

- Immediate redraw on SIGWINCH (correctness already handled by per-tick re-query).
- nvidia-smi one-shot integration (GPU SM/mem/temp/power/clocks/PCIe) -> N.
- /proc integration (CPU%, disk IO rates, process RSS on Linux) -> N.
- Request lifecycle phases with queue-wait timing -> N.
- HTTP req/s and stream throughput -> B.
- Cumulative lifetime counters (footer) -> B.
- KV type getter (public API) -> B.
- Per-op profiling / eval_callback breakdown -> excluded (not multiseq-safe, out of scope).

## 13. Open questions

- Unicode box drawing vs ASCII fallback in practice (implementation detail, default: unicode with ASCII fallback).
- Exact tail token window size and whether prompt tail is shown during prefill (default: generated tail always; prompt tail only during prefill).

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline | owner |
