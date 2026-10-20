#pragma once

// EM terrain slice -> rectangles (FR-MOV-4), engine-independent so it is unit-tested offline.
//
// Per column the Host runs two multi traces through Lilac's plane: one downward (each hit is a shape's
// TOP face) and one upward (each hit is a shape's BOTTOM face). A hit that starts inside a shape is
// flagged `inside` (the shape contains the trace origin). Per component we walk the faces from top to
// bottom with a depth counter, which yields the solid intervals even for compound components; the
// per-component intervals are then unioned. Adjacent columns with equal intervals merge into one
// rectangle; runs of single-column steps whose tops change linearly become a slope slab + a fill box.

#include "core/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace lie::terrain {

struct Face {
    double z = 0.0;
    std::uint64_t component = 0; // opaque id (pointer value)
    bool inside = false;         // trace started inside this component
    std::uint32_t flags = kTerrainSolid;
};

struct Interval {
    double lo = 0.0, hi = 0.0;
    std::uint32_t flags = kTerrainSolid;
};

// [POLICY] two surfaces closer than this are the same surface (trace hit precision)
inline constexpr double kTol = 0.5;

inline std::vector<Interval> union_intervals(std::vector<Interval> in) {
    std::vector<Interval> out;
    std::sort(in.begin(), in.end(), [](const Interval& a, const Interval& b) { return a.flags != b.flags ? a.flags < b.flags : a.lo < b.lo; });
    for (const Interval& iv : in) {
        if (iv.hi - iv.lo <= 0.0) continue;
        if (!out.empty() && out.back().flags == iv.flags && iv.lo <= out.back().hi + kTol) {
            out.back().hi = std::max(out.back().hi, iv.hi);
        } else {
            out.push_back(iv);
        }
    }
    std::sort(out.begin(), out.end(), [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
    return out;
}

struct ComponentIntervals {
    std::uint64_t component = 0;
    std::vector<Interval> solids; // as reconstructed from the faces: assumes the shape is a closed volume
};

// tops: hits of the downward trace (window top -> bottom); bottoms: hits of the upward trace.
inline std::vector<ComponentIntervals> component_intervals(const std::vector<Face>& tops, const std::vector<Face>& bottoms, double z_min, double z_max) {
    struct Ev {
        double z;
        int delta; // +1 entering going down (top face), -1 leaving (bottom face)
        std::uint32_t flags;
    };
    std::map<std::uint64_t, std::vector<Ev>> per_comp;
    std::map<std::uint64_t, int> start_depth;
    std::map<std::uint64_t, std::uint32_t> comp_flags;
    for (const Face& f : tops) {
        comp_flags[f.component] = f.flags;
        if (f.inside) ++start_depth[f.component];
        else per_comp[f.component].push_back({std::clamp(f.z, z_min, z_max), +1, f.flags});
    }
    for (const Face& f : bottoms) {
        comp_flags[f.component] = f.flags;
        if (!f.inside) per_comp[f.component].push_back({std::clamp(f.z, z_min, z_max), -1, f.flags});
        else per_comp[f.component]; // ensure the component is visited: it extends below the window
    }
    for (const auto& [c, d] : start_depth) per_comp[c];

    std::vector<ComponentIntervals> out;
    for (auto& [comp, evs] : per_comp) {
        std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.z != b.z ? a.z > b.z : a.delta > b.delta; });
        const std::uint32_t flags = comp_flags[comp];
        int depth = start_depth.count(comp) ? std::max(1, start_depth[comp]) : 0;
        double open_hi = z_max;
        ComponentIntervals ci{comp, {}};
        for (const Ev& e : evs) {
            if (e.delta > 0) {
                if (depth++ == 0) open_hi = e.z;
            } else if (depth == 0) {
                ci.solids.push_back({e.z, z_max, flags}); // exit without entry: the shape contains the window top
            } else if (--depth == 0) {
                ci.solids.push_back({e.z, open_hi, flags});
            }
        }
        if (depth > 0) ci.solids.push_back({z_min, open_hi, flags}); // still inside at the window bottom
        out.push_back(std::move(ci));
    }
    return out;
}

// [POLICY] thickness given to an open surface (shell) when it is mirrored: enough to stand on / bump into
inline constexpr double kShellThickness = 8.0;

