# llama-server TUI - Implementation

Status: APPROVED BASELINE

Date: 2026-02-14

This document is the source of truth for HOW to implement the TUI per DESIGN.md. Any divergence from DESIGN.md requires approval and a doc update first.

## 1. Scope and relation to DESIGN

- Implements the tagged plain-text frame format from DESIGN.md section 6.
- Fields without a data source render `-` (DESIGN.md section 9).
- All locked semantics in DESIGN.md section 8.3 are authoritative.

## 2. Architecture overview

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

## 3. Files

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

## 4. CLI flag

- `--tui N`: N = total lines per printed frame (exactly), default 0 = off. `--tui 0` / omitted = off (stdout stays empty). Works regardless of TTY (plain text, pipe-safe). Router mode: not applicable, flag is ignored.
- Env: `LLAMA_ARG_TUI` (int).

## 5. Logging stream changes

- Today all llama-server logs go to stderr (verified: `common_log_entry::print()` in `common/log.cpp:89-93` sends level != NONE to stderr; INFO/WARN/ERROR/DEBUG all qualify).
- The only stdout writer is the router-mode `LOG("[%5d] %s", ...)` in `server-models.cpp:1028` (LEVEL_NONE -> stdout). Move it to stderr.
- Enforce: when `tui == 0`, stdout must remain empty to preserve current behavior for scripts/pipes/systemd.

## 6. Snapshot

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

## 7. Data source mapping

As of the initial implementation, all S1 rows below are wired into the snapshot (see `tools/server/tui.h` and `fill_snapshot` in `server-context.cpp`). N/B rows still render `-`.

| Snapshot field | Source | Tier | Status |
|---|---|---|---|
| model_desc / alias | `llama_model_desc()` (include/llama.h:625), `params.model_alias` | S1 | available |
| n_ctx / n_ctx_seq / n_ctx_train | `llama_n_ctx` (:558), `llama_n_ctx_seq` (:559), `llama_model_n_ctx_train` (:579) | S1 | available |
| kv_unified | `params.kv_unified` | S1 | available |
| flash_attn | `params.flash_attn_type` via `llama_flash_attn_type_name` (:196) | S1 | available |
| kv type | no public getter | B | placeholder `-` |
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

Note on loc: KV may be on GPU and CPU simultaneously for a layer-split model; loc derives from which backends are present in the breakdown for the KV context buffer and whether the slot is resident. RAM-cache-only requires the server to know an idle slot was cleared after being saved (see section 9.5).

## 8. Terminal handling

None. The printer thread only does `fwrite`/`fflush(stdout)`. No alt-screen, no cursor control, no `ioctl`, no resize handling, no signal-unsafe work. Ctrl-C path is unchanged.

## 9. Frame format and line budget

- The printer builds one frame per second: exactly `N` lines, `N = clamp(tui, 8, 200)`.
- Lines 1-6 are tagged: `[SERVER] ...`, `[RUN] ...`, `[MEMORY] ...`, `[THROUGHPUT] ...`, `[GPU] ...`, `[SYSTEM] ...`.
- Then per-slot blocks, then 1 trailing blank line (frame separator).
- Allocation (matches DESIGN.md section 7):
  - `content = N - 7` lines available for `[SEQ]` + tails.
  - `shown = min(n_slots, content)`; `hidden = n_slots - shown`; `note = hidden > 0 ? 1 : 0`; if `shown + note > content`, shrink `shown` to `content - note`.
  - `tail_budget = content - shown - note`; per-slot `t_i = tail_budget / shown`, and the last `rem = tail_budget % shown` cells get `+1` (the last cell may be longer).
  - if `hidden > 0`, emit `[SKIP] +K hidden` as the final block line.
