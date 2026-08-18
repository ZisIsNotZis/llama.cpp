#pragma once

// TUI dashboard for llama-server.
// Plain-text timely-output program: prints one tagged frame per second to
// stdout. No ncurses, no terminal control, no width/height math.
// See docs/dashboard/DESIGN.md and docs/dashboard/IMPLEMENTATION.md.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct server_context_impl;

namespace tui {

constexpr int MAX_SLOTS       = 256;  // max parallel slots we handle
constexpr int TAIL_CHARS      = 8192; // max chars of tail kept per slot (cap)
constexpr int TAIL_TOKENS_MIN = 64;   // min tail tokens detokenized per slot
constexpr int TAIL_TOKENS_MAX = 1024; // max tail tokens detokenized per slot
constexpr int FRAME_MIN       = 9;    // min frame lines (fixed + blank + 1)
constexpr int FRAME_MAX       = 200;  // max frame lines
constexpr int FRAME_FIXED     = 8;    // fixed lines: 6 tags + [TOTAL] + blank

enum class phase  { idle, prefill, decode };
enum class kv_loc { none, ram_cache, kv_cpu, kv_gpu, kv_mixed };

struct slot_snap {
    int       id           = -1;
    phase     ph           = phase::idle;
    kv_loc    loc          = kv_loc::none;
    bool      occupied     = false;
    int32_t   n_ctx        = 0;   // reserved cells (split) or shared cap (unified)
    int32_t   kv_used      = 0;   // used cells (seq_pos_max + 1)
    int32_t   n_prompt     = 0;
    int32_t   n_prompt_proc = 0;
    int32_t   n_prompt_cached = 0;
    int32_t   n_decoded    = 0;
    int32_t   n_remain     = -1;
    int64_t   t_last_used  = 0;
    int32_t   idle_age_s   = -1;
    double    queue_ms     = 0.0; // queue wait for the current task
    double    pp_tps       = 0.0;
    double    tg_tps       = 0.0;
    double    t_prompt_ms  = 0.0;
    double    t_gen_ms     = 0.0;
    int32_t   id_task      = -1;
    int       tail_lines   = 0;   // tail lines to print for this slot (engine-computed)
    double    pp5_tps      = 0.0; // sliding-window (~5 s) prompt speed
    double    tg5_tps      = 0.0; // sliding-window (~5 s) generation speed
    char      tail[TAIL_CHARS];
    int       tail_len     = 0;
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
    char    kv_type[32];   // "f16/f16", "--" if n/a
    bool    speculative = false;
    bool    sleeping    = false;
    int     busy        = 0;
    int     deferred    = 0;
    double  engine_busy = 0.0;
    double  prompt_tps  = 0.0;
    double  gen_tps     = 0.0;
    double  spec_acc    = -1.0;
    double  hit_rate    = -1.0;
    double  pp_ms_tok   = 0.0;
    double  tg_ms_tok   = 0.0;
    double  first_tok_s = 0.0;
    bool    req_valid   = false;
    double  req_q_s     = 0.0;
    double  req_pp_s    = 0.0;
    double  req_gen_s   = 0.0;
    uint64_t total_requests = 0;
    double  req_per_s   = 0.0;
    size_t  kv_gpu      = 0;
    size_t  kv_cpu      = 0;
    size_t  weights_gpu = 0;
    size_t  weights_cpu = 0;
    size_t  compute_gpu = 0;
    size_t  compute_cpu = 0;
    size_t  ram_cache   = 0;
    size_t  ram_cache_max = 0;
    size_t  rss         = 0;
    int64_t uptime_s    = 0;
    // cumulative lifetime counters (since server start)
    uint64_t total_prompt   = 0;
    uint64_t total_gen      = 0;
    uint64_t total_cached   = 0;
    uint64_t total_decode   = 0;
    // frame layout (engine-computed from --tui N)
    int     frame_n     = 0;   // clamped N
    int     n_shown     = 0;   // slots that get a [SEQ] line
};

// external probes read on the printer thread (nvidia-smi, /proc)
struct ext_snap {
    bool    gpu_avail   = false;
    int     gpu_count   = 0;
    int     gpu_sm      = 0;
    int     gpu_mem     = 0;
    int     gpu_temp    = 0;
    double  gpu_pwr     = 0;
    double  gpu_pclk    = 0;
    double  gpu_mclk    = 0;
    bool    pcie_avail  = false;
    double  pcie_rx     = 0;
    double  pcie_tx     = 0;
    bool    proc_avail  = false;
    double  cpu_pct     = 0;
    double  io_r        = 0;
    double  io_w        = 0;
};

struct snapshot {
    std::mutex  mtx;
    uint64_t    seq     = 0;
    global_snap global;
    slot_snap   slots[MAX_SLOTS];
    int         n_slots = 0;
};

// per-slot tail line allocation (matches DESIGN.md section 7)
struct tail_alloc_t {
    std::vector<int> per_slot; // tail lines per slot (hidden slots get 0)
    int shown  = 0;
    int hidden = 0;
};
tail_alloc_t tail_alloc(int N, int n_slots);

// Fills the snapshot. Called from the engine thread (throttled externally).
// Implemented in server-context.cpp where the impl internals are visible.
void fill_snapshot(snapshot & snap, server_context_impl & ctx);

// Owns the printer thread. No input handling; Ctrl-C keeps killing the server.
class controller {
public:
    explicit controller(snapshot & snap);
    ~controller();

    // spawn the printer thread; no-op if already running
    void start(int n_lines);
    // stop the thread; idempotent
    void stop();
    // true while the printer thread is running
    bool is_active() const { return running_; }

private:
    void run();
    void read_ext();

    snapshot &        snap_;
    std::thread       thread_;
    std::atomic<bool> running_ {false};
    int               n_lines_ = 0;

    // external probe data + previous sample state (rate deltas)
    ext_snap  ext_;
    uint64_t  pcie_rx_prev = 0;
    uint64_t  pcie_tx_prev = 0;
    int64_t   pcie_t_prev  = 0;
    uint64_t  io_r_prev    = 0;
    uint64_t  io_w_prev    = 0;
    uint64_t  cpu_ticks_prev = 0;
    int64_t   proc_t_prev  = 0;
};

} // namespace tui
