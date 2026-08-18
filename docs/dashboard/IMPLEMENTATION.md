# llama-server TUI - Implementation

Status: APPROVED BASELINE

Date: 2026-02-14

This document is the source of truth for HOW to implement the TUI per DESIGN.md. Any divergence from DESIGN.md requires approval and a doc update first.

## 1. Scope and relation to DESIGN

- Implements the full final layout from DESIGN.md section 6.
- Fields without a data source render `-` (DESIGN.md section 9).
- All locked semantics in DESIGN.md section 8.3 are authoritative.

## 2. Architecture overview

- One dedicated TUI thread owns stdout and the terminal.
- The engine thread (server_context, single thread) publishes a bounded snapshot under a mutex, throttled to <= 10 Hz.
- The TUI thread reads the latest snapshot at 1 Hz, re-queries terminal size, lays out, renders a full frame, and flushes.
- The TUI thread never calls llama APIs directly. All llama-derived values (including the tail string) are pre-computed in the snapshot on the engine thread. This removes any cross-thread llama API question entirely.
- No stdin handling, no raw mode, no keys. Ctrl-C goes through the existing signal path and kills the server; shutdown restores the terminal.

```
engine thread (server_context)                TUI thread
  update_slots()  -- every iteration -->
    if (now - last_publish >= 100ms)
      fill_snapshot(&snap, ctx_server)  (mutex)
                                            every 1000ms:
                                              ioctl(TIOCGWINSZ)  -> W,H
                                              lock, copy snap
                                              layout(W,H) + render
                                              flush stdout
  shutdown: set snap.stop=true (atomic), join thread, restore terminal
```

## 3. Files

New:

- `tools/server/tui.h` - snapshot types, TUI lifecycle API, renderer entry points.
- `tools/server/tui.cpp` - snapshot fill, TUI thread loop, layout + ANSI renderer.

Modified:

- `tools/server/server-context.cpp/.h` - publish snapshot after `update_slots()` (server-context.cpp:2699); add engine-busy timing around it; expose `llama_get_memory_breakdown()` data, prompt-cache size, and request lifecycle phases.
- `tools/server/server-task.h` - add `t_arrival_us` to `server_task`.
- `tools/server/server-queue.cpp` - stamp `t_arrival_us` in `post()` (queue-wait baseline).
- `tools/server/server.cpp` - `--tui` wiring; start/stop TUI thread around `ctx_server.start_loop()`; terminal restore in `clean_up()` path.
- `tools/server/server-common.h` - server params field `tui`.
- `common/arg.cpp`, `common/common.h` - new `--tui` / `--no-tui` flag and env `LLAMA_ARG_TUI`.
- `tools/server/server-models.cpp:1028` - move the router `LOG(...)` line to stderr so stdout has no other writer.
- `tools/server/CMakeLists.txt` - add `tui.cpp`.

## 4. CLI flag

- `--tui` / `--no-tui` (default auto): enabled when `isatty(fileno(stdout))` and not disabled. Router mode: TUI is not applicable (no model context), flag is ignored.
- Env: `LLAMA_ARG_TUI` (true/false), consistent with other boolean flags (see `common/arg.cpp` and README line 307).

## 5. Logging stream changes

- Today all llama-server logs go to stderr (verified: `common_log_entry::print()` in `common/log.cpp:89-93` sends level != NONE to stderr; INFO/WARN/ERROR/DEBUG all qualify).
- The only stdout writer is the router-mode `LOG("[%5d] %s", ...)` in `server-models.cpp:1028` (LEVEL_NONE -> stdout). Move it to stderr.
- Enforce: when TUI is inactive (not a TTY), stdout must remain empty to preserve current behavior for scripts/pipes/systemd.

## 6. Snapshot

Fixed-capacity structs preallocated once (no per-tick allocation in the engine thread). Token/char bounds are constants.

