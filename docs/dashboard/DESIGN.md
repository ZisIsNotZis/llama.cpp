# llama-server live monitoring - Overall design

Status: APPROVED BASELINE

Date: 2026-02-14

This is the overall/source-of-truth design for the live monitoring feature. It defines the shared model and producer used by ALL consumers:

- the **printui** (plain-text stdout printer, `--printui N`) - detailed design + implementation in IMPLEMENTATION.md
- the **terminal TUI** (ncurses, `--tui`) - detailed in section 7.3
- the **web dashboard** (SSE tab in the built-in UI) - detailed design + implementation in WEB.md

Any divergence from this design (or the detail docs) requires approval and a doc update first.

## Document control / change process

- These docs (DESIGN.md + IMPLEMENTATION.md + WEB.md) are the single source of truth for this feature.
- Every design or implementation change/compromise requires explicit approval from the owner before any code changes.
- The order is always: agree on the change -> update the docs -> then change code.
- Each approved change appends a row to the change log at the end of the affected doc.

## 1. Goals

- Provide a live, read-only view of every sequence (active in a slot, idle, or cached/evicted) plus the whole resource picture: model identity, throughput, latency, memory (GPU/CPU/RAM), and real GPU/CPU/disk utilization.
- Three surfaces with one shared data source:
  - a plain-text stdout stream, pipe-safe, no terminal control (`--printui N`).
  - a real terminal UI (`--tui`, ncurses): box-drawing, resize-aware, interactive scrolling, live token push.
  - the built-in web UI: a Dashboard tab with full scrollable per-sequence text, live push (no polling), scroll-follow, and cached sequences in a flat list. On by default, no switch.
- Per-sequence metadata everywhere: created time, last modified, cache hit rate.
- Keep it non-invasive: no behavior change when the TUIs are off, no engine impact.

## 2. Non-goals

- **printui** and the web dashboard are non-interactive; the **ncurses TUI is interactive** (mouse wheel, keyboard nav, maximize).
- No graphs, sparklines, or historical time-series plots.
- No remote monitoring: all surfaces are in-process and reflect the local server.
- No exposing model internals beyond what is already reachable or cheaply computable.
- No long polling for token-speed data (50 tok/s makes it dumb).
- No changes to inference throughput: the engine thread must not pay meaningful cost.

## 3. Constraints

- Runs inside the llama-server process (personal fork; keep changes clean and reviewable).
- **printui** (`--printui N`, renamed from the old `--tui N`): stdout reserved for it, logs stay on stderr. N > 0 = lines per frame; 0/omitted = off, stdout empty. Plain text, no terminal control, works piped.
- **terminal TUI** (`--tui`): ncurses, opt-in, default off. Requires a TTY. Uses the alternate screen buffer.
- `--printui` and `--tui` both write stdout: setting both is an error.
- Web dashboard: part of the built-in web UI. **On by default, no enable/disable switch.**
- Server-side dependencies: `ncurses` (terminal TUI) and `md4c` (permissive markdown) are new; everything else stays dependency-free (nvidia-smi, /proc read directly). Frontend uses modern libraries.
- The feature is at its final form from the start; fields without a source render `-`/n/a.

## 4. Core principles (locked)

1. **Multiple consumers, one producer**: the printui, the ncurses TUI, and the web SSE all read the same per-sequence event stream.
2. printui = plain-text timely-output program (no terminal control). ncurses TUI = smooth box-drawing UI (ncurses `doupdate()` diff-rendering, no full-clear flash, 30 Hz cap). Web = push-based (SSE), never polling.
3. Engine impact negligible: status publish throttled (<= 10 Hz); token deltas emitted per decode step (batched).
4. One final format each. Unretrievable fields render `-` and are filled as sources land.
5. printui and web are non-interactive; the ncurses TUI is interactive (mouse/keyboard) with a smooth, pinned-at-bottom scroll model.
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

### 7.1 printui (stdout printer) - see IMPLEMENTATION.md

Plain-text frames at 1 Hz, `--printui N` lines per frame, tagged `[SERVER]`/`[SEQ n]` lines, per-slot status + tail lines. Reads `tui_snap` (bounded tail). Non-interactive.

### 7.2 Web dashboard - see WEB.md

