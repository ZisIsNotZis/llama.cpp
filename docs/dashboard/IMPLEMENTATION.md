# llama-server TUI - Implementation detail

Status: APPROVED BASELINE

Date: 2026-02-14

This document is the detailed design + implementation for the TUI consumer (plain-text stdout printer). The shared sequence model and event producer live in DESIGN.md (overall design); the web dashboard lives in WEB.md.

## 1. Scope and relation to DESIGN

- Implements the TUI consumer of the shared producer (DESIGN.md sections 5-7).
- Fields without a data source render `-` (DESIGN.md section 9).
- All locked semantics in DESIGN.md section 8.3 are authoritative.

## 2. TUI design

### 2.1 User experience

- A live plain-text status stream: every second it prints an exactly-N-line frame and scrolls the terminal upward (watch it directly, or pipe to `less`/a file).
- Every line is tagged with an uppercase bracket label (`[SERVER]`, `[RUN]`, `[MEMORY]`, `[THROUGHPUT]`, `[GPU]`, `[SYSTEM]`, `[TOTAL]`, `[SEQ n]`), so UI lines are instantly distinguishable from raw tail text and are greppable/parseable.
- Per-sequence blocks: `[SEQ n]` status line (k:v data) followed by its tail lines.
- Plain ASCII, no colors (pure text is pipe-friendly).

### 2.2 Screen format (target)

```
[SERVER] LLaMA 3.2 3B Q8_0 (app)  ctx 32768/128000  split  KV f16/f16  FA on  up 0:31h
[RUN] busy 3/4  queue 2  engine 78%
[MEMORY] kv gpu 5.1G  kv cpu 0M  weights 3.6G  cache 512M  rss 4.2G  cells 12.4k/32k
[THROUGHPUT] prompt 312/s  gen 45.2/s  req 3.5/s  spec 82%  hit 68%  pp 24ms  tg 22ms  ftok 1.24s
[GPU] SM 63%  mem 41%  pwr 245W  t 68C  pclk 1.9G  mclk 1.6G  pcie 80M/s
[SYSTEM] cpu 320%  ioR 45M/s  ioW 1M/s  req q 0.3s pp 1.2s dec 8.4s
[TOTAL] prompt 123456  gen 7890  cached 3456  decode 5000
[SEQ 0] PF G kv:##...... len:2048/8192 cch:900 pp:312.4 tg:- pp5:300.1 tg5:- q:0.3s dec:136 rem:500 t:100
Once upon a time in a land far away, there lived a brave knight who traveled across mountains and
rivers to find the legendary golden sword hidden deep within the enchanted forest of whispers.
[SEQ 1] DEC G kv:######## len:8123/8192 cch:- pp:- tg:45.2 pp5:- tg5:46.0 q:- dec:8123 rem:- t:101
<|im_start|>assistant
The user has just said "hello". I need to respond in a friendly and helpful manner. It's a common
[SEQ 2] IDL G kv:##...... len:2048/8192 cch:2048 pp:- tg:- pp5:- tg5:- q:- dec:- rem:- t:-
(idle 12s, KV resident)
(blank separator line between frames)
```

### 2.3 Layout rules (line budget)

- `--tui N`: N is the target number of visual rows per frame (default 0 = off).
- Frame structure:
  - 7 fixed tagged lines: `[SERVER]`, `[RUN]`, `[MEMORY]`, `[THROUGHPUT]`, `[GPU]`, `[SYSTEM]`, `[TOTAL]`.
  - one block per sequence, in slot order: `[SEQ n]` status line + up to `t_n` tail lines.
  - 1 trailing blank line (frame separator). Fixed lines total `FRAME_FIXED = 8`.
- Tail allocation (coarse, engine-side): `content = N - FRAME_FIXED`; `shown = min(n_slots, content)`; `tail_budget = content - shown - (hidden?1:0)`; per-slot `t_i = tail_budget / shown`, last cells get `+1` (last cell may be longer).
- If `N` cannot fit all `[SEQ]` lines, show the first that fit and emit `[SKIP] +K hidden`.
- Tail lines are newline-delimited segments of the pre-detokenized tail; no width wrapping.
- Tail token budget: `tokens_n ~= t_n x 32`, clamped to `[64, 1024]`, engine-side per slot; the snapshot stores each slot's `tail_lines`.
- Wrap fix: the printer reads the terminal width, estimates each line's visual rows as `ceil(display_cols / width)` (lines wrap in the terminal), and trims the emitted lines to ~N visual rows (always keeping the 7 fixed lines). Piped (no TTY) -> emit full frame.

