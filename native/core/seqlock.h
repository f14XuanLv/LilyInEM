#pragma once

// Single-writer / multi-reader seqlock for trivially copyable state in shared memory.
// The writer bumps seq to odd, writes, bumps to even. A reader retries until it sees the same even seq
// before and after copying. Never blocks the writer.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace lie {

template <class T>
struct Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    std::atomic<std::uint32_t> seq{0};
    T value{};

    template <class F>
    void write(F&& mutate) {
        const std::uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        mutate(value);
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release);
    }

    void store(const T& v) {
        write([&](T& dst) { std::memcpy(&dst, &v, sizeof(T)); });
    }

    // Returns false while nothing was ever written or the writer kept racing us.
    bool read(T& out, int attempts = 64) const {
        for (int i = 0; i < attempts; ++i) {
            const std::uint32_t s0 = seq.load(std::memory_order_acquire);
            if (s0 == 0) return false;
            if (s0 & 1u) continue;
            std::memcpy(&out, const_cast<const T*>(&value), sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s0) return true;
        }
        return false;
    }

    std::uint32_t version() const { return seq.load(std::memory_order_acquire); }
};

} // namespace lie
