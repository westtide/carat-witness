// Single-producer single-consumer byte-ring of fixed 64-byte event records.
// The producer is a Carat solver thread; the consumer is the profiler's own
// sender thread. No locks, no allocation on the write path.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Protocol.h"

namespace car {
namespace prof {

class RingBuffer {
public:
    // capacity_records is rounded up to a power of two; at least 2.
    explicit RingBuffer(size_t capacity_records)
        : mask_(RoundUpPow2(capacity_records) - 1),
          slots_(RoundUpPow2(capacity_records)) {}

    // Producer thread only. Returns false when full (caller counts a drop).
    bool Push(const EventRecord &rec) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= slots_.size()) return false;
        slots_[h & mask_] = rec;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Pops up to max records into out.
    size_t Pop(EventRecord *out, size_t max) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        const size_t n = static_cast<size_t>(h - t) < max
                             ? static_cast<size_t>(h - t)
                             : max;
        for (size_t i = 0; i < n; ++i) out[i] = slots_[(t + i) & mask_];
        if (n > 0) tail_.store(t + n, std::memory_order_release);
        return n;
    }

    size_t Capacity() const { return slots_.size(); }
    // Approximate fill level (racy read is fine for telemetry).
    size_t Size() const {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        return static_cast<size_t>(h - t);
    }

private:
    static size_t RoundUpPow2(size_t v) {
        size_t p = 2;
        while (p < v) p <<= 1;
        return p;
    }

    uint64_t mask_;
    std::vector<EventRecord> slots_;
    alignas(64) std::atomic<uint64_t> head_{0};  // written by producer
    alignas(64) std::atomic<uint64_t> tail_{0};  // written by consumer
};

}  // namespace prof
}  // namespace car