### 2.4 Merging and dedup (TUI presentation)

- Sequence length == KV used cells -> one "kv/len" field.
- Busy state is conveyed by phase; a single "busy n/m" in `[RUN]`.
- Cache hits double as prefill progress.
- All RAM numbers -> `[MEMORY]`; all GPU/VRAM numbers -> `[GPU]`.
- Global gauges + spec + hit + req -> `[THROUGHPUT]`; latency + engine + phases -> `[SYSTEM]`; lifetime -> `[TOTAL]`.


## 3. Architecture overview

- One dedicated printer thread emits one frame per second to stdout. No terminal control, no ncurses, no width/height computation.
- The engine thread (server_context, single thread) publishes a bounded snapshot under a mutex, throttled to <= 10 Hz. Tails are pre-detokenized on the engine thread (bounded by per-slot `tail_lines`); the printer thread never calls llama APIs.
- The printer thread also reads the external probes (nvidia-smi, /proc) into `ext_snap`.
- No stdin handling, no keys. Ctrl-C goes through the existing signal path and kills the server; there is no terminal state to restore.

```
engine thread (server_context)                 printer thread
  update_slots()  -- every iteration -->
    if (now - last_publish >= 100ms)
      fill_snapshot(&snap, ctx_server)  (mutex)  every 1000ms:
      (computes per-slot tail_lines + tails)       read ext probes (nvidia-smi, /proc)
                                                  lock, copy snap
                                                  format one N-line frame to stdout
  shutdown: join thread
```

## 4. Files

New:

- `tools/server/tui.h` - snapshot types, printer lifecycle API, frame formatter entry point.
- `tools/server/tui.cpp` - printer thread, external probes, frame formatter (plain text).

Modified:

- `tools/server/server-context.cpp/.h` - publish snapshot after `update_slots()` (server-context.cpp:2699); engine-busy timing; `llama_get_memory_breakdown()` data; per-slot `tail_lines` allocation + tail detokenization.
- `tools/server/server-task.h` - `t_arrival_us` on `server_task`.
- `tools/server/server-queue.cpp` - stamp `t_arrival_us` in `post()` (queue-wait baseline).
- `tools/server/server.cpp` - `--tui` wiring; start/stop printer thread around `ctx_server.start_loop()`.
- `tools/server/server-common.h` - server params field `tui`.
- `common/arg.cpp`, `common/common.h` - `--tui N` flag and env `LLAMA_ARG_TUI`.
- `tools/server/server-models.cpp:1028` - move the router `LOG(...)` line to stderr so stdout has no other writer.
- `tools/server/CMakeLists.txt` - add `tui.cpp`.

## 5. CLI flag

- `--tui N`: N = total lines per printed frame (exactly), default 0 = off. `--tui 0` / omitted = off (stdout stays empty). Works regardless of TTY (plain text, pipe-safe). Router mode: not applicable, flag is ignored.
- Env: `LLAMA_ARG_TUI` (int).

## 6. Logging stream changes

- Today all llama-server logs go to stderr (verified: `common_log_entry::print()` in `common/log.cpp:89-93` sends level != NONE to stderr; INFO/WARN/ERROR/DEBUG all qualify).
- The only stdout writer is the router-mode `LOG("[%5d] %s", ...)` in `server-models.cpp:1028` (LEVEL_NONE -> stdout). Move it to stderr.
- Enforce: when `tui == 0`, stdout must remain empty to preserve current behavior for scripts/pipes/systemd.

## 7. Snapshot

Fixed-capacity structs preallocated once (no per-tick allocation in the engine thread). Token/char bounds are constants.