Dashboard tab in the built-in UI, SSE, on by default (no switch). Full scrollable per-cell text with immediate token push, terminal-emulator scroll-follow, HxW grid of active cells, cached sequences in a flat list, per-cell created/modified/hit-rate. Virtualized (lazy) rendering; no "load more" button. Reuses the chat UI's markdown + syntax-highlight renderer.

### 7.3 Terminal TUI (ncurses, `--tui`)

A real interactive terminal UI, opt-in (default off), requires a TTY. Uses the alternate screen buffer; box-drawing via ncurses `WACS` glyphs.

**Data**: reads `dash::feed` (full text + token deltas + active/evicted cells) for the text, and `tui_snap` (global + per-slot status) for the stats - the same two sources as the web dashboard.

**Smoothness**: ncurses double-buffered `wnoutrefresh`/`doupdate` writes only changed cells in one batched flush - no full-clear flash, no partial frames. Render loop wakes on the feed (condition variable) and redraws at most ~30 Hz. Resize via `KEY_RESIZE` re-layouts.

**Grid**: shared `grid_dims(n, rows, cols, ratio)` picks HxW to minimize **log-space loss** `|log(W/H) - log(target)| + lambda x log(1 + empty)` where `target = ratio x cols / rows`. `--tui-ratio R` is the target cell height:width; default `auto` = `usable_rows / cols` computed at TUI start (so cells are square on the usable grid area -> 4 cells give 2x2 on any screen, which the fixed 0.375 default did not: it gave 3x2 on a typical 120x25 grid). `[`/`]` tune it live (+/-0.02, clamped 0.08..1.0) and the current value shows on the totals header line. The web uses the same function with the default. Cells have a minimum width (~20 cols).

**Interaction**: mouse wheel over a cell scrolls it (at the bottom it is pinned and auto-follows; scrolling up holds position); double-click maximizes a cell and double-click again restores; `left`/`right` move selection (selected cell border is highlighted); `up`/`down` scroll the selected cell; `Enter` maximizes/restores; `[`/`]` tune the grid ratio live (shown in the header); `i` opens a bottom input bar targeting the selected cell - type and press `Enter` to fire a raw completion to that slot (`Shift+Enter` inserts a newline, `Esc` cancels), the response streams into the cell (slot-pinned via `id_slot`, no chat template); `k` aborts the running completion in the selected slot - it ends the generation gracefully (`STOP_TYPE_ABORT`, mapped to finish_reason "stop"): the slot sends its final response with the text generated so far, so the HTTP client connection completes cleanly like an end-of-stream instead of hanging; `s` saves the maximized (or selected) cell's raw text to `yymmddhhmmss.txt` in the working directory (transient confirmation on the status line); a thin right-edge scroll indicator shows position in scrollable cells; `q` detaches the TUI (server keeps running); Ctrl-C still kills the server. `i`/`k` fall back to the first active cell when nothing is selected. The browser dashboard mirrors all of this (input bar, kill, selection/maximize, `[`/`]`, grid_dims).

**Speculative stats**: when running with `-sp`, the header line shows `spec acc %` (accepted/proposed) and `len A/P` (avg accept length = 1 + accepted/verif steps, avg proposed length = draft tokens/verif steps), from the shared `global_snap`.

**Rendering pipeline (markdown + syntax highlight)**: the full cell text is parsed by **md4c** (MIT, streaming SAX, permissive - never crashes on truncated/invalid markdown) into a **styled-line model** (per-line ncurses attributes). Fenced code blocks are tokenized by a small embedded **lenient lexer** (on a malformed construct, stop highlighting that block and fall back to plain text; never crash) and mapped to ncurses color-pairs. The sliding window shows the last N styled lines, preserving block context (a fence opened above the window still highlights correctly). Languages: `python`/`py`, `bash`/`sh`, `c`/`cpp`/`c++`/`cxx`, `java`, `sql`, `json`/`json5`, `yaml`/`yml`; the lexer is extensible.

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
- TUI: fuller markdown (headers/bold/lists beyond code blocks) once the styled-line pipeline is proven.
- Shared: unify the printui frame formatter and the web event emitter further (both already read the shared producer).

Excluded (documented non-goals):

