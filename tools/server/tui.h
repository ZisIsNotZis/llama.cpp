#pragma once

// TUI dashboard for llama-server.
// See docs/dashboard/DESIGN.md and docs/dashboard/IMPLEMENTATION.md (source of truth).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

struct server_context_impl;

namespace tui {

constexpr int MAX_SLOTS   = 256;  // max parallel slots we render
constexpr int TAIL_TOKENS = 256;  // tokens copied per slot for the tail
constexpr int TAIL_CHARS  = 4096; // max chars of tail kept per slot

enum class phase  { idle, prefill, decode };
enum class kv_loc { none, ram_cache, kv_cpu, kv_gpu, kv_mixed };

struct slot_snap {
    int      id         = -1;
    phase    ph         = phase::idle;
    kv_loc   loc        = kv_loc::none;
    bool     occupied   = false;
    int32_t  n_ctx      = 0;   // reserved cells (split) or shared cap (unified)
    int32_t  kv_used    = 0;   // used cells (seq_pos_max + 1)
    int32_t  n_prompt   = 0;   // total prompt tokens of the task
    int32_t  n_prompt_proc = 0;
    int32_t  n_prompt_cached = 0;
    int32_t  n_decoded  = 0;
    int32_t  n_remain   = -1;
    int64_t  t_last_used = 0;  // us since epoch, 0 if never
    int32_t  idle_age_s  = -1; // seconds since last use, -1 if n/a
    double   queue_ms    = 0.0; // queue wait for the current task
    double   pp_tps     = 0.0;
    double   tg_tps     = 0.0;
    double   t_prompt_ms = 0.0;
    double   t_gen_ms   = 0.0;
    int32_t  id_task    = -1;
    char     tail[TAIL_CHARS];
    int      tail_len   = 0;
};

// external probes read on the TUI thread (nvidia-smi, /proc)
struct ext_snap {
    // GPU (nvidia-smi)
    bool   gpu_avail = false;
    int    gpu_sm    = 0; // %
    int    gpu_mem   = 0; // % memory utilization
    int    gpu_temp  = 0; // C
    double gpu_pwr   = 0; // W
    double gpu_pclk  = 0; // MHz
    double gpu_mclk  = 0; // MHz
    double pcie_rx   = 0; // MB/s
    double pcie_tx   = 0; // MB/s
    bool   pcie_avail = false;
    // system (/proc, Linux)
    bool   proc_avail = false;
    double cpu_pct    = 0; // process CPU %
    double io_r       = 0; // MB/s
    double io_w       = 0; // MB/s
};

struct global_snap {
    char    model_desc[256];
    char    alias[128];
    uint32_t n_ctx      = 0;
    uint32_t n_ctx_seq  = 0;
    uint32_t n_ctx_train = 0;
    uint32_t n_slots    = 0;
    bool    kv_unified  = false;
    char    flash_attn[16];
    bool    speculative = false;
    bool    sleeping    = false;
    int     busy        = 0;
    int     idle        = 0;
    int     deferred    = 0;
    double  engine_busy = 0.0; // 0..1 rolling window
    double  prompt_tps  = 0.0;
    double  gen_tps     = 0.0;
    double  spec_acc    = -1.0; // -1 = n/a
    double  hit_rate    = -1.0; // -1 = n/a
    double  pp_ms_tok   = 0.0;
    double  tg_ms_tok   = 0.0;
    double  first_tok_s = 0.0;
    // request lifecycle phases of the most recently launched active task
    bool    req_valid = false;
    double  req_q_s   = 0.0;  // queue wait
    double  req_pp_s  = 0.0;  // prefill time
    double  req_gen_s = 0.0;  // decode time
    size_t  kv_gpu      = 0; // context memory on non-host backends
    size_t  kv_cpu      = 0; // context memory on host backend
    size_t  weights_gpu = 0;
    size_t  weights_cpu = 0;
    size_t  compute_gpu = 0;
    size_t  compute_cpu = 0;
    size_t  ram_cache   = 0;
    size_t  ram_cache_max = 0;
    size_t  rss         = 0; // 0 = unavailable
    int64_t uptime_s    = 0;
    // placeholders until nvidia-smi / proc integration (DESIGN.md section 9)
    int     gpu_sm      = 0;
    int     gpu_mem     = 0;
    int     gpu_temp    = 0;
};

struct snapshot {
    std::mutex  mtx;
    uint64_t    seq     = 0;
    global_snap global;
    slot_snap   slots[MAX_SLOTS];
    int         n_slots = 0;
};

// Fills the snapshot. Called from the engine thread (throttled externally).
// Implemented in server-context.cpp where the impl internals are visible.
void fill_snapshot(snapshot & snap, server_context_impl & ctx);

// Owns the TUI thread and terminal. No input handling; Ctrl-C keeps killing the server.
class controller {
public:
    explicit controller(snapshot & snap);
    ~controller();

    // spawn the TUI thread; no-op if stdout is not a tty or already running
    void start();
    // stop the thread and restore the terminal; idempotent
    void stop();
    // true while the TUI thread is running (stdout is a tty)
    bool is_active() const { return running_; }

private:
    void run();

    snapshot &        snap_;
    std::thread       thread_;
    std::atomic<bool> running_ {false};

    // external probe data + previous sample state (rate deltas)
    ext_snap  ext_;
    uint64_t  pcie_rx_prev = 0;
    uint64_t  pcie_tx_prev = 0;
    int64_t   pcie_t_prev  = 0;
    uint64_t  io_r_prev    = 0;
    uint64_t  io_w_prev    = 0;
    uint64_t  cpu_ticks_prev = 0;
    int64_t   proc_t_prev  = 0;

    void read_ext();
};

} // namespace tui