```cpp
// tools/server/tui.h
namespace tui {
constexpr int MAX_SLOTS      = 256;
constexpr int TAIL_CHARS     = 8192;  // max chars of tail kept per slot (cap)
constexpr int TAIL_TOKENS_MIN = 64;   // min tail tokens detokenized per slot
constexpr int TAIL_TOKENS_MAX = 1024; // max tail tokens detokenized per slot

enum class phase  { idle, prefill, decode };
enum class kv_loc { none, ram_cache, kv_cpu, kv_gpu, kv_mixed };

struct slot_snap {
    int       id;
    phase     ph;
    kv_loc    loc;
    bool      occupied;
    int32_t   n_ctx;          // reserved cells (split) or shared cap (unified)
    int32_t   kv_used;        // used cells (seq_pos_max+1)
    int32_t   n_prompt;       // total prompt tokens of task
    int32_t   n_prompt_proc;  // processed
    int32_t   n_prompt_cached;// cache hits
    int32_t   n_decoded;
    int32_t   n_remain;
    int64_t   t_last_used;    // us, 0 if never
    int32_t   idle_age_s;     // seconds since last use, -1 if n/a
    double    queue_ms;       // queue wait for the current task
    double    pp_tps;
    double    tg_tps;
    double    t_prompt_ms;    // pp time (first-token proxy)
    double    t_gen_ms;
    int32_t   id_task;
    int       tail_lines;     // tail lines to print for this slot (engine-computed)
    char      tail[TAIL_CHARS];
    int       tail_len;
};

struct global_snap {
    char      model_desc[256];
    char      alias[128];
    uint32_t  n_ctx, n_ctx_seq, n_ctx_train;
    uint32_t  n_slots;
    bool      kv_unified;
    char      flash_attn[16];
    bool      speculative;
    bool      sleeping;
    int       busy, idle, deferred;
    double    engine_busy;    // 0..1
    double    prompt_tps, gen_tps;
    double    spec_acc;       // -1 if n/a
    double    hit_rate;       // -1 if n/a
    double    pp_ms_tok, tg_ms_tok, first_tok_s;
    size_t    kv_gpu, kv_cpu, kv_other;
    size_t    weights_gpu, weights_cpu, weights_other;
    size_t    compute_gpu, compute_cpu, compute_other;
    size_t    ram_cache, ram_cache_max;
    size_t    rss;            // 0 if unavailable
    int64_t   uptime_s;
    // placeholders (0 = n/a) until nvidia-smi / proc land
    int       gpu_sm, gpu_mem, gpu_temp;
    // ...
};

struct snapshot {
    std::mutex   mtx;
    uint64_t     seq;       // incremented on each publish
    global_snap  global;
    slot_snap    slots[MAX_SLOTS];
    int          n_slots;
};
}
```

## 8. Data source mapping

As of the initial implementation, all S1 rows below are wired into the snapshot (see `tools/server/tui.h` and `fill_snapshot` in `server-context.cpp`). N/B rows still render `-`.

