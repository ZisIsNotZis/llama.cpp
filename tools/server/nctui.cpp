#include "nctui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <locale.h>
#include <unistd.h>
#endif
#include <curses.h>

namespace nctui {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static std::string fstr(double v, int prec) {
    char b[32];
    snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

static std::string fint(long long v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", v);
    return b;
}

static std::string bytes(size_t b) {
    static const char * u[] = {"B", "kB", "MB", "GB", "TB"};
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
        snprintf(buf, sizeof(buf), "%.1f%s", v, u[i]);
    }
    return buf;
}

// clip to at most `cols` columns at a utf-8 boundary (wide chars undercount)
static std::string clip(const std::string & s, size_t cols) {
    size_t c = 0, i = 0;
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

// approximate column width of a utf-8 string (narrow chars only)
static size_t u8_cols(const std::string & s) {
    size_t c = 0, i = 0;
    while (i < s.size()) {
        unsigned char b = (unsigned char) s[i];
        size_t l = 1;
        if      (b >= 0xF0) l = 4;
        else if (b >= 0xE0) l = 3;
        else if (b >= 0xC0) l = 2;
        if (i + l > s.size()) l = s.size() - i;
        i += l;
        c++;
    }
    return c;
}

// byte length of the prefix of s starting at `off` that fits in `max_cols`
static size_t take_prefix_at(const std::string & s, size_t off, size_t max_cols) {
    size_t c = 0, i = off;
    while (i < s.size() && c < max_cols) {
        unsigned char b = (unsigned char) s[i];
        size_t l = 1;
        if      (b >= 0xF0) l = 4;
        else if (b >= 0xE0) l = 3;
        else if (b >= 0xC0) l = 2;
        if (i + l > s.size()) l = s.size() - i;
        i += l;
        c++;
    }
    return i - off;
}

// hard-wrap a styled line into visual rows of `width` columns, preserving
// per-run style/color across the wrap boundaries
static std::vector<markup::line> wrap_line(const markup::line & ml, int width) {
    std::vector<markup::line> rows;
    markup::line cur;
    int col = 0;
    for (const auto & r : ml) {
        size_t i = 0;
        while (i < r.text.size()) {
            if (col >= width) {
                rows.push_back(cur);
                cur  = markup::line();
                col  = 0;
            }
            const size_t len = take_prefix_at(r.text, i, (size_t) std::max(1, width - col));
            if (len == 0) {
                break;
            }
            markup::run sub;
            sub.text  = r.text.substr(i, len);
            sub.style = r.style;
            sub.color = r.color;
            if (!cur.empty() && cur.back().style == sub.style && cur.back().color == sub.color) {
                cur.back().text += sub.text;
            } else {
                cur.push_back(std::move(sub));
            }
            col += (int) u8_cols(sub.text);
            i   += len;
        }
    }
    if (!cur.empty() || rows.empty()) {
        rows.push_back(cur);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// box drawing
// ---------------------------------------------------------------------------

static void draw_hline(int y, int x, int len) {
    for (int i = 0; i < len; i++) {
        mvadd_wch(y, x + i, WACS_HLINE);
    }
}

static void draw_vline(int y, int x, int len) {
    for (int i = 0; i < len; i++) {
        mvadd_wch(y + i, x, WACS_VLINE);
    }
}

static void box_at(int y, int x, int h, int w, bool selected) {
    if (h <= 0 || w <= 0) {
        return;
    }
    if (selected) {
        attron(A_BOLD | COLOR_PAIR(2));
    }
    if (h == 1) {
        draw_hline(y, x, w);
        if (selected) {
            attroff(A_BOLD | COLOR_PAIR(2));
        }
        return;
    }
    if (w == 1) {
        draw_vline(y, x, h);
        if (selected) {
            attroff(A_BOLD | COLOR_PAIR(2));
        }
        return;
    }
    mvadd_wch(y, x, WACS_ULCORNER);
    draw_hline(y, x + 1, w - 2);
    mvadd_wch(y, x + w - 1, WACS_URCORNER);
    draw_vline(y + 1, x, h - 2);
    draw_vline(y + 1, x + w - 1, h - 2);
    mvadd_wch(y + h - 1, x, WACS_LLCORNER);
    draw_hline(y + h - 1, x + 1, w - 2);
    mvadd_wch(y + h - 1, x + w - 1, WACS_LRCORNER);
    if (selected) {
        attroff(A_BOLD | COLOR_PAIR(2));
    }
}

static void write_line(int y, int x, const std::string & s, int max_w) {
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || max_w <= 0) {
        return;
    }
    const std::string c = clip(s, (size_t) std::max(0, std::min(max_w, COLS - x)));
    mvaddstr(y, x, c.c_str());
}

// map a markup run to ncurses attributes
static attr_t run_attr(const markup::run & r) {
    attr_t a = 0;
    if (r.style & markup::S_BOLD) {
        a |= A_BOLD;
    }
    if (r.style & markup::S_ITALIC) {
        a |= A_ITALIC;
    }
    if (r.style & markup::S_DIM) {
        a |= A_DIM;
    }
    if (r.style & markup::S_HEADING) {
        a |= A_BOLD;
    }
    if (r.color != markup::C_DEFAULT) {
        a |= COLOR_PAIR(9 + r.color); // C_KW=1 -> pair 10 ...
    } else if (r.style & markup::S_CODE) {
        a |= COLOR_PAIR(17); // inline code
    } else if (r.style & markup::S_QUOTE) {
        a |= COLOR_PAIR(18); // blockquote
    }
    return a;
}

// draw one styled line, clipping to max_w columns
static void draw_styled_line(int y, int x, const markup::line & ml, int max_w) {
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || max_w <= 0) {
        return;
    }
    const int limit = std::min(max_w, COLS - x);
    int cx = x;
    for (const auto & r : ml) {
        if (cx - x >= limit) {
            break;
        }
        const std::string t = clip(r.text, (size_t) (limit - (cx - x)));
        const attr_t a = run_attr(r);
        if (a) {
            attron(a);
        }
        mvaddstr(y, cx, t.c_str());
        if (a) {
            attroff(a);
        }
        cx += (int) u8_cols(t);
    }
}

// ---------------------------------------------------------------------------
// controller
// ---------------------------------------------------------------------------

controller::controller(dash::feed & feed, printui::snapshot & snap) : feed_(feed), snap_(snap) {}

controller::~controller() {
    stop();
}

void controller::start(double ratio) {
    if (running_) {
        return;
    }
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    if (!isatty(fileno(stdout))) {
        return; // ncurses requires a tty
    }
#else
    return;
#endif
    ratio_   = ratio;
    running_ = true;
    thread_  = std::thread([this]() { run(); });
}

void controller::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void controller::run() {
    setlocale(LC_ALL, "");
    // alternate screen so the TUI does not clobber the terminal scrollback
    fputs("\033[?1049h", stdout);
    fflush(stdout);
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    start_color();
    use_default_colors();
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(250); // synthesize CLICKED / DOUBLE_CLICKED within 250 ms
    set_escdelay(25);

    init_pair(1, COLOR_CYAN,    -1); // headers
    init_pair(2, COLOR_GREEN,   -1); // selected border / active
    init_pair(3, COLOR_YELLOW,  -1); // prefill
    init_pair(4, COLOR_MAGENTA, -1); // cached
    init_pair(5, COLOR_BLUE,    -1); // idle
    // syntax colors: pair = 9 + markup::color
    init_pair(10, COLOR_CYAN,    -1); // keyword
    init_pair(11, COLOR_YELLOW,  -1); // string
    init_pair(12, COLOR_BLUE,    -1); // comment
    init_pair(13, COLOR_MAGENTA, -1); // number
    init_pair(14, -1, -1); // symbol (default fg)
    init_pair(15, COLOR_GREEN,   -1); // meta / macro / key
    init_pair(16, COLOR_GREEN,   -1); // type
    init_pair(17, COLOR_CYAN,    -1); // inline code
    init_pair(18, COLOR_BLUE,    -1); // blockquote

    // auto ratio: square cells on the usable grid area (terminal rows/cols minus
    // the 4 header lines and the status line). Yields 2x2 for 4 cells on any
    // screen; '['/']' still override live.
    if (ratio_ < 0.0) {
        ratio_ = std::min(0.75, std::max(0.12, (double) std::max(1, LINES - 5) / (double) std::max(1, COLS)));
    }

    while (running_) {
        // read the shared data
        std::vector<dash::cell> cells = feed_.snapshot();
        printui::global_snap g;
        printui::slot_snap slots[printui::MAX_SLOTS];
        int n_slots = 0;
        {
            std::lock_guard<std::mutex> lk(snap_.mtx);
            g = snap_.global;
            n_slots = std::min(snap_.n_slots, (int) printui::MAX_SLOTS);
            for (int i = 0; i < n_slots; i++) {
                slots[i] = snap_.slots[i];
            }
        }

        handle_input(cells);
        render(cells, g, slots, n_slots);
        doupdate();

        napms(30); // ~30 fps
    }

    endwin();
    fputs("\033[?1049l", stdout);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// input handling
// ---------------------------------------------------------------------------

std::vector<cell_rect> controller::layout_cells(const std::vector<dash::cell> & cells) {
    std::vector<cell_rect> rects;
    const int header_h = 4;
    const int bottom_h = 1;
    const int grid_rows = std::max(1, LINES - header_h - bottom_h);

    std::vector<const dash::cell *> active;
    for (const auto & c : cells) {
        if (!c.evicted) {
            active.push_back(&c);
        }
    }

    // maximized cell fills the whole grid area
    if (max_id_ >= 0) {
        for (const auto * c : active) {
            if (c->id == max_id_) {
                rects.push_back({max_id_, header_h, 0, grid_rows, COLS});
                return rects;
            }
        }
        max_id_ = -1; // maximized cell is gone; fall back to grid
    }

    int H = 1, W = 1;
    printui::grid_dims((int) active.size(), grid_rows, COLS, ratio_, H, W);
    const int cell_h = std::max(1, grid_rows / H);
    const int cell_w = std::max(2, COLS / W);
    for (int i = 0; i < (int) active.size(); i++) {
        const int r = i / W;
        const int c = i % W;
        const int y = header_h + r * cell_h;
        const int x = c * cell_w;
        if (y + cell_h > LINES || x >= COLS) {
            break;
        }
        rects.push_back({active[i]->id, y, x, cell_h, cell_w});
    }
    return rects;
}

int controller::cell_at(int y, int x) const {
    for (const auto & r : rects_) {
        if (y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w) {
            return r.id;
        }
    }
    return -1;
}

const std::vector<markup::line> & controller::parsed_lines(const dash::cell & cell) {
    auto it = md_.find(cell.id);
    if (it == md_.end() || it->second.tseq != cell.tseq) {
        cell_md m;
        m.tseq  = cell.tseq;
        m.lines = markup::parse(cell.text);
        md_[cell.id] = std::move(m);
    }
    return md_[cell.id].lines;
}

int controller::cell_total(int id, const std::vector<dash::cell> & cells) {
    for (const auto & c : cells) {
        if (c.id == id) {
            const int w = cell_win_w(id);
            size_t n = 0;
            for (const auto & ml : parsed_lines(c)) {
                n += wrap_line(ml, w).size();
            }
            return (int) n;
        }
    }
    return 0;
}

int controller::cell_win_h(int id) const {
    for (const auto & r : rects_) {
        if (r.id == id) {
            return std::max(1, r.h - 3);
        }
    }
    return 1;
}

int controller::cell_win_w(int id) const {
    for (const auto & r : rects_) {
        if (r.id == id) {
            return std::max(1, r.w - 2);
        }
    }
    return 40;
}

int controller::compute_top(int id, int total, int win_h) const {
    const auto it = anchor_.find(id);
    const int  a  = it == anchor_.end() ? -1 : it->second;
    if (a < 0 || total <= win_h) {
        return std::max(0, total - win_h); // pinned at bottom / content fits
    }
    return std::min(a, std::max(0, total - win_h));
}

void controller::select(int id) {
    if (id >= 0) {
        sel_id_ = id;
    }
}

void controller::move_sel(int dx, const std::vector<dash::cell> & cells) {
    std::vector<int> ids;
    for (const auto & c : cells) {
        if (!c.evicted) {
            ids.push_back(c.id);
        }
    }
    if (ids.empty()) {
        sel_id_ = -1;
        return;
    }
    int pos = 0;
    for (int i = 0; i < (int) ids.size(); i++) {
        if (ids[i] == sel_id_) {
            pos = i;
            break;
        }
    }
    pos = (pos + dx + (int) ids.size()) % (int) ids.size();
    sel_id_ = ids[pos];
}

void controller::scroll_cell(int id, int dir, const std::vector<dash::cell> & cells) {
    if (id < 0) {
        return;
    }
    const int total = cell_total(id, cells);
    const int win_h = cell_win_h(id);
    if (total <= win_h) {
        anchor_[id] = -1; // nothing to scroll
        return;
    }
    int & a = anchor_[id];
    if (dir > 0) { // scroll up (toward older content)
        if (a < 0) {
            a = total - win_h - 1; // unpin: one line above the bottom
        } else {
            a = std::max(0, a - 1);
        }
    } else { // scroll down (toward newer content)
        if (a < 0) {
            return; // already pinned at bottom
        }
        a++;
        if (a >= total - win_h) {
            a = -1; // back to pinned / auto-follow
        }
    }
}

void controller::scroll_sel(int dir, const std::vector<dash::cell> & cells) {
    scroll_cell(sel_id_, dir, cells);
}

void controller::toggle_max(int id) {
    if (id < 0) {
        return;
    }
    if (max_id_ == id) {
        max_id_ = -1;
    } else {
        max_id_ = id;
    }
}

void controller::set_status(const std::string & msg) {
    status_msg_   = msg;
    status_until_ = (int64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 4 * 1000 * 1000;
}

void controller::save_cell(const std::vector<dash::cell> & cells) {
    int id = max_id_ >= 0 ? max_id_ : (sel_id_ >= 0 ? sel_id_ : -1);
    const dash::cell * cell = nullptr;
    for (const auto & c : cells) {
        if (c.id == id) {
            cell = &c;
            break;
        }
    }
    if (!cell) {
        set_status("no active cell to save");
        return;
    }
    char fn[64];
    const time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(fn, sizeof(fn), "%y%m%d%H%M%S.txt", &tmv);
    FILE * f = fopen(fn, "w");
    if (!f) {
        set_status(std::string("cannot write ") + fn);
        return;
    }
    fwrite(cell->text.data(), 1, cell->text.size(), f);
    fclose(f);
    set_status("saved " + std::string(fn));
}

void controller::set_hooks(nctui_hooks hooks) {
    hooks_ = std::move(hooks);
}

static bool is_shift_enter() {
    // consume the rest of a modified-key sequence after an initial ESC
    char buf[8];
    int n = 0;
    while (n < 7) {
        const int c = getch();
        if (c == ERR) {
            break;
        }
        buf[n++] = (char) c;
    }
    const std::string seq(buf, n);
    return seq == "[13;2u" || seq == "[13;2~";
}

void controller::start_input(int id_slot) {
    input_slot_ = id_slot;
    input_.clear();
    input_mode_ = true;
}

void controller::end_input(bool submit) {
    if (submit && !input_.empty() && input_slot_ >= 0) {
        if (hooks_.submit_completion) {
            hooks_.submit_completion(input_slot_, input_);
            set_status("fired completion to slot " + std::to_string(input_slot_));
        }
    }
    input_mode_ = false;
    input_.clear();
    input_slot_ = -1;
}

void controller::handle_input(const std::vector<dash::cell> & cells) {
    rects_ = layout_cells(cells);

    int ch;
    while ((ch = getch()) != ERR) {
        // input mode captures text; only a few keys escape it
        if (input_mode_) {
            if (ch == 27) { // ESC or a modified-key prefix (Shift+Enter)
                if (is_shift_enter()) {
                    input_.push_back('\n');
                } else {
                    end_input(false);
                }
            } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                end_input(true);
            } else if (ch == KEY_BACKSPACE || ch == 0x7f || ch == 0x08) {
                if (!input_.empty()) {
                    input_.pop_back();
                }
            } else if (ch == KEY_RESIZE) {
                // layout recomputes; keep the input
            } else if (ch >= 32 && ch < 127) {
                input_.push_back((char) ch);
            }
            continue;
        }

        if (ch == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                const int id = cell_at(me.y, me.x);
                if (me.bstate & (BUTTON4_PRESSED | BUTTON4_CLICKED)) {
                    scroll_cell(id, +1, cells);
                } else if (me.bstate & (BUTTON5_PRESSED | BUTTON5_CLICKED)) {
                    scroll_cell(id, -1, cells);
                } else if (me.bstate & BUTTON1_DOUBLE_CLICKED) {
                    select(id);
                    toggle_max(id);
                } else if (me.bstate & BUTTON1_CLICKED) {
                    select(id);
                }
            }
        } else {
            switch (ch) {
            case 'q':
            case 'Q':
                running_ = false;
                return;
            case '[': // wider cells (fewer columns)
                ratio_ = std::max(0.08, ratio_ - 0.02);
                set_status("ratio " + fstr(ratio_, 2));
                break;
            case ']': // narrower cells (more columns)
                ratio_ = std::min(1.0, ratio_ + 0.02);
                set_status("ratio " + fstr(ratio_, 2));
                break;
            case 's':
            case 'S':
                save_cell(cells);
                break;
            case 'i':
            case 'I': {
                int target = sel_id_;
                if (target < 0 && !cells.empty()) {
                    target = cells[0].id;
                }
                if (target >= 0) {
                    start_input(target);
                }
                break;
            }
            case 'k':
            case 'K': {
                int target = sel_id_;
                if (target < 0 && !cells.empty()) {
                    target = cells[0].id;
                }
                if (target >= 0 && hooks_.kill_slot) {
                    hooks_.kill_slot(target);
                    set_status("kill slot " + std::to_string(target));
                }
                break;
            }
            case KEY_LEFT:
                move_sel(-1, cells);
                break;
            case KEY_RIGHT:
                move_sel(+1, cells);
                break;
            case KEY_UP:
                scroll_sel(+1, cells);
                break;
            case KEY_DOWN:
                scroll_sel(-1, cells);
                break;
            case '\n':
            case KEY_ENTER:
                toggle_max(sel_id_);
                break;
            default:
                break; // KEY_RESIZE: LINES/COLS refresh automatically
            }
        }
    }

    // keep selection valid and prune stale anchors
    if (sel_id_ >= 0) {
        bool found = false;
        for (const auto & c : cells) {
            if (!c.evicted && c.id == sel_id_) {
                found = true;
                break;
            }
        }
        if (!found) {
            for (const auto & c : cells) {
                if (!c.evicted) {
                    sel_id_ = c.id;
                    found   = true;
                    break;
                }
            }
            if (!found) {
                sel_id_ = -1;
            }
        }
    }
    for (auto it = anchor_.begin(); it != anchor_.end();) {
        bool found = false;
        for (const auto & c : cells) {
            if (c.id == it->first) {
                found = true;
                break;
            }
        }
        if (!found) {
            it = anchor_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = md_.begin(); it != md_.end();) {
        bool found = false;
        for (const auto & c : cells) {
            if (c.id == it->first) {
                found = true;
                break;
            }
        }
        if (!found) {
            it = md_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

void controller::render(const std::vector<dash::cell> & cells, const printui::global_snap & g,
                        const printui::slot_snap * slots, int n_slots) {
    erase();

    // ---- global stats header ----
    {
        std::string m = std::string(g.model_desc) + "  (" + std::string(g.alias) + ")  ctx "
                      + fint(g.n_ctx) + "/" + fint(g.n_ctx_train) + "  " + (g.kv_unified ? "unified" : "split")
                      + "  KV " + g.kv_type + "  FA " + g.flash_attn;
        attron(COLOR_PAIR(1) | A_BOLD);
        write_line(0, 0, m, COLS);
        attroff(COLOR_PAIR(1) | A_BOLD);

        std::string r = "busy " + fint(g.busy) + "/" + fint(g.n_slots)
                      + "  queue " + fint(g.deferred) + "  engine " + fint((long long) (g.engine_busy * 100.0))
                      + "%  req " + fstr(g.req_per_s, 1) + "/s  prompt " + fstr(g.prompt_tps, 0)
                      + "/s  gen " + fstr(g.gen_tps, 0) + "/s  hit " + (g.hit_rate >= 0 ? fstr(g.hit_rate * 100.0, 0) + "%" : "-");
        if (g.speculative) {
            r += "  spec acc " + (g.spec_acc >= 0 ? fstr(g.spec_acc * 100.0, 0) + "%" : "-")
               + " len " + (g.spec_acc_len > 0 ? fstr(g.spec_acc_len, 1) : "-")
               + "/" + (g.spec_prop_len > 0 ? fstr(g.spec_prop_len, 1) : "-");
        }
        write_line(1, 0, r, COLS);

        std::string t = "pp " + fstr(g.pp_ms_tok, 0) + "ms  tg " + fstr(g.tg_ms_tok, 0) + "ms  ftok "
                      + (g.first_tok_s > 0 ? fstr(g.first_tok_s, 2) + "s" : "-")
                      + "  kv gpu " + bytes(g.kv_gpu_used) + "  kv cpu " + bytes(g.kv_cpu_used)
                      + "  weights " + bytes(g.weights_gpu + g.weights_cpu) + "  cache " + bytes(g.ram_cache)
                      + "  rss " + bytes(g.rss);
        write_line(2, 0, t, COLS);

        std::string total = "total prompt " + fint(g.total_prompt) + "  gen " + fint(g.total_gen)
                          + "  cached " + fint(g.total_cached) + "  decode " + fint(g.total_decode)
                          + "  ratio " + fstr(ratio_, 2);
        write_line(3, 0, total, COLS);
    }

    const int header_h = 4;
    const int bottom_h = 1;

    // active vs cached
    std::vector<const dash::cell *> active, cached;
    for (const auto & c : cells) {
        if (c.evicted) {
            cached.push_back(&c);
        } else {
            active.push_back(&c);
        }
    }

    // active cell boxes
    for (const auto & r : rects_) {
        const dash::cell * cell = nullptr;
        for (const auto & c : active) {
            if (c->id == r.id) {
                cell = c;
                break;
            }
        }
        if (!cell) {
            continue;
        }
        const printui::slot_snap * s = nullptr;
        for (int k = 0; k < n_slots; k++) {
            if (slots[k].id == cell->id) {
                s = &slots[k];
                break;
            }
        }
        box_at(r.y, r.x, r.h, r.w, r.id == sel_id_);

        const char * phase = s ? (s->ph == printui::phase::prefill ? "PF" : s->ph == printui::phase::decode ? "DEC" : "IDL") : "-";
        std::string head = " SEQ " + std::to_string(cell->id) + " " + phase
                         + " kv:" + (s ? (std::to_string(s->kv_used) + "/" + std::to_string(s->n_ctx)) : "-")
                         + " pp:" + (s && s->pp5_tps > 0 ? fstr(s->pp5_tps, 0) : "-")
                         + " tg:" + (s && s->tg5_tps > 0 ? fstr(s->tg5_tps, 0) : "-")
                         + " hit:" + (cell->hit_rate >= 0 ? fstr(cell->hit_rate * 100.0, 0) + "%" : "-");
        write_line(r.y + 1, r.x + 1, head, r.w - 2);

        // text window: wrap styled lines to the cell width, then slice a
        // window around the scroll anchor; right-edge scroll indicator
        const auto & mlines = parsed_lines(*cell);
        const int win_h = std::max(1, r.h - 3);
        const int wrap_w = std::max(1, r.w - 2);
        std::vector<markup::line> vrows;
        for (const auto & ml : mlines) {
            auto w = wrap_line(ml, wrap_w);
            vrows.insert(vrows.end(), w.begin(), w.end());
        }
        const int total = (int) vrows.size();
        const bool scrollable = total > win_h;
        const int text_w = std::max(0, wrap_w - (scrollable ? 1 : 0));
        const int top = compute_top(cell->id, total, win_h);
        for (int ln = 0; ln < win_h; ln++) {
            const int idx = top + ln;
            if (idx < total) {
                draw_styled_line(r.y + 2 + ln, r.x + 1, vrows[idx], text_w);
            }
        }
        if (scrollable && r.w >= 4) {
            const int ind_x = r.x + r.w - 2; // column just inside the right border
            const double vpos = (total - win_h) > 0 ? (double) top / (double) (total - win_h) : 1.0;
            int bh = std::max(1, (int) ((double) win_h * (double) win_h / (double) total));
            bh = std::min(bh, win_h);
            const int by = (int) (vpos * (double) (win_h - bh));
            for (int row = 0; row < win_h; row++) {
                if (row >= by && row < by + bh) {
                    mvaddstr(r.y + 2 + row, ind_x, "\xe2\x96\x88"); // full block
                }
            }
        }
    }

    // cached flat list (hidden when a cell is maximized)
    if (max_id_ < 0) {
        int cy = header_h + (LINES - header_h - bottom_h);
        for (const auto * c : cached) {
            if (cy >= LINES) {
                break;
            }
            std::string line = " CACHE " + std::to_string(c->id)
                             + "  hit:" + (c->hit_rate >= 0 ? fstr(c->hit_rate * 100.0, 0) + "%" : "-")
                             + "  " + clip(c->text, 60);
            attron(COLOR_PAIR(4));
            write_line(cy, 0, line, COLS);
            attroff(COLOR_PAIR(4));
            cy++;
        }
    }

    // bottom status line (transient messages like save results override the hint)
    attron(A_DIM);
    const int64_t now_us = (int64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (input_mode_) {
        std::string bar = "llama-server  [SEQ " + std::to_string(input_slot_) + "] > ";
        for (const char c : input_) {
            bar += c == '\n' ? "\xE2\x86\xB5" : std::string(1, c);
        }
        bar += " \xE2\x96\x88  [Enter] send  [Shift+Enter] newline  [Esc] cancel";
        write_line(LINES - 1, 0, bar, COLS);
    } else if (now_us < status_until_ && !status_msg_.empty()) {
        write_line(LINES - 1, 0, "llama-server  " + status_msg_, COLS);
    } else if (max_id_ >= 0) {
        write_line(LINES - 1, 0, "llama-server  [q] detach  [i] input  [k] kill  [\xE2\x86\x91/\xE2\x86\x93] scroll  [s] save  [Enter / 2x click] restore  [Ctrl-C] stop", COLS);
    } else {
        write_line(LINES - 1, 0, "llama-server  [q] detach  [L/R] select  [\xE2\x86\x91/\xE2\x86\x93] scroll  [[/]] ratio  [i] input  [k] kill  [s] save  [Enter / 2x click] maximize  [Ctrl-C] stop", COLS);
    }
    attroff(A_DIM);
}

} // namespace nctui