- Per-op profiling / eval_callback breakdown (not multiseq-safe).
- Graphs, remote monitoring.

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline (TUI design) | owner |
| 2026-02-14 | Open questions resolved (TUI): always ASCII; tails for all sequences; even height share; proportional tail token budget. | owner |
| 2026-02-14 | TUI major redesign: plain-text timely-output program, `--tui N`, tagged lines, no terminal control. | owner |
| 2026-02-14 | Tier B complete (TUI): `[TOTAL]`, KV type, sleep publish, req/s, pp5/tg5, RAM-cache R, multi-GPU; single-write + width-aware wrap fix. | owner |
| 2026-02-14 | Restructure into overall design + TUI detail (IMPLEMENTATION.md) + web detail (WEB.md). Add shared sequence model (identity/created/modified/hit-rate), shared event producer, web dashboard consumer (SSE, on by default, no switch). | owner |
| 2026-02-14 | Terminal TUI added: rename `--tui N` to `--printui N` (plain-text, stays on `tui_snap`); new `--tui` = ncurses interactive TUI on `dash::feed`+`tui_snap`, opt-in default off, both-writing-stdout is an error. Shared `grid_dims` with `--tui-ratio` (log-space-loss objective, default 0.375). Interaction: mouse wheel per-cell (pin-at-bottom), double-click maximize, L/R/U/D, Enter, scroll indicator, q detach. Markdown via `md4c` + embedded lenient lexer (py/bash/c/cpp/java/sql/json/yaml), styled-line window preserves block context. Web reuses the chat markdown renderer. | owner |
| 2026-02-14 | Implemented steps 1-2: `--printui N` rename (flag/field/env, `LLAMA_ARG_PRINTUI`), new `--tui` ncurses flag (`LLAMA_ARG_TUI`) with conflict check; ncurses TUI shell (`nctui.{h,cpp}`, ncursesw): alt-screen, render loop (~30 fps, `doupdate` diff for smoothness), box-drawing grid (shared `grid_dims` default ratio), global stats header + cell text windows from `dash::feed`/`printui::snapshot`, resize via `KEY_RESIZE`, `q`/Ctrl-C status. Known limitations: server stderr logs share the terminal (run `--tui 2>log`); interaction (step 4) and md4c/lexer (step 5) pending. | owner |
| 2026-02-14 | Implemented step 3: `grid_dims` moved to shared `printui::grid_dims` + `TUI_RATIO_DEFAULT`; new `--tui-ratio R` flag/field/env (`LLAMA_ARG_TUI_RATIO`, default 0.375, range (0,10] validated) plumbed into the TUI layout. The web dashboard (separate `llama-ui` repo, fetched at build) is to mirror `grid_dims` with the default ratio; not modifiable in this repo. | owner |
| 2026-02-14 | Implemented step 4 (interaction): per-cell scroll state (absolute-top anchor, -1 = pinned/auto-follow); wheel (BUTTON4/5) scrolls cell under cursor, holds position when scrolled up, returns to pinned at bottom; right-edge scroll indicator (thumb block); mouse click selects cell, double-click maximizes/restores (ncurses BUTTON1_CLICKED/DOUBLE_CLICKED via mouseinterval 250); L/R move selection (bold-green border), U/D scroll selected cell, Enter maximize/restore; q detaches (thread exits, server keeps running), Ctrl-C still SIGINTs via cbreak. Verified end-to-end with pty harness + ANSI decoder: wheel/indicator, dbl-click maximize, Enter restore, U/D scroll, q detach all pass. | owner |
| 2026-02-14 | Implemented step 5 (markdown + lexer): vendored `md4c.h` (MIT, release-0.4.8, matches system `libmd4c.so.0`; linked via `find_library` with `libmd4c.so.0` fallback) + new `markup.{h,cpp}`: md4c SAX -> styled-line model (runs with style bits + syntax color), GFM dialect, NOHTML. Fenced/indented code blocks tokenized by a config-driven lenient lexer (py/bash/c-cpp/java/sql/json/yaml), returns plain fallback on malformed constructs (never crashes). Render-time hard-wrapping of styled lines to the cell width (preserves per-run style across wrap), blank separator after headings/code blocks, per-cell parse cache keyed by `tseq`. ncurses maps style/color -> A_BOLD/A_ITALIC/A_DIM + syntax color pairs 10-18. Verified: markup unit dump (headings bold, inline bold/italic/code, blockquote, python+c colors, malformed-code fallback), SGR colors in pty output, all interaction tests still pass. Web dashboard (separate `llama-ui` repo) to reuse the chat markdown renderer; out of scope here. | owner |
| 2026-02-14 | Fixed shared snapshot bugs (TUI + web both read `printui::fill_snapshot`): (1) memory breakdown accumulated every fill - `global_snap g` carries over the previous frame, so the `+=` on kv/weights/compute grew ~31x/frame (kv cpu read 95G, weights grew to 15G; RSS ~4G was the truth); memory fields are now zeroed before summing, KV shows used-bytes (reserved x occupancy), weights stay constant. (2) pp/tg/gen gauges were cumulative or update-only-at-completion; added a global 5 s sample ring (mirrors the per-slot ring) over summed per-slot `n_gen` for realtime `tg_ms_tok`/`gen_tps`, and pp uses the prompt-bucket rate (`1000/prompt_tps`) because `n_prompt_processed` ticks during MTP decode and would pollute a ring. TUI cell header now shows realtime `pp5_tps`/`tg5_tps` per sequence. Verified: weights constant 580.3MB, kv cpu ~9-13MB, pp 11ms/tg 27ms/gen 37/s all realtime and mutually consistent. | owner |
| 2026-02-14 | TUI ergonomics round: `--tui-ratio` default changed to `auto` (-1 sentinel) = `usable_rows/cols` at TUI start, so cells are square and 4 cells give 2x2 on any screen (fixed 0.375 gave 3x2 on a typical 120x25 grid); `[`/`]` tune the ratio live (+/-0.02, clamped 0.08..1.0), current value shown on the totals header line; `s` saves the maximized/selected cell raw text to `yymmddhhmmss.txt` in the CWD with a transient status-line confirmation; draft stats added to `global_snap` (`spec_prop_len`, `spec_acc_len`) and shown on the header line when speculative (acc %, len A/P). | owner |
| 2026-02-14 | UI parity + in-process controls: `--printui` global header restructured to mirror the ncurses TUI's 4 lines (model/ctx; run + prompt/gen/hit + spec; pp/tg/ftok + used kv/weights/cache/rss; totals), keeping [GPU]/[SYSTEM] as extras and per-seq blocks unchanged (already a superset of the TUI cell header); FRAME_FIXED 8->7. TUI bottom input bar (`i`): raw completion to the selected slot via `id_slot` pinning (existing `get_available_slot` support) with no chat template - `tui_submit_completion` tokenizes the text as-is and posts a reader-less `SERVER_TASK_TYPE_COMPLETION`; `Shift+Enter` (`\e[13;2u`/`~`) newline, `Esc` cancel. TUI `k` kill: `tui_kill_slot` posts `SERVER_TASK_TYPE_CANCEL` with the slot's current task id, the engine releases the slot (same path as client-disconnect cancel). Verified: input bar + slot-pinned completion streams the response into the cell; `k` freezes kv and idles the slot. | owner |
| 2026-02-14 | Graceful kill + webui parity: `k` (and new `POST /abort {slot_id}` for the browser) now ENDS the completion like a stop token instead of hanging the HTTP client - the CANCEL handler sets `STOP_TYPE_ABORT` (new enum value, mapped to finish_reason "stop" in all oaicompat/stream paths) and calls `send_final_response` before `release`, so the client receives the text generated so far and the connection completes (~0.04 s). Chat re-parse can drop partial text on abort, so `update()` falls back to the raw `content`. `i`/`k` fall back to the first active cell when nothing is selected. Webui (`tools/ui/src/routes/dashboard/+page.svelte`, in this repo) now mirrors the TUI: ported `grid_dims` with auto ratio + `[`/`]` (shown in header), click-to-select (highlight), double-click maximize, per-cell kill button, bottom input bar (Enter send / Shift+Enter newline / Esc cancel) firing raw completions to the selected cell, spec len display. Verified: `/abort` ends a chat completion in 0.04 s with finish_reason "stop" and 721B partial text; UI builds clean and `/dashboard` serves 200. | owner |
