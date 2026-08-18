#include "tui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#endif

namespace tui {

static std::string bytes(size_t b) {
    static const char * unit[] = {"B", "kB", "MB", "GB", "TB"};
    double v = (double) b;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    char buf[32];
    if (i == 0) {
        snprintf(buf, sizeof(buf), "%zu", b);
    } else {
        snprintf(buf, sizeof(buf), "%.1f%s", v, unit[i]);
    }
    return buf;
}

static std::string fstr(double v, int prec) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

static std::string fint(long long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return buf;
}

// defined later in this file
std::vector<std::string> format_frame(const global_snap & g, const std::vector<slot_snap> & slots,
                                      const ext_snap & ext, int N);

// ---------------------------------------------------------------------------
// tail text helpers (no width math: lines are newline-delimited segments)
// ---------------------------------------------------------------------------

// strip control chars that could corrupt the terminal (ESC, tabs, CR, ...)
static std::string sanitize(const char * text, int len) {
    std::string out;
    out.reserve((size_t) len);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char) text[i];
        if (c == '\r') {
            if (i + 1 < len && text[i + 1] == '\n') {
                continue; // \r\n -> \n
            }
            out += '\n';
        } else if (c == '\n' || c == '\t') {
            out += c == '\n' ? '\n' : ' ';
        } else if (c < 0x20 || c == 0x7f) {
            continue;
        } else {
            size_t l = 1;
            if      (c >= 0xF0) l = 4;
            else if (c >= 0xE0) l = 3;
            else if (c >= 0xC0) l = 2;
            if (i + (int) l > len) {
                l = (size_t) (len - i);
            }
            out.append(text + i, l);
            i += (int) l - 1;
        }
    }
    return out;
}

