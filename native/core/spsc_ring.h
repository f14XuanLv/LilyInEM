#pragma once

// Lock-free single-producer / single-consumer ring of fixed-size records in shared memory.
// The producer owns `head`, the consumer owns `tail`. When full the NEW record is rejected and
// counted in `dropped` (dropping the oldest would require the producer to write `tail`, which
// breaks the single-writer rule). Capacity must be a power of two.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace lie {

template <class T, std::uint32_t N>
struct SpscRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    std::atomic<std::uint32_t> head{0};    // written by producer
    std::atomic<std::uint32_t> tail{0};    // written by consumer
    std::atomic<std::uint32_t> pushed{0};  // producer statistics
    std::atomic<std::uint32_t> dropped{0}; // producer statistics
    T slots[N]{};

    bool push(const T& item) {
        const std::uint32_t h = head.load(std::memory_order_relaxed);
        const std::uint32_t t = tail.load(std::memory_order_acquire);
        if (h - t >= N) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(&slots[h & (N - 1)], &item, sizeof(T));
        head.store(h + 1, std::memory_order_release);
        pushed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool pop(T& out) {
        const std::uint32_t t = tail.load(std::memory_order_relaxed);
        const std::uint32_t h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        std::memcpy(&out, &slots[t & (N - 1)], sizeof(T));
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer joining late (or after a peer restart): discard everything already queued.
    void drain() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }

    std::uint32_t size() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
};

} // namespace lie
