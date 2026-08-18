#include "tui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace tui {

// ANSI SGR
static constexpr const char * RESET   = "\033[0m";
static constexpr const char * DIM     = "\033[2m";
static constexpr const char * RED     = "\033[31m";
static constexpr const char * GREEN   = "\033[32m";
static constexpr const char * YELLOW  = "\033[33m";
static constexpr const char * BLUE    = "\033[34m";
static constexpr const char * MAGENTA = "\033[35m";
static constexpr const char * CYAN    = "\033[36m";

//
// small utf-8 aware width helpers.
// every code point counts as 1 column (CJK/wide glyphs undercount; acceptable v1).
//

static size_t u8len(const char * s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char b = (unsigned char) s[i];
        size_t l = 1;
        if      (b >= 0xF0) l = 4;
        else if (b >= 0xE0) l = 3;
        else if (b >= 0xC0) l = 2;
        if (i + l > n) l = n - i;
        i += l;
        c++;
    }
    return c;
}

// truncate to at most `cols` columns at a utf-8 boundary
static std::string clip(const std::string & s, size_t cols) {
    size_t c = 0;
    size_t i = 0;
    while (i < s.size() && c < cols) {
        unsigned char b = (unsigned char) s[i];
        size_t l = 1;
        if      (b >= 0xF0) l = 4;
        else if (b >= 0xE0) l = 3;
        else if (b >= 0xC0) l = 2;
        if (i + l > s.size()) l = s.size() - i;
        i += l;
        c++;
    }
    return s.substr(0, i);
}

static void pad_cols(std::string & s, size_t cols) {
    size_t n = u8len(s.data(), s.size());
    if (n < cols) {
        s.append(cols - n, ' ');
    } else if (n > cols) {
        s = clip(s, cols);
    }
}

static std::string rep(const char * s, size_t n) {
    std::string out;
    out.reserve(n * std::strlen(s));
    for (size_t i = 0; i < n; i++) {
        out += s;
    }
    return out;
}

// display columns, skipping ANSI escape sequences
static size_t vlen(const std::string & s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != 'm') {
                j++;
            }
            i = j + 1;
            continue;
        }
        unsigned char b = (unsigned char) s[i];
        size_t l = 1;
        if      (b >= 0xF0) l = 4;
        else if (b >= 0xE0) l = 3;
        else if (b >= 0xC0) l = 2;
        if (i + l > s.size()) l = s.size() - i;
        i += l;
        n++;
    }
    return n;
}

// pad a possibly-ANSI string to exactly `cols` display columns (never truncates)
static void pad_visual(std::string & s, size_t cols) {
    size_t n = vlen(s);
    if (n < cols) {
        s.append(cols - n, ' ');
    }
}

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

static std::string fmt(double v, int prec) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

// defined later in this file
std::string render(const global_snap & g, const std::vector<slot_snap> & slots, const ext_snap & ext, int W, int H);

// ---------------------------------------------------------------------------
// external probes (nvidia-smi, /proc) - run on the TUI thread, never the engine
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
// controller
// ---------------------------------------------------------------------------

controller::controller(snapshot & snap) : snap_(snap) {}

controller::~controller() {
    stop();
}

void controller::start() {
    if (running_) {
        return;
    }
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    if (!isatty(fileno(stdout))) {
        return; // not a terminal, keep stdout empty
    }
#else
    return; // TUI only supported on unix-like for now
#endif
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
    // restore terminal (idempotent)
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}

void controller::run() {
    // enter alternate screen, hide cursor
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);

    while (running_) {
        read_ext();
        int W = 80;
        int H = 24;
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
        struct winsize ws;
        if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            W = (int) ws.ws_col;
            H = (int) ws.ws_row;
        }
#endif
        global_snap g;
        std::vector<slot_snap> slots;
        {
            std::lock_guard<std::mutex> lk(snap_.mtx);
            g = snap_.global;
            const int n = std::min(snap_.n_slots, (int) MAX_SLOTS);
            slots.assign(snap_.slots, snap_.slots + n);
        }

        std::string frame = render(g, slots, ext_, W, H);

        fputs("\033[H", stdout);
        fputs(frame.c_str(), stdout);
        fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ---------------------------------------------------------------------------
// renderer
// ---------------------------------------------------------------------------