| Snapshot field | Source | Tier | Status |
|---|---|---|---|
| model_desc / alias | `llama_model_desc()` (include/llama.h:625), `params.model_alias` | S1 | available |
| n_ctx / n_ctx_seq / n_ctx_train | `llama_n_ctx` (:558), `llama_n_ctx_seq` (:559), `llama_model_n_ctx_train` (:579) | S1 | available |
| kv_unified | `params.kv_unified` | S1 | available |
| flash_attn | `params.flash_attn_type` via `llama_flash_attn_type_name` (:196) | S1 | available |
| kv type | `llama_get_kv_cache_types()` (src/llama-ext.h), fills `[SERVER] ... KV f16/f16` | B | implemented |
| phase | `server_slot::state` enum (server-context.cpp:57) | S1 | available |
| occupied | `llama_memory_seq_pos_max(ctx, id)` (llama.h:791) >= 0, or `slot.prompt.n_tokens()>0` | S1 | available |
| kv_used | `llama_memory_seq_pos_max + 1` | S1 | available |
| n_ctx (reserved) | `slot.n_ctx` (split) / `n_ctx` (unified) | S1 | available |
| loc | KV resident? + `llama_get_memory_breakdown()` backends (src/llama-ext.h:91); RAM-cache-only from server cache state | S1 | available |
| n_prompt/proc/cached | `slot.stats.n_prompt_processed/n_prompt_cached`, `slot.prompt.n_tokens()` | S1 | available |
| pp/tg tps | `server_slot_stats::n_prompt_tps()/n_gen_tps()` (server-common.h:415-417) | S1 | available |
| t_prompt_ms / t_gen_ms | `server_slot_stats` | S1 | available |
| n_remain | `slot.n_remaining()` (server-context.cpp:~425) | S1 | available |
| t_last_used | `slot.t_last_used` | S1 | available |
| tail | `slot.generated_tokens` (and `slot.prompt.tokens` during prefill) -> last `TAIL_TOKENS` tokens -> `common_detokenize(ctx_tgt, suffix, true)` | S1 | available (see 9.4) |
| busy / idle | sum of `slot.is_processing()` | S1 | available |
| deferred | `queue_tasks.queue_tasks_deferred_size()` (server-context.cpp:2443) | S1 | available |
| sleeping | `queue_tasks.is_sleeping()` | S1 | available |
| engine_busy | new timing accumulation around `update_slots()` (server-context.cpp:2699) | S1 | implement |
| prompt_tps / gen_tps | `server_metrics.prompt_bucket/predict_bucket.n_per_second()` (server-common.h:452,459) | S1 | available |
| spec_acc | `metrics.n_draft_accepted / n_draft_tokens` (only when `params.speculative` enabled) | S1 | available |
| hit_rate | `metrics.n_prompt_cached / (metrics.prompt.count + metrics.n_prompt_cached)` | S1 | available |
| kv/weights/compute per backend | `llama_get_memory_breakdown(ctx_tgt)` (src/llama-ext.h:91) | S1 | available |
| ram_cache | `prompt_cache->size()` and limit (server-task.h:592) | S1 | available |
| rss | Linux: `/proc/self/statm` (or `getrusage` peak fallback); else 0 | S1 | Linux only |
| uptime | `metrics.t_start` vs now | S1 | available |
| GPU SM/mem/temp/pwr/clocks | `nvidia-smi --query-gpu=...` one-shot subprocess on the TUI thread (tui.cpp) | N | implemented (Linux) |
| pcie rx/tx | `nvidia-smi --query-gpu=pcie.rx_bytes,pcie.tx_bytes` (separate probe; not on all drivers) | N | implemented (falls back to `-`) |
| cpu% | `/proc/self/stat` utime+stime deltas on the TUI thread | N | implemented (Linux) |
| ioR/ioW | `/proc/self/io` read_bytes/write_bytes deltas | N | implemented (Linux) |
| req lifecycle phases | `server_task.t_arrival_us` (stamped in `server_queue::post()`) + `slot.stats.t_start` -> queue wait; prefill/decode from slot stats; shown for the latest active task | N | implemented |
| lifetime counters | `server_metrics.prompt.count / predict.count / n_prompt_cached / n_decode` -> `[TOTAL]` line | B | implemented |
| req/s | `server_queue.n_requests` (user task types, counted in `post()`) + 5 s rate ring -> `[THROUGHPUT] ... req X/s` | B | implemented |
| sliding-window speeds | per-slot 32-sample ring of `(t, n_prompt_processed, n_gen)` -> `[SEQ] pp5:/tg5:` | B | implemented |
| RAM-cache `R` loc | `server_prompt_cache_state.id_slot` (set in `alloc`) + `has_state_for(id)` -> idle slot loc `R` | B | implemented |
| multi-GPU | nvidia-smi parses all GPU lines: first GPU's SM/mem/temp/clocks, summed power, `xN` prefix | B | implemented |

Note on loc: KV may be on GPU and CPU simultaneously for a layer-split model; loc derives from which backends are present in the breakdown for the KV context buffer and whether the slot is resident. RAM-cache-only requires the server to know an idle slot was cleared after being saved (see section 9.5).

## 9. Terminal handling

None. The printer thread only does `fwrite`/`fflush(stdout)`. No alt-screen, no cursor control, no `ioctl`, no resize handling, no signal-unsafe work. Ctrl-C path is unchanged.

## 10. Frame format and line budget