// The face reconstruction assumes closed volumes. An open surface (triangle-mesh collision, single-sided
// shell) crossing the column yields an interval whose inside is empty space. `inside(component, z)` asks the
// engine whether z lies inside that component's own shape; an interval that is not solid at its middle keeps
// only its real faces (not the ones clamped to the window) as thin slabs, which is all Lilac collides with.
template <class InsideFn>
std::vector<Interval> resolve_shells(const std::vector<ComponentIntervals>& comps, double z_min, double z_max, InsideFn&& inside) {
    std::vector<Interval> all;
    for (const ComponentIntervals& c : comps) {
        for (const Interval& iv : c.solids) {
            if (iv.hi - iv.lo <= 2.0 * kShellThickness || inside(c.component, (iv.lo + iv.hi) * 0.5)) {
                all.push_back(iv);
                continue;
            }
            if (iv.hi < z_max - kTol) all.push_back({iv.hi - kShellThickness, iv.hi, iv.flags});
            if (iv.lo > z_min + kTol) all.push_back({iv.lo, iv.lo + kShellThickness, iv.flags});
        }
    }
    return union_intervals(std::move(all));
}

// The part of `in` inside [lo, hi]. A column is stored per absolute band; union_intervals stitches the
// pieces back together (touching ends merge) whichever scans they came from.
inline std::vector<Interval> clip_intervals(const std::vector<Interval>& in, double lo, double hi) {
    std::vector<Interval> out;
    for (const Interval& iv : in) {
        const double a = std::max(iv.lo, lo), b = std::min(iv.hi, hi);
        if (b > a) out.push_back({a, b, iv.flags});
    }
    return out;
}

// All shapes treated as closed volumes.
inline std::vector<Interval> column_intervals(const std::vector<Face>& tops, const std::vector<Face>& bottoms, double z_min, double z_max) {
    return resolve_shells(component_intervals(tops, bottoms, z_min, z_max), z_min, z_max, [](std::uint64_t, double) { return true; });
}

struct Column {
    double y = 0.0;
    std::vector<Interval> solids;
};