namespace {

struct render_ctx {
    int W;
    int H;
};

static const char * ph_str(phase p) {
    switch (p) {
        case phase::idle:    return "IDL";
        case phase::prefill: return "PF ";
        case phase::decode:  return "DEC";
    }
    return "?";
}

static const char * ph_color(phase p) {
    switch (p) {
        case phase::idle:    return DIM;
        case phase::prefill: return YELLOW;
        case phase::decode:  return GREEN;
    }
    return RESET;
}

static const char * loc_str(kv_loc loc) {
    switch (loc) {
        case kv_loc::none:     return "-";
        case kv_loc::ram_cache: return "R";
        case kv_loc::kv_cpu:   return "C";
        case kv_loc::kv_gpu:   return "G";
        case kv_loc::kv_mixed: return "M";
    }
    return "?";
}

static const char * loc_color(kv_loc loc) {
    switch (loc) {
        case kv_loc::none:      return DIM;
        case kv_loc::ram_cache: return BLUE;
        case kv_loc::kv_cpu:    return YELLOW;
        case kv_loc::kv_gpu:    return GREEN;
        case kv_loc::kv_mixed:  return MAGENTA;
    }
    return RESET;
}

static const char * bar_color(double pct) {
    if (pct >= 0.90) return RED;
    if (pct >= 0.70) return YELLOW;
    return GREEN;
}

// summary line for one slot, without the tail (ASCII part)
static std::string slot_summary(const slot_snap & s) {
    char buf[128];

    // kv bar
    double pct = s.n_ctx > 0 ? (double) s.kv_used / (double) s.n_ctx : 0.0;
    const int bar_w = 8;
    const int n_fill = std::min(bar_w, (int) std::ceil(pct * bar_w));
    std::string bar;
    for (int i = 0; i < bar_w; i++) {
        bar += i < n_fill ? "\xe2\x96\x88" : "\xe2\x96\x91"; // BLOCK, LIGHT SHADE
    }

    // len used/ctx
    std::string len = "-";
    if (s.occupied) {
        snprintf(buf, sizeof(buf), "%d/%d", s.kv_used, s.n_ctx);
        len = clip(buf, 10);
    }

    // cache hits
    std::string cch = s.n_prompt_cached > 0 ? std::to_string(s.n_prompt_cached) : "-";

    // speeds
    std::string pp = s.pp_tps > 0.0 ? fmt(s.pp_tps, 1) : "-";
    std::string tg = s.tg_tps > 0.0 ? fmt(s.tg_tps, 1) : "-";

    snprintf(buf, sizeof(buf), "%2d  ", s.id);
    std::string out = buf;

    out += ph_color(s.ph);
    out += ph_str(s.ph);
    out += RESET;
    out += " ";

    out += bar_color(pct);
    out += bar;
    out += RESET;
    out += " ";

    out += loc_color(s.loc);
    out += loc_str(s.loc);
    out += RESET;
    out += " ";

    snprintf(buf, sizeof(buf), "%-10s  %-5s  %7s  %7s  ", len.c_str(), cch.c_str(), pp.c_str(), tg.c_str());
    out += buf;
    return out;
}

// wrap text into lines of `cols` columns; keep the last `max_lines` lines
static std::vector<std::string> wrap_tail(const char * text, int len, size_t cols, size_t max_lines) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < (size_t) len) {
        size_t j = i;
        size_t c = 0;
        while (j < (size_t) len && c < cols) {
            unsigned char b = (unsigned char) text[j];
            size_t l = 1;
            if      (b >= 0xF0) l = 4;
            else if (b >= 0xE0) l = 3;
            else if (b >= 0xC0) l = 2;
            if (j + l > (size_t) len) l = (size_t) len - j;
            j += l;
            c++;
        }
        lines.emplace_back(text + i, j - i);
        i = j;
    }
    if (lines.size() > max_lines) {
        lines.erase(lines.begin(), lines.end() - max_lines);
        lines[0] = "..." + lines[0];
    }
    return lines;
}

} // namespace

