#pragma once

// Interactive terminal dashboard for llama-server (--tui, ncurses).
// Reads the shared dash::feed (full text + token deltas) and the status
// snapshot (global + per-slot). See docs/dashboard/DESIGN.md section 7.3.

#include "server-dash.h"
#include "printui.h"
#include "markup.h"

#include <atomic>
#include <functional>
#include <map>
#include <thread>
#include <vector>

namespace nctui {

struct cell_rect {
    int id = -1;
    int y = 0, x = 0, h = 1, w = 1;
};

struct cell_md {
    uint64_t            tseq = 0;
    std::vector<markup::line> lines;
};

// server-side callbacks wired by server_context_impl (in-process, fire & forget)
struct nctui_hooks {
    std::function<void(int id_slot, const std::string & prompt)> submit_completion;
    std::function<void(int id_slot)>                             kill_slot;
};

class controller {
public:
    controller(dash::feed & feed, printui::snapshot & snap);
    ~controller();

    void start(double ratio); // spawn the TUI thread; no-op if stdout is not a tty
    void stop();
    bool is_active() const { return running_; }

    void set_hooks(nctui_hooks hooks); // must be called before start()

private:
    void run();
    void handle_input(const std::vector<dash::cell> & cells);
    void start_input(int id_slot);
    void end_input(bool submit);
    void render(const std::vector<dash::cell> & cells, const printui::global_snap & g,
                const printui::slot_snap * slots, int n_slots);

    std::vector<cell_rect> layout_cells(const std::vector<dash::cell> & cells);
    int  cell_at(int y, int x) const;
    void select(int id);
    void move_sel(int dx, const std::vector<dash::cell> & cells);
    void scroll_sel(int dir, const std::vector<dash::cell> & cells);
    void scroll_cell(int id, int dir, const std::vector<dash::cell> & cells);
    void toggle_max(int id);
    void save_cell(const std::vector<dash::cell> & cells);
    void set_status(const std::string & msg);
    const std::vector<markup::line> & parsed_lines(const dash::cell & cell);
    int  cell_total(int id, const std::vector<dash::cell> & cells);
    int  cell_win_h(int id) const;
    int  cell_win_w(int id) const;
    int  compute_top(int id, int total, int win_h) const;

    dash::feed &        feed_;
    printui::snapshot & snap_;
    double              ratio_  = printui::TUI_RATIO_DEFAULT;
    std::thread         thread_;
    std::atomic<bool>   running_ {false};

    int              sel_id_ = -1; // selected cell (keyboard nav), -1 = none
    int              max_id_ = -1; // maximized cell id, -1 = grid view
    std::map<int,int> anchor_;     // cell id -> absolute top line (-1 = pinned at bottom)
    std::vector<cell_rect> rects_; // last computed active-cell layout
    std::map<int, cell_md> md_;    // per-cell markdown parse cache (keyed by tseq)

    std::string status_msg_;       // transient bottom-line message (e.g. save result)
    int64_t     status_until_ = 0; // monotonic us until which status_msg_ shows

    nctui_hooks hooks_;
    bool        input_mode_ = false; // bottom bar is capturing text
    std::string input_;
    int         input_slot_ = -1;    // slot the input targets
};

} // namespace nctui
