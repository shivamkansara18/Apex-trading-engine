#pragma once
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace apex {
// Mutex unlock/lock publishes ownership of each moved value. No borrowed references
// escape the lock. Producers never block waiting for capacity; callers own retries.
template<class T> class BoundedQueue {
public:
    struct Stats { std::size_t size, high_watermark, rejected; };
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) { throw std::invalid_argument("Queue capacity must be positive"); }
    }
    bool try_push(T value) {
        std::lock_guard lock(mutex_);
        if (queue_.size() == capacity_) { ++rejected_; return false; }
        queue_.push_back(std::move(value));
        if (queue_.size() > high_) { high_ = queue_.size(); }
        return true;
    }
    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) { return std::nullopt; }
        T value = std::move(queue_.front()); queue_.pop_front(); return value;
    }
    Stats stats() const { std::lock_guard lock(mutex_); return {queue_.size(), high_, rejected_}; }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> queue_;
    std::size_t high_{}, rejected_{};
};
} // namespace apex