std::string render(const global_snap & g, const std::vector<slot_snap> & slots, const ext_snap & ext, int W_in, int H_in) {
    const int W = std::max(24, std::min(W_in, 400));
    const int H = std::max(8, std::min(H_in, 500));

    std::string out;

    // ---- title row -------------------------------------------------------
    std::string ctx = fmt((double) g.n_ctx, 0) + "/" + fmt((double) g.n_ctx_train, 0);
    std::string kvmode = g.kv_unified ? "unified" : "split";
    std::string title = "llama-server  " + std::string(g.model_desc) + "  (" + std::string(g.alias) + ")"
                      + "  ctx " + ctx + "  " + kvmode + "  KV --  FA " + std::string(g.flash_attn)
                      + "  up " + fmt((double) g.uptime_s / 3600.0, 1) + "h";
    title = clip(title, (size_t) W - 4);
    out += "\xe2\x95\xad\xe2\x94\x80 "; // ╭─
    out += title;
    {
        size_t tc = u8len(title.data(), title.size());
        size_t fill = (size_t) std::max<long>(1, (long) W - 4 - (long) tc);
        for (size_t i = 0; i < fill; i++) {
            out += "\xe2\x94\x80"; // ─
        }
        out += "\xe2\x95\xae\n";    // ╮
    }

    // ---- status row -------------------------------------------------------
    {
        const char * status = g.sleeping ? "SLEEP" : "RUN";
        char buf[256];
        snprintf(buf, sizeof(buf), " %s  busy %d/%d  queue %d  engine %d%%  ref 1.0s",
                 status, g.busy, (int) g.n_slots, g.deferred, (int) (g.engine_busy * 100.0 + 0.5));
        std::string row = "\xe2\x94\x82" + std::string(buf); // │
        pad_cols(row, (size_t) W - 1);
        row += "\xe2\x94\x82\n";
        out += row;
    }

    const size_t L = (size_t) ((W - 2) / 2 - 1); // left panel content width

    // ---- MEMORY | THROUGHPUT header --------------------------------------
    {
        std::string a = "\xe2\x94\x9c\xe2\x94\x80 MEMORY ";        // ├─ MEMORY
        std::string b = " THROUGHPUT \xe2\x94\xa4";                // THROUGHPUT ┤
        pad_cols(a, L + 1);
        pad_cols(b, (size_t) W - L - 2);
        out += a + "\xe2\x94\xac" + b + "\n";                      // ┬
    }

    // used/reserved cells
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

    // ---- MEMORY | THROUGHPUT body ----------------------------------------
    char buf[256];
    {
        snprintf(buf, sizeof(buf), " kv gpu %s   kv cpu %s", bytes(g.kv_gpu).c_str(), bytes(g.kv_cpu).c_str());
        std::string a = buf;
        std::string b = " prompt " + fmt(g.prompt_tps, 0) + " t/s    gen " + fmt(g.gen_tps, 0) + " t/s";
        pad_cols(a, L);
        pad_cols(b, (size_t) (W - 2) - L - 1);
        out += "\xe2\x94\x82" + a + "\xe2\x94\x82" + b + "\xe2\x94\x82\n";
    }
    {
        snprintf(buf, sizeof(buf), " weights gpu %s", bytes(g.weights_gpu).c_str());
        std::string a = buf;
        std::string b = " spec acc " + (g.spec_acc >= 0 ? fmt(g.spec_acc * 100.0, 0) + "%" : std::string("-"))
                      + "    hit "   + (g.hit_rate >= 0 ? fmt(g.hit_rate * 100.0, 0) + "%" : std::string("-"));
        pad_cols(a, L);
        pad_cols(b, (size_t) (W - 2) - L - 1);
        out += "\xe2\x94\x82" + a + "\xe2\x94\x82" + b + "\xe2\x94\x82\n";
    }
    {
        std::string a = " cache " + bytes(g.ram_cache) + "   rss " + bytes(g.rss);
        char uc[64];
        snprintf(uc, sizeof(uc), "%d/%d", (int) used_cells, (int) resv_cells);
        std::string b = " pp " + fmt(g.pp_ms_tok, 0) + "ms/t  tg " + fmt(g.tg_ms_tok, 0) + "ms/t  ftok "
                      + (g.first_tok_s > 0 ? fmt(g.first_tok_s, 2) + "s" : std::string("-"));
        pad_cols(a, L);
        pad_cols(b, (size_t) (W - 2) - L - 1);
        out += "\xe2\x94\x82" + a + "\xe2\x94\x82" + b + "\xe2\x94\x82\n";
    }

    // ---- GPU | SYSTEM header ---------------------------------------------
    // format: "├─ GPU 0 ───────┴─ SYSTEM ─────┤"
    {
        const size_t L2 = (size_t) ((W - 2) / 2 - 1);
        std::string a = "\xe2\x94\x9c\xe2\x94\x80 GPU 0 "; // ├─ GPU 0
        std::string b = " SYSTEM \xe2\x94\xa4";              //  SYSTEM ┤
        pad_cols(a, L2 + 1);
        pad_cols(b, (size_t) W - L2 - 2);
        out += a + "\xe2\x94\xb4" + b + "\n";              // ┴
    }
    (void) L;

    // ---- GPU | SYSTEM body ------------------------------------------------
    {
        char buf[256];
        if (ext.gpu_avail) {
            snprintf(buf, sizeof(buf), " SM %d%%   mem %d%%   pwr %.0fW  temp %dC",
                     ext.gpu_sm, ext.gpu_mem, ext.gpu_pwr, ext.gpu_temp);
        } else {
            snprintf(buf, sizeof(buf), "%s", " SM -   mem -   pwr -   temp -");
        }
        std::string a = buf;
        if (ext.proc_avail) {
            snprintf(buf, sizeof(buf), " cpu %.0f%%  ioR %.0fM/s  ioW %.0fM/s", ext.cpu_pct, ext.io_r, ext.io_w);
        } else {
            snprintf(buf, sizeof(buf), "%s", " cpu -   ioR -   ioW -");
        }
        std::string b = buf;
        size_t L2 = (size_t) ((W - 2) / 2 - 1);
        pad_cols(a, L2);
        pad_cols(b, (size_t) (W - 2) - L2 - 1);
        out += "\xe2\x94\x82" + a + "\xe2\x94\x82" + b + "\xe2\x94\x82\n";
    }
    {
        char buf[256];
        if (ext.gpu_avail && ext.pcie_avail) {
            snprintf(buf, sizeof(buf), " pclk %.1fG  mclk %.1fG  pcie rx %.0fM/s  tx %.0fM/s",
                     ext.gpu_pclk / 1000.0, ext.gpu_mclk / 1000.0, ext.pcie_rx, ext.pcie_tx);
        } else if (ext.gpu_avail) {
            snprintf(buf, sizeof(buf), " pclk %.1fG  mclk %.1fG  pcie -",
                     ext.gpu_pclk / 1000.0, ext.gpu_mclk / 1000.0);
        } else {
            snprintf(buf, sizeof(buf), "%s", " pclk -   mclk -   pcie -");
        }
        std::string a = buf;
        std::string b;
        if (g.req_valid) {
            char rb[128];
            snprintf(rb, sizeof(rb), " req q %.1fs pp %.1fs dec %.1fs", g.req_q_s, g.req_pp_s, g.req_gen_s);
            b = rb;
        } else {
            b = " req phases -";
        }
        size_t L2 = (size_t) ((W - 2) / 2 - 1);
        pad_cols(a, L2);
        pad_cols(b, (size_t) (W - 2) - L2 - 1);
        out += "\xe2\x94\x82" + a + "\xe2\x94\x82" + b + "\xe2\x94\x82\n";
    }

    // ---- SEQUENCES header -------------------------------------------------
    {
        char h[64];
        snprintf(h, sizeof(h), "\xe2\x94\x9c\xe2\x94\x80 SEQUENCES \xc2\xb7 %d \xe2\x94\x80", (int) slots.size());
        std::string a = h;
        pad_cols(a, (size_t) W - 1);
        out += a + "\xe2\x94\xa4\n"; // ┤
    }

    // ---- SEQUENCES column header -----------------------------------------
    // positions must match slot_summary() column layout
    {
        char hdr[96];
        std::memset(hdr, ' ', sizeof(hdr));
        hdr[2] = '#';
        std::memcpy(hdr + 5,  "ph", 2);
        std::memcpy(hdr + 9,  "kv", 2);
        hdr[18] = 'l';
        std::memcpy(hdr + 20, "len", 3);
        std::memcpy(hdr + 32, "cch", 3);
        std::memcpy(hdr + 39, "pp/t", 4);
        std::memcpy(hdr + 48, "tg/t", 4);
        hdr[70] = '\0';
        std::string a = hdr;
        std::string row = "\xe2\x94\x82" + clip(a, (size_t) W - 2);
        pad_cols(row, (size_t) W - 1);
        row += "\xe2\x94\x82\n";
        out += row;
    }

    // ---- sequence blocks --------------------------------------------------
    std::vector<int> active;
    std::vector<int> idle;
    for (int i = 0; i < (int) slots.size(); i++) {
        if (slots[i].ph == phase::idle) {
            idle.push_back(i);
        } else {
            active.push_back(i);
        }
    }

    const int rows_total   = H;
    const int rows_fixed   = 11; // title..column header
    const int rows_avail   = rows_total - rows_fixed - 1; // -1 bottom border
    int T = 1;
    if (!active.empty()) {
        T = std::max(1, (rows_avail - (int) idle.size()) / (int) active.size());
        T = std::min(T, 40);
    }

    auto emit_row = [&](const std::string & content) {
        std::string row = "\xe2\x94\x82" + content;
        pad_visual(row, (size_t) W - 1);
        row += "\xe2\x94\x82\n";
        out += row;
    };

    int used_rows = 0;
    int rendered_active = 0;
    for (int i : active) {
        if (used_rows >= rows_avail) break;
        const slot_snap & s = slots[i];

        std::string tail_txt(s.tail, s.tail_len);
        if (tail_txt.empty()) {
            tail_txt = s.ph == phase::prefill ? "(prefill, no text yet)" : "(generating)";
        }

        emit_row(slot_summary(s));
        used_rows++;
        rendered_active++;

        const int tail_w = W - 6;
        // tail top
        {
            std::string r = " \xe2\x95\xad";                 // ╭
            r += rep("\xe2\x94\x80", tail_w);                // ─
            r += "\xe2\x95\xae ";                             // ╮
            emit_row(r);
            used_rows++;
        }
        auto lines = wrap_tail(tail_txt.data(), (int) tail_txt.size(), (size_t) tail_w, (size_t) T);
        for (size_t li = 0; li < lines.size() && used_rows < rows_avail; li++) {
            std::string t = lines[li];
            pad_cols(t, (size_t) tail_w);
            std::string r = " \xe2\x94\x82" + t + "\xe2\x94\x82 "; // │ t │
            emit_row(r);
            used_rows++;
        }
        // tail bottom
        if (used_rows < rows_avail) {
            std::string r = " \xe2\x95\xb0";                 // ╰
            r += rep("\xe2\x94\x80", tail_w);                // ─
            r += "\xe2\x95\xaf ";                             // ╯
            emit_row(r);
            used_rows++;
        }
    }

    int rendered_idle = 0;
    for (int i : idle) {
        if (used_rows >= rows_avail) break;
        rendered_idle++;
        const slot_snap & s = slots[i];
        char note[128];
        if (s.occupied) {
            snprintf(note, sizeof(note), "idle %ds KV resident", s.idle_age_s);
        } else if (s.loc == kv_loc::ram_cache) {
            snprintf(note, sizeof(note), "%s", "RAM-cached");
        } else {
            snprintf(note, sizeof(note), "%s", "-");
        }
        std::string tail_txt(note);
        // keep the summary ASCII columns, append the note as the "tail"
        std::string sline = slot_summary(s);
        size_t s_cols = vlen(sline);
        size_t rem = (size_t) W - 2 - s_cols;
        std::string row = sline + DIM + clip(tail_txt, rem) + RESET;
        emit_row(row);
        used_rows++;
    }
    // note when sequences did not fit
    if ((int) active.size() > rendered_active || (int) idle.size() > rendered_idle) {
        if (used_rows < rows_avail) {
            char note[64];
            snprintf(note, sizeof(note), "+%d more sequence(s) hidden",
                     (int) (active.size() + idle.size()) - rendered_active - rendered_idle);
            emit_row(DIM + std::string(note) + RESET);
            used_rows++;
        }
    }
    {
        std::string b = "\xe2\x95\xb0"; // ╰
        b += rep("\xe2\x94\x80", (size_t) W - 2);
        b += "\xe2\x95\xaf\n"; // ╯
        out += b;
    }

    return out;
}

} // namespace tui