- The printer builds one frame per second: exactly `N` lines, `N = clamp(tui, 8, 200)`.
- Lines 1-7 are tagged: `[SERVER] ...`, `[RUN] ...`, `[MEMORY] ...`, `[THROUGHPUT] ...`, `[GPU] ...`, `[SYSTEM] ...`, `[TOTAL] ...` (cumulative lifetime counters).
- Then per-slot blocks, then 1 trailing blank line (frame separator). Fixed lines total `FRAME_FIXED = 8` (7 tags + blank).
- Allocation (section 2.3):
  - `content = N - FRAME_FIXED` lines available for `[SEQ]` + tails.
  - `shown = min(n_slots, content)`; `hidden = n_slots - shown`; `note = hidden > 0 ? 1 : 0`; if `shown + note > content`, shrink `shown` to `content - note`.
  - `tail_budget = content - shown - note`; per-slot `t_i = tail_budget / shown`, and the last `rem = tail_budget % shown` cells get `+1` (the last cell may be longer).
  - if `hidden > 0`, emit `[SKIP] +K hidden` as the final block line.
- Each block = `[SEQ n] <summary>` + exactly `t_i` tail lines (blank-padded), so the frame is always exactly `N` lines.
- `[SEQ n]` summary: `ph loc kv:<bar> len:<used/ctx> cch:<n> pp:<tps> tg:<tps> q:<queue s> dec:<n> rem:<n> t:<task id>`; `-` for n/a.
- Tail lines = newline-delimited segments of the slot's pre-detokenized `tail` (the last `t_i` of them); no width wrapping (the terminal wraps visually).

### 10.5 RAM-cache-only detection

- When `--cache-idle-slots` + unified mode clear an idle slot, `prompt_clear()` empties its prompt and KV. The cleared slot has no KV cells and no prompt, so it shows loc `R` (RAM-cached) only if the server can attribute a cache entry to it.
- v1: if slot is idle, KV empty, and a matching entry exists in `server_prompt_cache`, show `R`; otherwise `-`. Exact attribution (cache entry -> slot) may need a small addition to `server_prompt_cache` (store owning slot id); that is optional/backlog. Simpler v1 fallback: show global `cache` bytes in MEMORY and per-slot `-` for cleared slots.

### 10.6 Units and formatting

- Bytes: humanized (B, kB, MB, GB); rates: tokens/s, MB/s; latency: ms or s with one decimal.
- kv bar is `#` (filled) / `.` (empty), 8 wide, by fraction used.
- Plain text, no colors. Values that are not applicable (e.g. tg during prefill) render `-`, not 0.

## 11. Concurrency

- Snapshot guarded by `std::mutex`; publish and read both take the lock briefly. Publish copies fixed structs only; no allocation per tick (tail scratch reused).
- Printer thread writes stdout only; engine thread never writes to stdout.
- Engine-busy instrumentation: timing accumulation in `update_slots()` entry/exit; rolling 5 s window computed from a small ring.
- No llama API call happens on the printer thread (all derived values, including tails, are precomputed in the snapshot). This keeps thread-safety trivially correct.
- The per-slot `tail_lines` and the tail text are both computed on the engine thread in `fill_snapshot`, so the printer just prints what the snapshot says.

## 12. Build integration

