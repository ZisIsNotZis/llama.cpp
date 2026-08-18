#pragma once

// Shared sequence feed for the monitoring feature (see docs/dashboard).
// One producer (the engine thread) writes per-sequence state + text deltas;
// consumers (the stdout TUI printer and the web SSE endpoint) read snapshots
// and incremental events. All llama calls happen on the engine thread; the
// feed only stores strings.

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dash {

constexpr size_t MAX_EVENTS = 8192; // bounded event ring

struct cell {
    int       id         = -1;
    bool      active     = false; // in a slot (processing or idle-resident)
    bool      evicted    = false; // kept in the RAM prompt cache, not in a slot
    int64_t   t_created  = 0;     // us
    int64_t   t_modified = 0;     // us
    double    hit_rate   = -1.0;  // per-sequence, -1 = n/a
    uint64_t  tseq       = 0;     // token count at the end of text
    std::string text;             // full text
};

enum event_kind : int {
    EV_DELTA  = 0, // new tokens appended to a cell (text = delta)
    EV_CELL   = 1, // metadata / state change (text = new full text if replaced)
    EV_ADD    = 2, // cell created (text = full text)
    EV_REMOVE = 3, // cell died
    EV_CACHE  = 4, // a cell became evicted (in the RAM cache)
};

struct event {
    uint64_t  gseq = 0;
    int       kind = EV_DELTA;
    int       id   = -1;
    uint64_t  tseq = 0;
    std::string text;
};

class feed {
public:
    // ---- engine thread ----
    // mark the start of a frame; prunes cells that are no longer seen this frame
    void begin();
    // active (in-slot) cell: full == new full text when text was replaced/reset,
    // delta == the newly generated substring otherwise (append to existing text)
    void update_active(int id, bool processing, int64_t t_created, int64_t t_modified,
                       double hit_rate, uint64_t tseq, const std::string & full, const std::string & delta);
    // evicted (RAM-cache) cell
    void update_evicted(int id, int64_t t_created, int64_t t_modified,
                        double hit_rate, uint64_t tseq, const std::string & text);
    void end(); // prunes cells not updated this frame

    // ---- subscriber thread ----
    uint64_t subscribe();                 // returns the current global event cursor
    std::vector<cell> snapshot() const;   // all current cells
    bool has_cell(int id) const;
    // returns events with gseq >= cursor (advances cursor). If the ring dropped
    // events (cursor behind the ring start), returns empty and sets *resync = true.
    std::vector<event> poll(uint64_t & cursor, bool * resync);
    // block until new events or timeout
    void wait(std::chrono::milliseconds timeout);

private:
    void push(const event & ev);
    void prune_stale();

    mutable std::mutex        mtx_;
    std::condition_variable   cv_;
    std::map<int, cell>       cells_;
    std::vector<event>        events_;      // ring
    uint64_t                  events_start_ = 0; // gseq of events_[0]
    uint64_t                  gseq_         = 0;
    bool                      frame_open_   = false;
    std::vector<int>          seen_;         // ids seen this frame
};

} // namespace dash
