#include "server-dash.h"

#include <algorithm>

namespace dash {

void feed::begin() {
    std::lock_guard<std::mutex> lk(mtx_);
    frame_open_ = true;
    seen_.clear();
}

void feed::update_active(int id, bool processing, int64_t t_created, int64_t t_modified,
                         double hit_rate, uint64_t tseq, const std::string & full, const std::string & delta) {
    (void) processing; // active/idle distinction is via `active`; phase is derived per-consumer
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cells_.find(id);
    if (it == cells_.end()) {
        // new cell
        cell c;
        c.id = id;
        c.active     = true;
        c.evicted    = false;
        c.t_created  = t_created;
        c.t_modified = t_modified;
        c.hit_rate   = hit_rate;
        c.tseq       = tseq;
        c.text       = full.empty() ? delta : full;
        cells_[id] = c;
        push({ gseq_, EV_ADD, id, tseq, c.text });
    } else {
        cell & c = it->second;
        const bool was_evicted = c.evicted;
        c.active     = true;
        c.evicted    = false;
        c.t_created  = t_created != 0 ? t_created : c.t_created;
        c.t_modified = t_modified;
        c.hit_rate   = hit_rate;
        c.tseq       = tseq;
        if (!delta.empty()) {
            c.text += delta;
            push({ gseq_, EV_DELTA, id, tseq, delta });
        } else if (!full.empty() && full != c.text) {
            c.text = full;
            push({ gseq_, EV_CELL, id, tseq, full });
        }
        if (was_evicted) {
            push({ gseq_, EV_ADD, id, tseq, c.text }); // restored to active
        }
    }
    seen_.push_back(id);
}

void feed::update_evicted(int id, int64_t t_created, int64_t t_modified,
                          double hit_rate, uint64_t tseq, const std::string & text) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cells_.find(id);
    if (it == cells_.end()) {
        cell c;
        c.id = id;
        c.active     = false;
        c.evicted    = true;
        c.t_created  = t_created;
        c.t_modified = t_modified;
        c.hit_rate   = hit_rate;
        c.tseq       = tseq;
        c.text       = text;
        cells_[id] = c;
        push({ gseq_, EV_CACHE, id, tseq, text });
    } else {
        cell & c = it->second;
        const bool was_active = c.active;
        c.active     = false;
        c.evicted    = true;
        c.t_modified = t_modified;
        c.hit_rate   = hit_rate;
        c.tseq       = tseq;
        if (was_active && !c.text.empty() && text != c.text) {
            c.text = text;
            push({ gseq_, EV_CACHE, id, tseq, text });
        }
    }
    seen_.push_back(id);
}

void feed::end() {
    std::lock_guard<std::mutex> lk(mtx_);
    prune_stale();
    frame_open_ = false;
}

void feed::prune_stale() {
    // remove cells that were not seen this frame (they died or were cleared)
    for (auto it = cells_.begin(); it != cells_.end();) {
        if (std::find(seen_.begin(), seen_.end(), it->first) == seen_.end()) {
            push({ gseq_, EV_REMOVE, it->first, 0, "" });
            it = cells_.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t feed::subscribe() {
    std::lock_guard<std::mutex> lk(mtx_);
    return gseq_;
}

std::vector<cell> feed::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<cell> out;
    out.reserve(cells_.size());
    for (const auto & kv : cells_) {
        out.push_back(kv.second);
    }
    return out;
}

bool feed::has_cell(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cells_.count(id) != 0;
}

std::vector<event> feed::poll(uint64_t & cursor, bool * resync) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (resync) {
        *resync = false;
    }
    if (cursor < events_start_) {
        // ring dropped events the subscriber has not consumed yet
        if (resync) {
            *resync = true;
        }
        cursor = gseq_;
        return {};
    }
    std::vector<event> out;
    for (size_t i = 0; i < events_.size(); i++) {
        if (events_[i].gseq >= cursor) {
            out.push_back(events_[i]);
        }
    }
    cursor = gseq_;
    return out;
}

void feed::wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait_for(lk, timeout);
}

void feed::push(const event & ev) {
    if (events_.size() >= MAX_EVENTS) {
        events_.erase(events_.begin());
        events_start_++;
    }
    events_.push_back(ev);
    gseq_++;
    cv_.notify_all();
}

} // namespace dash