// Merge equally spaced columns (pitch dy, ascending y) into rectangles in EM coordinates.
inline std::vector<TerrainRect> merge_columns(const std::vector<Column>& cols, double dy) {
    struct Run {
        std::size_t first, last;
        Interval iv;
    };
    std::vector<Run> open, closed;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        std::vector<Run> next;
        std::vector<bool> used(open.size(), false);
        for (const Interval& iv : cols[i].solids) {
            bool matched = false;
            for (std::size_t k = 0; k < open.size(); ++k) {
                const Run& r = open[k];
                if (used[k] || r.last + 1 != i || r.iv.flags != iv.flags) continue;
                if (std::abs(r.iv.lo - iv.lo) <= kTol && std::abs(r.iv.hi - iv.hi) <= kTol) {
                    used[k] = true;
                    next.push_back({r.first, i, r.iv});
                    matched = true;
                    break;
                }
            }
            if (!matched) next.push_back({i, i, iv});
        }
        for (std::size_t k = 0; k < open.size(); ++k) {
            if (!used[k]) closed.push_back(open[k]);
        }
        open = std::move(next);
    }
    closed.insert(closed.end(), open.begin(), open.end());
    std::sort(closed.begin(), closed.end(), [](const Run& a, const Run& b) { return a.first != b.first ? a.first < b.first : a.iv.lo < b.iv.lo; });

    std::vector<TerrainRect> out;
    auto emit_box = [&](double y0, double y1, double lo, double hi, std::uint32_t flags) {
        if (hi - lo <= 0.0 || y1 - y0 <= 0.0) return;
        out.push_back({static_cast<float>((y0 + y1) * 0.5), static_cast<float>((lo + hi) * 0.5),
                       static_cast<float>((y1 - y0) * 0.5), static_cast<float>((hi - lo) * 0.5), 0.0f, flags});
    };
    auto col_y0 = [&](std::size_t i) { return cols[i].y - dy * 0.5; };
    auto col_y1 = [&](std::size_t i) { return cols[i].y + dy * 0.5; };

    // Slope chains: consecutive single-column runs whose tops step by a constant amount.
    std::vector<bool> consumed(closed.size(), false);
    for (std::size_t a = 0; a < closed.size(); ++a) {
        if (consumed[a] || closed[a].first != closed[a].last) continue;
        std::vector<std::size_t> chain{a};
        double step = 0.0;
        for (std::size_t b = a + 1; b < closed.size(); ++b) {
            const Run& prev = closed[chain.back()];
            const Run& cur = closed[b];
            if (consumed[b] || cur.first != cur.last || cur.iv.flags != prev.iv.flags) continue;
            if (cur.first > prev.first + 1) break;
            if (cur.first != prev.first + 1) continue;
            const double d = cur.iv.hi - prev.iv.hi;
            const bool overlaps = cur.iv.lo < prev.iv.hi && prev.iv.lo < cur.iv.hi;
            if (!overlaps || std::abs(d) <= kTol || std::abs(d) > dy * 3.0) continue; // steeper than ~72 deg: a wall
            if (chain.size() >= 2 && std::abs(d - step) > kTol * 2.0) continue;
            step = d;
            chain.push_back(b);
        }
        if (chain.size() < 3) continue;
        for (std::size_t c : chain) consumed[c] = true;
        const Run& f = closed[chain.front()];
        const Run& l = closed[chain.back()];
        double floor_hi = 1e30, floor_lo = -1e30, min_thick = 1e30;
        for (std::size_t c : chain) {
            floor_hi = std::min(floor_hi, closed[c].iv.hi);
            floor_lo = std::max(floor_lo, closed[c].iv.lo);
            min_thick = std::min(min_thick, closed[c].iv.hi - closed[c].iv.lo);
        }
        const double y0 = col_y0(f.first), y1 = col_y1(l.last);
        emit_box(y0, y1, floor_lo, floor_hi, f.iv.flags);
        // top line through column centres, extended half a column on both ends
        const double slope = step / dy;
        const double ya = y0, za = f.iv.hi - slope * dy * 0.5;
        const double yb = y1, zb = l.iv.hi + slope * dy * 0.5;
        const double len = std::hypot(yb - ya, zb - za);
        const double ang = std::atan2(zb - za, yb - ya);
        const double rise = std::abs(zb - za);
        // the slab fills the wedge above the fill box (rise), but never reaches below the thinnest column:
        // a floating thin slope (plank, sloped one-way platform) keeps its real thickness
        const double half_t = std::max(1.0, std::min(rise * std::cos(ang) * 0.5 + 1.0, min_thick * std::cos(ang) * 0.5));
        const double nx = -std::sin(ang), nz = std::cos(ang); // upward normal of the top line
        TerrainRect slab{};
        slab.y = static_cast<float>((ya + yb) * 0.5 - nx * half_t);
        slab.z = static_cast<float>((za + zb) * 0.5 - nz * half_t);
        slab.hy = static_cast<float>(len * 0.5);
        slab.hz = static_cast<float>(half_t);
        slab.roll_deg = static_cast<float>(ang * 180.0 / 3.14159265358979323846);
        slab.flags = f.iv.flags;
        out.push_back(slab);
    }
    for (std::size_t k = 0; k < closed.size(); ++k) {
        if (!consumed[k]) emit_box(col_y0(closed[k].first), col_y1(closed[k].last), closed[k].iv.lo, closed[k].iv.hi, closed[k].iv.flags);
    }
    return out;
}

// Keep at most `limit` rectangles, nearest to `focus_y` first (the pool has a fixed size).
inline void clamp_to_capacity(std::vector<TerrainRect>& rects, std::size_t limit, double focus_y, double focus_z) {
    if (rects.size() <= limit) return;
    auto dist = [&](const TerrainRect& r) {
        const double dy = std::max(0.0, std::abs(r.y - focus_y) - r.hy);
        const double dz = std::max(0.0, std::abs(r.z - focus_z) - r.hz);
        return dy * dy + dz * dz;
    };
    std::nth_element(rects.begin(), rects.begin() + static_cast<std::ptrdiff_t>(limit), rects.end(),
                     [&](const TerrainRect& a, const TerrainRect& b) { return dist(a) < dist(b); });
    rects.resize(limit);
}

} // namespace lie::terrain
