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
- TUI is active only when `--tui N` is set (N > 0). Output is plain tagged text, so no TTY check is needed; it works piped or to a file. `--tui 0` / omitted = off and stdout stays empty.
- No new external dependencies. GPU/CPU/disk data that requires external tools (nvidia-smi, /proc) is sourced by reading them directly; nothing is added to the build.
- The TUI is at its final version from the start: the full frame always renders. Fields whose data source is not implemented yet show a `-` placeholder.

## 4. Core principles (locked)

1. The "TUI" is a regular timely-output program: it prints one plain-text frame per second to stdout. No ncurses, no ANSI terminal control, no cursor/alternate-screen, no width/height calculation.
2. `--tui N` is the total number of lines per frame, printed exactly. Each frame pushes the screen upward, which is what makes it a "TUI" without in-place redraw.
3. One final frame format. No "demo version" distinction. Unretrievable fields render `-` and are filled in as data sources land.
4. Zero input handling. Ctrl-C keeps killing the whole server (existing signal path); there is no terminal state to restore.
5. Engine thread impact must be negligible: snapshot publish is throttled (target <= 10 Hz), printer emits at 1 Hz.
6. Information density over decoration. Everything shown must carry distinct meaning; duplicates are merged.

## 5. User experience

- A live plain-text status stream: every second it prints an exactly-N-line frame and scrolls the terminal upward (watch it directly, or pipe to `less`/a file).
- Every line is tagged with an uppercase bracket label (`[SERVER]`, `[RUN]`, `[MEMORY]`, `[THROUGHPUT]`, `[GPU]`, `[SYSTEM]`, `[SEQ n]`), so UI lines are instantly distinguishable from raw tail text and are greppable/parseable.
- Per-sequence blocks: `[SEQ n]` status line (k:v data) followed by its tail lines.
- Plain ASCII, no colors (pure text is pipe-friendly).

## 6. Screen format (target)

```
[SERVER] LLaMA 3.2 3B Q8_0 (app)  ctx 32768/128000  split  KV --  FA on  up 0:31h
[RUN] busy 3/4  queue 2  engine 78%
[MEMORY] kv gpu 5.1G  kv cpu 0M  weights 3.6G  cache 512M  rss 4.2G  cells 12.4k/32k
[THROUGHPUT] prompt 312/s  gen 45.2/s  spec 82%  hit 68%  pp 24ms  tg 22ms  ftok 1.24s
[GPU] SM 63%  mem 41%  pwr 245W  t 68C  pclk 1.9G  mclk 1.6G  pcie 80M/s
[SYSTEM] cpu 320%  ioR 45M/s  ioW 1M/s  req q 0.3s pp 1.2s dec 8.4s
[SEQ 0] PF G kv:##...... len:2048/8192 cch:900 pp:312.4 tg:- q:0.3s dec:136 rem:500 t:100
Once upon a time in a land far away, there lived a brave knight who traveled across mountains and
rivers to find the legendary golden sword hidden deep within the enchanted forest of whispers.
[SEQ 1] DEC G kv:######## len:8123/8192 cch:- pp:- tg:45.2 q:- dec:8123 rem:- t:101
<|im_start|>assistant
The user has just said "hello". I need to respond in a friendly and helpful manner. It's a common
[SEQ 2] IDL G kv:##...... len:2048/8192 cch:2048 pp:- tg:- q:- dec:- rem:- t:-
(idle 12s, KV resident)
(blank separator line between frames)
```

## 7. Layout rules (line budget)

- `--tui N`: N is the total number of lines in each printed frame (exactly). Default 0 = off.
- Frame structure:
  - 6 fixed tagged lines: `[SERVER]`, `[RUN]`, `[MEMORY]`, `[THROUGHPUT]`, `[GPU]`, `[SYSTEM]`.
  - one block per sequence, in slot order: `[SEQ n]` status line + `t_n` tail lines.
  - 1 trailing blank line (frame separator).
- Tail allocation (coarse): `tail_budget = N - 7 - n_shown`, where `n_shown` = number of `[SEQ]` blocks that fit. Each of the first cells gets `t = tail_budget / n_shown`, and the last cells get `t + 1` for the remainder (the last cell may be longer).
- If `N` cannot fit all `[SEQ]` lines, show the first that fit and emit `[SKIP] +K hidden` (counted in the budget).
- Every cell is padded to exactly `t_n` lines (blank lines) so the frame is always exactly N lines.
- Tail lines are newline-delimited segments of the pre-detokenized tail; no width wrapping (the terminal wraps visually).
- Tail token budget: `tokens_n ~= t_n x 32` (~100 chars/line, ~4 chars/token), clamped to `[64, 1024]`, computed on the engine side per slot; the snapshot stores each slot's `tail_lines`. No width-based feedback channel.

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

- Printer emits exactly one frame (N lines) per second.
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

## 13. Open questions (resolved)

- **Box drawing: always ASCII.** No unicode box-drawing or block characters in the TUI output. `+`, `-`, `|` borders; kv bar uses `#` / `.`.
- **Tail: always shown, for every sequence, in any state** (idle, prefill, decode). Idle slots show the retained prompt/generated tail; cleared idle slots show a short note in the box.
- **Tail window sizing:** the tail area spans the full width; height is evenly shared across all sequences. Each sequence block = 1 summary line + a tail box of T text rows, T = `(avail_rows / n_slots) - 3` (coarse, remainder distributed to first slots).
- **Tail token budget:** terminal size + slot count determine the tail window size, which determines how many tail tokens to detokenize per slot: `tokens ≈ (tail_width x T) / 4` (~4 chars/token), clamped to a sane min/max. The TUI thread writes this hint back into the snapshot; the engine thread uses it when building each tail. Coarse approximation is acceptable.

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline | owner |
| 2026-02-14 | Open questions resolved: always ASCII; tails always shown for all sequences; tail window full-width, height evenly shared; tail token budget proportional to window area (feedback hint from TUI to engine). | owner |
| 2026-02-14 | Major redesign: the TUI becomes a plain-text timely-output program. `--tui N` = total lines per frame (exactly N, trailing blank separator); tagged lines (`[SERVER]`, `[RUN]`, ..., `[SEQ n]`); no terminal control / width / height / ncurses; tail lines are newline segments (no wrapping); per-slot `tail_lines` stored in the snapshot, token budget `~ t x 32` per slot; `[SEQ]` gains `q:`/`dec:`/`rem:`/`t:` fields. | owner |
