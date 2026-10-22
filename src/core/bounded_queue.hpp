// Fixed-capacity FIFO with a drop-oldest overflow policy.
//
// For live vital signs the newest value is the one that matters: a station
// that falls behind should skip stale samples, not show them late. So when
// the queue is full, push() evicts the oldest element and reports it.
//
// Thread-safe: producers (the hub's publish path, many ORB threads) and the
// single consumer (the publisher thread) only hold the lock for O(batch).
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace telemetry::core {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) throw std::invalid_argument("BoundedQueue capacity must be > 0");
    }

    // Returns true when an older element was dropped to make room.
    bool push(T value) {
        std::lock_guard lock(mutex_);
        bool dropped = false;
        if (items_.size() == capacity_) {
            items_.pop_front();
            ++dropped_total_;
            dropped = true;
        }
        items_.push_back(std::move(value));
        return dropped;
    }

    // Moves up to max_items from the front into out (appended). Returns count.
    std::size_t drain(std::vector<T>& out, std::size_t max_items) {
        std::lock_guard lock(mutex_);
        const std::size_t n = std::min(max_items, items_.size());
        out.reserve(out.size() + n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
        }
        return n;
    }

    // Calls f(front) under the lock if the queue is not empty.
    template <typename F>
    bool peek_front(F&& f) const {
        std::lock_guard lock(mutex_);
        if (items_.empty()) return false;
        f(items_.front());
        return true;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t dropped_total() const {
        std::lock_guard lock(mutex_);
        return dropped_total_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> items_;
    std::size_t dropped_total_ = 0;
};

}  // namespace telemetry::core