static std::vector<std::string> split_lines(const std::string & s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\n') {
            lines.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// external probes (nvidia-smi, /proc) - run on the printer thread, never engine
// ---------------------------------------------------------------------------

#if defined(__linux__)
static bool nvidia_smi_probe(int & sm, int & mem, int & temp, double & pwr, double & pclk, double & mclk) {
    FILE * p = popen("nvidia-smi --query-gpu=utilization.gpu,utilization.memory,temperature.gpu,power.draw,"
                     "clocks.sm,clocks.mem --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) {
        return false;
    }
    char line[512];
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    if (!ok) {
        return false;
    }
    return sscanf(line, "%d,%d,%d,%lf,%lf,%lf", &sm, &mem, &temp, &pwr, &pclk, &mclk) == 6;
}

// pcie counters are not supported on all drivers; tolerate failure
static bool nvidia_smi_pcie_probe(unsigned long long & rx, unsigned long long & tx) {
    FILE * p = popen("nvidia-smi --query-gpu=pcie.rx_bytes,pcie.tx_bytes --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) {
        return false;
    }
    char line[256];
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    if (!ok) {
        return false;
    }
    return sscanf(line, "%llu,%llu", &rx, &tx) == 2;
}

static bool proc_read(uint64_t & io_r, uint64_t & io_w, uint64_t & cpu_ticks) {
    io_r = 0;
    io_w = 0;
    {
        std::ifstream f("/proc/self/io");
        std::string k;
        uint64_t v = 0;
        while (f >> k >> v) {
            if      (k == "read_bytes:")  io_r = v;
            else if (k == "write_bytes:") io_w = v;
        }
    }
    {
        std::ifstream f("/proc/self/stat");
        std::string s;
        if (!std::getline(f, s)) {
            return false;
        }
        const size_t p = s.rfind(')'); // comm may contain spaces/parens
        if (p == std::string::npos) {
            return false;
        }
        std::istringstream iss(s.substr(p + 1));
        uint64_t v = 0;
        int count = 0;
        uint64_t utime = 0;
        uint64_t stime = 0;
        while (iss >> v) {
            count++;
            if      (count == 12) utime = v; // field 14
            else if (count == 13) stime = v; // field 15
            if (count > 13) break;
        }
        cpu_ticks = utime + stime;
    }
    return true;
}
#endif

void controller::read_ext() {
    const int64_t now = (int64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#if defined(__linux__)
    int sm = 0, mem = 0, temp = 0;
    double pwr = 0, pclk = 0, mclk = 0;
    if (nvidia_smi_probe(sm, mem, temp, pwr, pclk, mclk)) {
        ext_.gpu_avail = true;
        ext_.gpu_sm   = sm;
        ext_.gpu_mem  = mem;
        ext_.gpu_temp = temp;
        ext_.gpu_pwr  = pwr;
        ext_.gpu_pclk = pclk;
        ext_.gpu_mclk = mclk;
    } else {
        ext_.gpu_avail = false;
    }

    unsigned long long rx = 0, tx = 0;
    if (nvidia_smi_pcie_probe(rx, tx)) {
        ext_.pcie_avail = true;
        if (pcie_t_prev > 0 && now > pcie_t_prev) {
            const double dt = (now - pcie_t_prev) / 1e6;
            ext_.pcie_rx = dt > 0 ? (double) (rx - pcie_rx_prev) / dt / 1024.0 / 1024.0 : 0.0;
            ext_.pcie_tx = dt > 0 ? (double) (tx - pcie_tx_prev) / dt / 1024.0 / 1024.0 : 0.0;
        } else {
            ext_.pcie_rx = 0;
            ext_.pcie_tx = 0;
        }
        pcie_rx_prev = rx;
        pcie_tx_prev = tx;
        pcie_t_prev  = now;
    } else {
        ext_.pcie_avail = false;
        ext_.pcie_rx    = 0;
        ext_.pcie_tx    = 0;
    }

    uint64_t io_r = 0, io_w = 0, cpu_ticks = 0;
    if (proc_read(io_r, io_w, cpu_ticks)) {
        ext_.proc_avail = true;
        if (proc_t_prev > 0 && now > proc_t_prev) {
            const double dt = (now - proc_t_prev) / 1e6;
            const double clk = (double) sysconf(_SC_CLK_TCK);
            ext_.cpu_pct = dt > 0 ? (double) (cpu_ticks - cpu_ticks_prev) / clk / dt * 100.0 : 0.0;
            ext_.io_r    = dt > 0 ? (double) (io_r - io_r_prev) / dt / 1024.0 / 1024.0 : 0.0;
            ext_.io_w    = dt > 0 ? (double) (io_w - io_w_prev) / dt / 1024.0 / 1024.0 : 0.0;
        } else {
            ext_.cpu_pct = 0;
            ext_.io_r    = 0;
            ext_.io_w    = 0;
        }
        cpu_ticks_prev = cpu_ticks;
        io_r_prev      = io_r;
        io_w_prev      = io_w;
        proc_t_prev    = now;
    } else {
        ext_.proc_avail = false;
    }
#else
    ext_.gpu_avail  = false;
    ext_.proc_avail = false;
#endif
}

// ---------------------------------------------------------------------------
// controller (printer thread)
// ---------------------------------------------------------------------------

controller::controller(snapshot & snap) : snap_(snap) {}

controller::~controller() {
    stop();
}

void controller::start(int n_lines) {
    if (running_) {
        return;
    }
    n_lines_ = n_lines;
    running_ = true;
    thread_  = std::thread([this]() { run(); });
}

void controller::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void controller::run() {
    while (running_) {
        read_ext();

        global_snap g;
        std::vector<slot_snap> slots;
        {
            std::lock_guard<std::mutex> lk(snap_.mtx);
            g = snap_.global;
            const int n = std::min(snap_.n_slots, (int) MAX_SLOTS);
            slots.assign(snap_.slots, snap_.slots + n);
        }

        std::vector<std::string> frame = format_frame(g, slots, ext_, n_lines_);
        for (const auto & ln : frame) {
            fwrite(ln.data(), 1, ln.size(), stdout);
            fputc('\n', stdout);
        }
        fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ---------------------------------------------------------------------------
// frame formatting
// ---------------------------------------------------------------------------

// per-slot tail line allocation (DESIGN.md section 7)
tail_alloc_t tail_alloc(int N, int n_slots) {
    N = std::max(FRAME_MIN, std::min(N, FRAME_MAX));
    tail_alloc_t r;
    r.per_slot.assign((size_t) std::max(0, n_slots), 0);
    const int content = N - 7; // lines for [SEQ] + tails (trailing blank reserved)
    int shown  = std::min(n_slots, content);
    int hidden = n_slots - shown;
    int note   = hidden > 0 ? 1 : 0;
    if (shown + note > content) {
        shown  = std::max(0, content - note);
        hidden = n_slots - shown;
        note   = hidden > 0 ? 1 : 0;
    }
    const int tail_budget = content - shown - note;
    if (shown > 0) {
        const int base = tail_budget / shown;
        const int rem  = tail_budget % shown;
        for (int i = 0; i < shown; i++) {
            r.per_slot[i] = base + (i >= shown - rem ? 1 : 0); // last cells longer
        }
    }
    r.shown  = shown;
    r.hidden = hidden;
    return r;
}

static const char * ph_str(phase p) {
    switch (p) {
        case phase::idle:    return "IDL";
        case phase::prefill: return "PF";
        case phase::decode:  return "DEC";
    }
    return "?";
}

static const char * loc_str(kv_loc loc) {
    switch (loc) {
        case kv_loc::none:      return "-";
        case kv_loc::ram_cache: return "R";
        case kv_loc::kv_cpu:    return "C";
        case kv_loc::kv_gpu:    return "G";
        case kv_loc::kv_mixed:  return "M";
    }
    return "?";
}

static std::string seq_line(const slot_snap & s) {
    const double pct = s.n_ctx > 0 ? (double) s.kv_used / (double) s.n_ctx : 0.0;
    std::string bar;
    const int n_fill = std::min(8, (int) std::ceil(pct * 8.0));
    for (int i = 0; i < 8; i++) {
        bar += i < n_fill ? "#" : ".";
    }
    std::string len = s.occupied ? std::to_string(s.kv_used) + "/" + std::to_string(s.n_ctx) : "-";
    std::string cch = s.n_prompt_cached > 0 ? std::to_string(s.n_prompt_cached) : "-";
    std::string pp  = s.pp_tps > 0.0 ? fstr(s.pp_tps, 1) : "-";
    std::string tg  = s.tg_tps > 0.0 ? fstr(s.tg_tps, 1) : "-";
    std::string q   = s.queue_ms > 0.0 ? fstr(s.queue_ms / 1000.0, 1) : "-";
    std::string dec = s.n_decoded > 0 ? std::to_string(s.n_decoded) : "-";
    std::string rem = s.n_remain >= 0 ? std::to_string(s.n_remain) : "-";
    std::string tid = s.id_task >= 0 ? std::to_string(s.id_task) : "-";

    return "[SEQ " + std::to_string(s.id) + "] " + ph_str(s.ph) + " " + loc_str(s.loc)
         + " kv:" + bar + " len:" + len + " cch:" + cch
         + " pp:" + pp + " tg:" + tg + " q:" + q + " dec:" + dec + " rem:" + rem + " t:" + tid;
}

static std::string note_for(const slot_snap & s) {
    if (s.ph == phase::idle) {
        if (s.occupied) {
            return "(idle " + std::to_string(s.idle_age_s) + "s, KV resident)";
        }
        if (s.loc == kv_loc::ram_cache) {
            return "(RAM-cached)";
        }
        return "(idle)";
    }
    return s.ph == phase::prefill ? "(prefill, no text yet)" : "(generating)";
}

std::vector<std::string> format_frame(const global_snap & g, const std::vector<slot_snap> & slots,
                                      const ext_snap & ext, int N) {
    N = std::max(FRAME_MIN, std::min(N, FRAME_MAX));
    std::vector<std::string> L;

    // 6 fixed tagged lines
    {
        std::string ctx = fstr((double) g.n_ctx, 0) + "/" + fstr((double) g.n_ctx_train, 0);
        L.push_back("[SERVER] " + std::string(g.model_desc) + " (" + std::string(g.alias) + ")  ctx " + ctx
                  + "  " + (g.kv_unified ? "unified" : "split") + "  KV --  FA " + std::string(g.flash_attn)
                  + "  up " + fstr((double) g.uptime_s / 3600.0, 1) + "h");
    }
    L.push_back("[RUN] busy " + fint(g.busy) + "/" + fint(g.n_slots)
              + "  queue " + fint(g.deferred) + "  engine " + fint((long long) (g.engine_busy * 100.0 + 0.5)) + "%");
    {
        size_t used_cells = 0;
        size_t resv_cells = 0;
        for (const auto & s : slots) {
            used_cells += (size_t) std::max(0, s.kv_used);
            if (g.kv_unified) {
                resv_cells = g.n_ctx;
            } else {
                resv_cells += (size_t) std::max(0, s.n_ctx);
            }
        }
        L.push_back("[MEMORY] kv gpu " + bytes(g.kv_gpu) + "  kv cpu " + bytes(g.kv_cpu)
                  + "  weights " + bytes(g.weights_gpu) + "  cache " + bytes(g.ram_cache)
                  + "  rss " + bytes(g.rss) + "  cells " + fint(used_cells) + "/" + fint(resv_cells));
    }
    L.push_back("[THROUGHPUT] prompt " + fstr(g.prompt_tps, 0) + "/s  gen " + fstr(g.gen_tps, 0) + "/s"
              + "  spec " + (g.spec_acc >= 0 ? fstr(g.spec_acc * 100.0, 0) + "%" : "-")
              + "  hit "  + (g.hit_rate >= 0 ? fstr(g.hit_rate * 100.0, 0) + "%" : "-")
              + "  pp " + fstr(g.pp_ms_tok, 0) + "ms  tg " + fstr(g.tg_ms_tok, 0) + "ms  ftok "
              + (g.first_tok_s > 0 ? fstr(g.first_tok_s, 2) + "s" : "-"));
    if (ext.gpu_avail) {
        std::string pcie = ext.pcie_avail ? fstr(ext.pcie_rx, 0) + "M/s" : "-";
        L.push_back("[GPU] SM " + fint(ext.gpu_sm) + "%  mem " + fint(ext.gpu_mem) + "%  pwr " + fstr(ext.gpu_pwr, 0)
                  + "W  t " + fint(ext.gpu_temp) + "C  pclk " + fstr(ext.gpu_pclk / 1000.0, 1) + "G  mclk "
                  + fstr(ext.gpu_mclk / 1000.0, 1) + "G  pcie " + pcie);
    } else {
        L.push_back("[GPU] SM -  mem -  pwr -  t -  pclk -  mclk -  pcie -");
    }
    {
        std::string cpu = ext.proc_avail ? fstr(ext.cpu_pct, 0) + "%" : "-";
        std::string ior = ext.proc_avail ? fstr(ext.io_r, 0) + "M/s" : "-";
        std::string iow = ext.proc_avail ? fstr(ext.io_w, 0) + "M/s" : "-";
        std::string req = g.req_valid
                        ? ("q " + fstr(g.req_q_s, 1) + "s pp " + fstr(g.req_pp_s, 1) + "s dec " + fstr(g.req_gen_s, 1) + "s")
                        : "phases -";
        L.push_back("[SYSTEM] cpu " + cpu + "  ioR " + ior + "  ioW " + iow + "  req " + req);
    }

    // per-sequence blocks (allocation computed on the engine thread)
    const int n     = (int) slots.size();
    const int shown = std::min(g.n_shown, n);
    const int hidden = n - shown;
    for (int i = 0; i < shown; i++) {
        const slot_snap & s = slots[i];
        L.push_back(seq_line(s));

        const int t = std::max(0, s.tail_lines);
        std::vector<std::string> tl;
        if (s.tail_len > 0) {
            tl = split_lines(sanitize(s.tail, s.tail_len));
        }
        if (tl.empty()) {
            tl.push_back(note_for(s));
        }
        // the last t lines; blank-pad to keep the frame exactly N lines
        const int start = std::max(0, (int) tl.size() - t);
        for (int k = start; k < start + t; k++) {
            L.push_back(k < (int) tl.size() ? tl[k] : std::string());
        }
    }
    if (hidden > 0) {
        L.push_back("[SKIP] +" + fint(hidden) + " more hidden");
    }

    L.push_back(std::string()); // trailing blank: frame separator

    // safety: exactly N lines
    while ((int) L.size() < N) {
        L.push_back(std::string());
    }
    if ((int) L.size() > N) {
        L.resize(N);
    }
    return L;
}

} // namespace tui
