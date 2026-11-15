// Bounded lock-free single-producer / single-consumer queue (wait-free push and pop).
// Used between the real-time audio callback and the app thread in both directions.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace drums::engine {

template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue elements are copied with memcpy semantics");

public:
    // capacity must be a power of two. Allocates once, here.
    explicit SpscQueue(size_t capacity) : mask_(capacity - 1), buffer_(new T[capacity]) {
        assert(capacity >= 2 && (capacity & (capacity - 1)) == 0);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer side. Returns false when full (the element is dropped by the caller).
    bool push(const T& value) noexcept {
        const size_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) > mask_) return false;
        buffer_[w & mask_] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) noexcept {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        out = buffer_[r & mask_];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const noexcept { return mask_ + 1; }

private:
    const size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace drums::engine