- Add `tui.cpp` to the `server-context` target in `tools/server/CMakeLists.txt` (it needs access to `server_slot`, `server_metrics`, `prompt_cache`, and `llama_ext`).
- Include `src/llama-ext.h` (already reachable via the target's `${CMAKE_SOURCE_DIR}` include dir; common/ already uses it) to get `llama_get_memory_breakdown`.
- No new link dependencies; std::thread already used.

## 13. Testing / validation

- `--tui` absent or `0`: stdout stays empty, behavior unchanged (piped to file).
- `--tui N` piped to a file: each frame is exactly N lines (verify with `wc -l` across frames), tags present, blank separator line between frames.
- Multi-slot: send concurrent completions (2-8 slots), verify `[SEQ n]` blocks, phase transitions, idle notes, `busy n/m`, `queue`, `[SKIP]` when N too small.
- Unified vs split: run both, verify kv/len semantics (split per-slot reserved, unified shared occupancy) and loc G/C/R/- behavior.
- Tail: multi-line chat content stays aligned (newline-split), tail lines blank-pad to `t_i`.
- Cache: repeat a prompt prefix, verify `cch` and `hit%` increase.
- Spec: run with `-sp`, verify `spec acc`.
- Memory: verify kv gpu/cpu, weights, cache, rss against a known config; cross-check with `nvidia-smi` and `/proc` when available.
- Impact: run a fixed benchmark with and without `--tui`, confirm engine throughput delta is within noise.

## 14. Risks and notes

- The engine thread publishes under a mutex at <= 10 Hz; if profiling shows measurable impact, reduce publish rate.
- `src/llama-ext.h` is a staging header ("try as much as possible to not include in the rest of the codebase"). Using it from the server is acceptable for a personal fork but should be isolated in one place (server-context), not spread around.
- The router LOG->stderr change is the only stdout behavior change and only affects router mode.
- stdout is a live stream; when `--tui N` is set the server writes N lines/second to stdout, so consumers should pipe to a file or `less`.

## 15. Implementation checklist

1. `--tui N` (int) in common_params + arg + server wiring.
2. Move router LOG to stderr; verify stdout empty when `tui == 0`.
3. Snapshot types + publish hook in `update_slots()` (throttled) + engine-busy timing.
4. Printer thread lifecycle + 1 Hz loop (no terminal control).
5. Frame formatter: tagged lines + N-line budget + per-slot `[SEQ]` + tail lines.
6. `fill_snapshot`: per-slot `tail_lines` allocation + bounded tail detokenization; fill all S1 fields; N/B placeholders.
7. External probes (nvidia-smi, /proc) on the printer thread.
8. CMake + build + manual validation (section 12).

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline | owner |
| 2026-02-14 | Initial implementation: snapshot struct, publish hook, controller, renderer, `--tui` flag, router LOG->stderr, `tui.cpp`/`tui.h`. S1 fields implemented; N/B placeholders. | owner |
| 2026-02-14 | Tier N (partial): GPU panel via `nvidia-smi` (SM/mem/temp/pwr/clocks, pcie fallback) and SYSTEM panel via `/proc` (process CPU%, disk IO rates), read on the TUI thread into `ext_snap`. | owner |
| 2026-02-14 | Tier N complete: request lifecycle phases (`req q Xs pp Xs dec Xs`) via `server_task.t_arrival_us` stamped in `server_queue::post()`. | owner |
| 2026-02-14 | Design change (open questions): always ASCII; tails shown for all sequences in any state; tail window full-width with evenly shared height; tail token budget proportional to window area (TUI writes `tail_tokens_hint` into the snapshot). | owner |
| 2026-02-14 | Tail box fix: split tail text on newlines, wrap each logical line, sanitize control chars so the box stays aligned with multi-line chat content. | owner |
| 2026-02-14 | Design change: drop the `...` truncation marker (broke alignment); SEQUENCES uses inline `k:v` summary labels instead of a far-away table header (column header row removed, fixed band now 10 rows). | owner |
| 2026-02-14 | Major redesign: plain-text timely-output program. `--tui N` = exact lines per frame; tagged lines (`[SERVER]`/`[RUN]`/.../`[SEQ n]`); no terminal control/width/ncurses; per-slot `tail_lines` computed on the engine thread; tail token budget `~ t x 32`; `[SEQ]` gains `q:`/`dec:`/`rem:`/`t:`; trailing blank frame separator. | owner |
| 2026-02-14 | Tier B (partial): `[TOTAL]` lifetime counters line; `[SERVER]` KV cache types via `llama_get_kv_cache_types()` (new llama-ext API); snapshot publishes on sleep-state change (model info carried over, no more stale `[RUN]` while sleeping). | owner |
| 2026-02-14 | Bug fix: one concatenated single-write frame (no jitter); terminal width at boot used to estimate per-line visual rows (lines wrap in the terminal) and trim the frame to ~N visual rows; no manual wrapping. | owner |
| 2026-02-14 | Tier B complete: `req/s` (task counter + rate ring), sliding-window per-slot `pp5:`/`tg5:` speeds, per-slot RAM-cache `R` attribution (`server_prompt_cache_state.id_slot`), multi-GPU `[GPU]` (all nvidia-smi lines, summed power, `xN`). | owner |
| 2026-02-14 | Restructured: this doc is now the TUI detail; the shared sequence model/event producer moved to DESIGN.md (overall); the web dashboard lives in WEB.md. | owner |
