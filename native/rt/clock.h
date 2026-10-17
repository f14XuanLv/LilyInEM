#pragma once

// Clocks comparable across the two processes (QPC is system-wide on Windows).

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lie::clock {

inline double now_s() noexcept {
    static const double inv = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}

inline std::uint64_t now_ms() noexcept { return static_cast<std::uint64_t>(now_s() * 1000.0); }

// Simple rate limiter for low-frequency game-thread work
class Every {
public:
    explicit Every(double period_s) : period_(period_s) {}
    bool due(double now) {
        if (now < next_) return false;
        next_ = now + period_;
        return true;
    }
    void reset() { next_ = 0.0; }

private:
    double period_;
    double next_ = 0.0;
};

} // namespace lie::clock
