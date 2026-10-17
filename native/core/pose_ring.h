#pragma once

// Lily pose history (FR-MOV-3). The Guest publishes one QPC-stamped sample per frame; the Host samples
// it at (now - delay) with linear interpolation, which removes the phase drift between two
// unsynchronised 60 Hz processes. Only samples of the newest epoch are used, so a rebase never
// interpolates across two different mappings. No extrapolation past the newest sample.

#include "core/protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lie::pose {

inline void publish(PoseRing& ring, const PoseSample& s) {
    const std::uint32_t idx = ring.head.load(std::memory_order_relaxed) + 1u;
    ring.slots[idx % kPoseSlots].store(s);
    ring.head.store(idx, std::memory_order_release);
}

struct Sampled {
    bool valid = false;
    PoseSample pose{};
};

inline Sampled sample(const PoseRing& ring, double t) {
    Sampled r;
    const std::uint32_t head = ring.head.load(std::memory_order_acquire);
    if (head == 0) return r;
    std::array<PoseSample, kPoseSlots> buf{};
    std::size_t n = 0;
    std::uint32_t newest_epoch = 0;
    for (std::uint32_t k = 0; k < kPoseSlots && k < head; ++k) {
        PoseSample s{};
        if (!ring.slots[(head - k) % kPoseSlots].read(s)) continue;
        if (k == 0 || newest_epoch == 0) newest_epoch = s.epoch;
        if (s.epoch == newest_epoch && s.t > 0.0) buf[n++] = s;
    }
    if (n == 0) return r;
    std::sort(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n), [](const PoseSample& a, const PoseSample& b) { return a.t < b.t; });
    r.valid = true;
    if (t >= buf[n - 1].t) {
        r.pose = buf[n - 1];
        return r;
    }
    if (t <= buf[0].t) {
        r.pose = buf[0];
        return r;
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (buf[i].t < t) continue;
        const PoseSample& a = buf[i - 1];
        const PoseSample& b = buf[i];
        const double span = b.t - a.t;
        const double w = span > 1e-9 ? (t - a.t) / span : 1.0;
        auto lerp = [w](double x, double y) { return x + (y - x) * w; };
        r.pose = w < 0.5 ? a : b; // discrete fields from the nearer sample
        r.pose.t = t;
        r.pose.guest_y = lerp(a.guest_y, b.guest_y);
        r.pose.guest_foot = lerp(a.guest_foot, b.guest_foot);
        r.pose.host_y = lerp(a.host_y, b.host_y);
        r.pose.host_foot = lerp(a.host_foot, b.host_foot);
        r.pose.vy = static_cast<float>(lerp(a.vy, b.vy));
        r.pose.vz = static_cast<float>(lerp(a.vz, b.vz));
        return r;
    }
    r.pose = buf[n - 1];
    return r;
}

// Sampling delay: one Guest frame of headroom above its p95 frame time, never less than one Host frame.
inline double sample_delay(double guest_dt_p95, double host_dt) {
    return std::max(1.2 * guest_dt_p95, host_dt);
}

// Rolling p95 over the last N frame intervals (Guest side)
template <std::size_t N>
class FrameTimer {
public:
    void tick(double now) {
        if (last_ > 0.0) {
            hist_[i_++ % N] = now - last_;
            if (count_ < N) ++count_;
        }
        last_ = now;
    }
    double p95() const {
        if (count_ == 0) return 0.0;
        std::vector<double> v(hist_.begin(), hist_.begin() + static_cast<std::ptrdiff_t>(count_));
        const std::size_t k = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(count_))) - 1;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    }

private:
    std::array<double, N> hist_{};
    std::size_t i_ = 0, count_ = 0;
    double last_ = 0.0;
};

} // namespace lie::pose