- Each block = `[SEQ n] <summary>` + exactly `t_i` tail lines (blank-padded), so the frame is always exactly `N` lines.
- `[SEQ n]` summary: `ph loc kv:<bar> len:<used/ctx> cch:<n> pp:<tps> tg:<tps> q:<queue s> dec:<n> rem:<n> t:<task id>`; `-` for n/a.
- Tail lines = newline-delimited segments of the slot's pre-detokenized `tail` (the last `t_i` of them); no width wrapping (the terminal wraps visually).

### 9.5 RAM-cache-only detection

- When `--cache-idle-slots` + unified mode clear an idle slot, `prompt_clear()` empties its prompt and KV. The cleared slot has no KV cells and no prompt, so it shows loc `R` (RAM-cached) only if the server can attribute a cache entry to it.
- v1: if slot is idle, KV empty, and a matching entry exists in `server_prompt_cache`, show `R`; otherwise `-`. Exact attribution (cache entry -> slot) may need a small addition to `server_prompt_cache` (store owning slot id); that is optional/backlog. Simpler v1 fallback: show global `cache` bytes in MEMORY and per-slot `-` for cleared slots.

### 9.6 Units and formatting

- Bytes: humanized (B, kB, MB, GB); rates: tokens/s, MB/s; latency: ms or s with one decimal.
- kv bar is `#` (filled) / `.` (empty), 8 wide, by fraction used.
- Plain text, no colors. Values that are not applicable (e.g. tg during prefill) render `-`, not 0.

## 10. Concurrency

- Snapshot guarded by `std::mutex`; publish and read both take the lock briefly. Publish copies fixed structs only; no allocation per tick (tail scratch reused).
- Printer thread writes stdout only; engine thread never writes to stdout.
- Engine-busy instrumentation: timing accumulation in `update_slots()` entry/exit; rolling 5 s window computed from a small ring.
- No llama API call happens on the printer thread (all derived values, including tails, are precomputed in the snapshot). This keeps thread-safety trivially correct.
- The per-slot `tail_lines` and the tail text are both computed on the engine thread in `fill_snapshot`, so the printer just prints what the snapshot says.

## 11. Build integration

- Add `tui.cpp` to the `server-context` target in `tools/server/CMakeLists.txt` (it needs access to `server_slot`, `server_metrics`, `prompt_cache`, and `llama_ext`).
- Include `src/llama-ext.h` (already reachable via the target's `${CMAKE_SOURCE_DIR}` include dir; common/ already uses it) to get `llama_get_memory_breakdown`.
- No new link dependencies; std::thread already used.

## 12. Testing / validation

- `--tui` absent or `0`: stdout stays empty, behavior unchanged (piped to file).
- `--tui N` piped to a file: each frame is exactly N lines (verify with `wc -l` across frames), tags present, blank separator line between frames.
- Multi-slot: send concurrent completions (2-8 slots), verify `[SEQ n]` blocks, phase transitions, idle notes, `busy n/m`, `queue`, `[SKIP]` when N too small.
- Unified vs split: run both, verify kv/len semantics (split per-slot reserved, unified shared occupancy) and loc G/C/R/- behavior.
- Tail: multi-line chat content stays aligned (newline-split), tail lines blank-pad to `t_i`.
- Cache: repeat a prompt prefix, verify `cch` and `hit%` increase.
- Spec: run with `-sp`, verify `spec acc`.
- Memory: verify kv gpu/cpu, weights, cache, rss against a known config; cross-check with `nvidia-smi` and `/proc` when available.
- Impact: run a fixed benchmark with and without `--tui`, confirm engine throughput delta is within noise.

## 13. Risks and notes

- The engine thread publishes under a mutex at <= 10 Hz; if profiling shows measurable impact, reduce publish rate.
- `src/llama-ext.h` is a staging header ("try as much as possible to not include in the rest of the codebase"). Using it from the server is acceptable for a personal fork but should be isolated in one place (server-context), not spread around.
- The router LOG->stderr change is the only stdout behavior change and only affects router mode.
- stdout is a live stream; when `--tui N` is set the server writes N lines/second to stdout, so consumers should pipe to a file or `less`.

## 14. Implementation checklist

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