```cpp
// tools/server/tui.h
namespace tui {
constexpr int MAX_SLOTS   = 256;
constexpr int TAIL_TOKENS = 256;   // tokens copied per slot for tail
constexpr int TAIL_CHARS  = 4096;  // max rendered chars per tail

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
    double    pp_tps;
    double    tg_tps;
    double    t_prompt_ms;    // pp time (first-token proxy)
    double    t_gen_ms;
    int32_t   id_task;
    char      tail[TAIL_CHARS];
    bool      tail_valid;
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
    bool         stop;      // TUI shutdown flag (atomic-ish under mtx)
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

- Init (when enabled + TTY):
  - enter alternate screen `ESC [ ? 1049 h`
  - hide cursor `ESC [ ? 25 l`
  - `ESC [ 2 J` clear
- Per tick: query size via `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` -> W,H. Resize is handled by re-querying every tick (correct by design; SIGWINCH immediate redraw is backlog).
- Render: build full frame into a buffer, move cursor home `ESC [ H`, write buffer, `fflush(stdout)`.
- Shutdown (normal exit path / after `start_loop()` returns):
  - set stop flag, join TUI thread (detached fallback on forced paths), then emit `ESC [ ? 25 h` + `ESC [ ? 1049 l`.
- The signal handler (`signal_handler` in server.cpp) is unchanged: it calls `shutdown_handler`, which triggers `ctx_server.terminate()` -> `start_loop()` returns -> normal `clean_up()` path. Terminal restore lives in that path, so no signal-unsafe work is needed in the handler.

## 9. Rendering and layout

### 9.1 Layout algorithm (per tick, given W,H and snapshot)

1. Fixed band: title(1) + status(1) + MEMORY/THROUGHPUT(3) + GPU/SYSTEM(2) = 7 rows. If W < 100, stack panels full-width (total rows grow accordingly; leftover shrinks the SEQUENCES box).
2. SEQUENCES rows R = H - band - 2 (borders).
3. Header row: fixed-width columns (see 9.3).
4. Active slots first: each = summary line + tail box of T rows where T = max(1, (R - 1 - n_idle) / n_active), capped (e.g. 24). All active slots share the space evenly; a single active slot gets nearly the whole box.
5. Idle slots: one compact line each; if they exceed remaining rows, print as many as fit and append `+N idle`.

### 9.2 Tail window

- Snapshot holds the last `TAIL_CHARS` chars (pre-detokenized on the engine thread).
- The box renders the most recent `T x (W-4)` characters, word-wrapped. If the text is shorter, pad with blanks. If longer, drop older lines (rolling window), prefixing the first shown line with `...` when truncated.

### 9.3 Per-slot summary columns (fixed widths)

`#  ph  kv(bar)  loc  len      cch   pp/t   tg/t` then tail text (or idle note) filling the rest. Summary line width is fixed so the tail column start is stable; the tail is clipped to available width.

### 9.4 Tail detokenization cost

- On publish (<= 10 Hz) copy last `TAIL_TOKENS` tokens of `generated_tokens` (and `prompt.tokens` during prefill) into a scratch buffer, detokenize once, keep last `TAIL_CHARS` chars.
- A suffix detokenize may start mid-UTF-8; acceptable for a preview (renderer drops a leading partial code point).
- This runs on the engine thread but is bounded (256 tokens per active slot, 10 Hz). If profiling shows impact, move to a lower publish rate.

### 9.5 RAM-cache-only detection

- When `--cache-idle-slots` + unified mode clear an idle slot, `prompt_clear()` empties its prompt and KV. The cleared slot has no KV cells and no prompt, so it shows loc `R` (RAM-cached) only if the server can attribute a cache entry to it.
- v1: if slot is idle, KV empty, and a matching entry exists in `server_prompt_cache`, show `R`; otherwise `-`. Exact attribution (cache entry -> slot) may need a small addition to `server_prompt_cache` (store owning slot id); that is optional/backlog. Simpler v1 fallback: show global `cache` bytes in MEMORY and per-slot `-` for cleared slots.

### 9.6 Units and colors

- Bytes: humanized (B, kB, MB, GB); rates: tokens/s, MB/s; latency: ms or s with one decimal.
- Palette (8-color): title accent, IDL dim, PF yellow, DEC green, KV bar threshold (green <70%, yellow <90%, red >=90%), loc letters G green / C yellow / G/C magenta / R blue / - dim, tail dim, placeholders `-` dim.
- Values that are not applicable (e.g. tg during prefill) render `-`, not 0.

## 10. Concurrency

- Snapshot guarded by `std::mutex`; publish and read both take the lock briefly. Publish copies fixed structs only; no allocation (buffers preallocated, tail scratch reused).
- TUI thread touches stdout only; engine thread never writes to stdout.
- Engine-busy instrumentation: two atomics or a small mutex-protected running sum updated in `update_slots()` entry/exit; snapshot reads it. Rolling window (e.g. 5 s) is computed from cumulative counters.
- No llama API call happens on the TUI thread (all derived values precomputed in the snapshot). This keeps thread-safety trivially correct.

## 11. Build integration

- Add `tui.cpp` to the `server-context` target in `tools/server/CMakeLists.txt` (it needs access to `server_slot`, `server_metrics`, `prompt_cache`, and `llama_ext`).
- Include `src/llama-ext.h` (already reachable via the target's `${CMAKE_SOURCE_DIR}` include dir; common/ already uses it) to get `llama_get_memory_breakdown`.
- No new link dependencies; std::thread already used.

## 12. Testing / validation

- Non-TTY: run with stdout to a file -> stdout stays empty, TUI absent, behavior unchanged.
- TTY: run with `--tui`, verify full frame, 1 Hz updates, no flicker, terminal restored on Ctrl-C.
- Multi-slot: send concurrent completions (2-8 slots), verify phase transitions, tail windows, idle collapse, `busy n/m`, `queue`.
- Unified vs split: run both, verify kv/len semantics (split per-slot reserved, unified shared occupancy) and loc R vs G behavior.
- Resize: shrink/grow terminal, verify re-layout on next tick.
- Cache: repeat a prompt prefix, verify `cch` and `hit%` increase.
- Spec: run with `-sp`, verify `spec acc`.
- Memory: verify kv gpu/cpu, weights, cache, rss against a known config; cross-check with `nvidia-smi` and `/proc` when available.
- Impact: run a fixed benchmark with and without `--tui`, confirm engine throughput delta is within noise.

## 13. Risks and notes

- The engine thread publishes under a mutex at <= 10 Hz; if profiling shows measurable impact, reduce publish rate or only publish while TUI is active and enabled.
- `src/llama-ext.h` is a staging header ("try as much as possible to not include in the rest of the codebase"). Using it from the server is acceptable for a personal fork but should be isolated in one place (tui.cpp / server-context), not spread around.
- The router LOG->stderr change is the only stdout behavior change and only affects router mode.
- Terminal restore depends on the normal shutdown path; second-Ctrl-C forced exit (server.cpp: signal_handler) will not restore the terminal (acceptable, it is a force-kill path).

## 14. Implementation checklist

1. Flag `--tui` in common_params + arg + server wiring.
2. Move router LOG to stderr; verify stdout empty when TUI off.
3. Snapshot types + publish hook in `update_slots()` (throttled) + engine-busy timing.
4. TUI thread lifecycle + terminal init/restore + size query + 1 Hz loop.
5. Renderer: fixed band, SEQUENCES layout, tail window, units/colors, placeholders.
6. Fill all S1 fields; leave N/B as placeholders.
7. CMake + build.
8. Manual validation (section 12).

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline | owner |
| 2026-02-14 | Initial implementation: snapshot struct, publish hook, controller, renderer, `--tui` flag, router LOG->stderr, `tui.cpp`/`tui.h`. S1 fields implemented; N/B placeholders. | owner |
| 2026-02-14 | Tier N (partial): GPU panel via `nvidia-smi` (SM/mem/temp/pwr/clocks, pcie fallback) and SYSTEM panel via `/proc` (process CPU%, disk IO rates), read on the TUI thread into `ext_snap`. | owner |
| 2026-02-14 | Tier N complete: request lifecycle phases (`req q Xs pp Xs dec Xs`) via `server_task.t_arrival_us` stamped in `server_queue::post()`. | owner |
