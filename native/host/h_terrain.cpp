// EM terrain scanner (FR-MOV-4): slices EM collision in Lilac's plane into rectangles for the Guest.
//
// Columns on an absolute grid (pitch facts::kTerrainColumnPitch) cover a window around Lilac sized from the
// EM camera's visible extent. Each column is two LineTraceMultiForObjects (down: top faces, up: bottom faces);
// verified live 2026-10-17: every shape on the ray returns one entry hit, origins inside a shape come back with
// bStartPenetrating. A hit counts as terrain only if Lilac and the component BLOCK each other (both responses,
// read at runtime from the live capsule, the same rule a capsule sweep applies), which removes triggers,
// sound/design volumes and characters. Behaviour (one-way, moving, breakable) comes from the single registry
// core/terrain_kinds.h. Every reconstructed interval is checked against its own component's shape
// (K2_SphereOverlapComponent): open surfaces keep only their faces (terrain::resolve_shells).
// Coverage lives on an absolute 2D grid: columns (Y) x bands (Z, facts::kTerrainBandHeight). The window only
// decides which cells are wanted; moving it adds / drops whole cells and never invalidates the rest, so what
// the Guest already has stays valid while new cells arrive. Invariant: a cell enters the window half_h away
// from Lilac's feet and the cell nearest to her is scanned first on the next tick, so falls / dashes cannot
// outrun the mirror. Cells are scanned nearest-first under a per-frame time budget (one trace pair covers a
// contiguous run of wanted bands) and refreshed stalest-first; a new revision is published only when the
// merged rectangle set actually changes. tools\collision_check.ps1 diffs EM against the EL mirror.

#include "host/host.h"

#include "core/facts.h"
#include "core/terrain_kinds.h"
#include "core/terrain_merge.h"
#include "rt/clock.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lie::host {

namespace {

using ue::Call;
using ue::V3;

// [POLICY] mirror 1.5x the visible extent so dashes / falls never outrun the scan
constexpr double kWindowScreens = 1.5;
// [POLICY] scanner time budget per frame (Oracle target: P99 <= 0.5 ms)
constexpr double kBudgetMs = 0.4;
constexpr int kMinColumnsPerTick = 2;
// [POLICY] a scanned cell is refreshed after this long (doors, breakable walls, moving blocks)
constexpr double kRefreshSeconds = 1.0;
constexpr int kBlock = 2; // [UE] ECollisionResponse::ECR_Block
// [POLICY] verdicts (does this component block Lilac, which mechanism) are re-read this often: doors and
// breakables change their responses at runtime
constexpr double kVerdictSeconds = 2.0;
// [POLICY] probe radius for "is this point inside the component's own shape"
constexpr double kInsideProbeRadius = 1.0;

constexpr double kBand = facts::kTerrainBandHeight;
// [POLICY] bands kept beyond the window on each side, so hovering at a band edge never rescans
constexpr long long kBandKeep = 1;

struct Cell {
    double scanned_at = 0.0;
    std::vector<terrain::Interval> solids; // clipped to the band
};
using Column = std::map<long long, Cell>; // band index -> cell

struct Verdict {
    ue::Weak comp; // a recycled address is a different component
    int flags = 0; // 0 = not terrain, otherwise TerrainFlags
};

struct State {
    std::map<long long, Column> cols;
    std::unordered_map<const UObject*, Verdict> verdict;
    double verdict_at = 0.0;
    std::vector<std::pair<UClass*, const terrain::MechanismRule*>> rules; // registry order, resolved classes
    int lilac_type = -1;
    int lilac_resp[32]{};
    bool have_profile = false;
    double half_w = 0.0, half_h = 0.0;
    long long first = 0, last = -1;   // wanted columns
    long long b_lo = 0, b_hi = -1;    // wanted bands
    double foot = 0.0;
    bool dirty = false;
    bool ready = false;
    std::vector<TerrainRect> published;
    std::uint32_t revision = 0;
    double cost_ms[128]{};
    std::size_t cost_i = 0;
    std::unique_ptr<TerrainSnapshot> snap = std::make_unique<TerrainSnapshot>();
    UClass* character = nullptr;
    std::uint32_t shells = 0; // intervals resolved as open surfaces (status)
} g;

void read_profile(UObject* pawn) {
    UObject* cap = ue::obj(pawn, L"CapsuleComponent");
    if (!cap) return;
    g.lilac_type = static_cast<int>(ue::call_num(cap, L"GetCollisionObjectType", -1.0));
    for (int ch = 0; ch < 32; ++ch) {
        Call r(cap, L"GetCollisionResponseToChannel");
        g.lilac_resp[ch] = r.i(ch).go() ? static_cast<int>(r.ri()) : 0;
    }
    g.have_profile = g.lilac_type >= 0;
    g.character = ue::find_class(L"/Script/Engine.Character");
    g.rules.clear();
    for (const terrain::MechanismRule& r : terrain::kMechanisms) {
        UClass* cls = ue::find_class(r.em_class);
        if (!cls) log::error(L"terrain: mechanism class %s not found (rule inactive)", r.em_class);
        g.rules.emplace_back(cls, &r);
    }
    log::info(L"terrain: Lilac object type %d, %zu mechanism rules", g.lilac_type, g.rules.size());
}

// Registry lookup: the first rule whose class the owner IsA, else plain static geometry
std::uint32_t mechanism_flags(UObject* owner) {
    for (const auto& [cls, rule] : g.rules) {
        if (ue::is_a(owner, cls)) return rule->flags;
    }
    return terrain::kStaticFlags;
}

// 0 = not terrain, otherwise TerrainFlags
int classify(UObject* comp) {
    if (auto it = g.verdict.find(comp); it != g.verdict.end() && it->second.comp.get() == comp) return it->second.flags;
    int v = 0;
    const int t = static_cast<int>(ue::call_num(comp, L"GetCollisionObjectType", -1.0));
    Call back(comp, L"GetCollisionResponseToChannel");
    const bool blocks = t >= 0 && t < 32 && g.lilac_resp[t] == kBlock && back.i(g.lilac_type).go() && back.ri() == kBlock;
    UObject* owner = ue::call_obj(comp, L"GetOwner");
    if (blocks && !ue::is_a(owner, g.character)) v = static_cast<int>(mechanism_flags(owner));
    g.verdict.insert_or_assign(comp, Verdict{ue::Weak(comp), v});
    return v;
}

// Is (x, y, z) inside the component's own collision shape (not just near its surface)?
bool inside(UObject* comp, double x, double y, double z) {
    Call c(comp, L"K2_SphereOverlapComponent");
    return !c || (c.v({x, y, z}).f(kInsideProbeRadius).b(false).b(false).b(false).go() && c.rb()); // no probe: assume solid
}

std::vector<terrain::Face> trace(UObject* pawn, double x, double y, double z0, double z1) {
    std::vector<terrain::Face> faces;
    static std::vector<std::uint8_t> types = [] {
        std::vector<std::uint8_t> t(32);
        for (int i = 0; i < 32; ++i) t[i] = static_cast<std::uint8_t>(i); // EObjectTypeQuery: everything, filtered by response below
        return t;
    }();
    Call c(ue::system_library(), L"LineTraceMultiForObjects");
    if (!c) return faces;
    std::vector<UObject*> ignore{pawn};
    c.o(pawn).v({x, y, z0}).v({x, y, z1}).arr(types).b(false).arr(ignore).i(0);
    ue::FProperty* hits_prop = c.param(L"OutHits");
    if (!c.go() || !hits_prop) return faces;
    auto* arr = static_cast<ue::RawArray*>(c.slot(hits_prop));
    ue::FProperty* inner = ue::array_inner(hits_prop);
    if (!arr || !arr->data || !inner) return faces;
    static ue::FProperty* f_point = nullptr;
    static ue::FProperty* f_pen = nullptr;
    static ue::FProperty* f_comp = nullptr;
    if (!f_point) {
        f_point = ue::member_of(inner, L"ImpactPoint");
        f_pen = ue::member_of(inner, L"bStartPenetrating");
        f_comp = ue::member_of(inner, L"Component");
    }
    if (!f_point || !f_pen || !f_comp) return faces;
    const auto stride = static_cast<std::size_t>(inner->GetSize());
    for (std::int32_t i = 0; i < arr->num; ++i) {
        auto* h = static_cast<std::uint8_t*>(arr->data) + stride * static_cast<std::size_t>(i);
        UObject* comp = ue::weak_get(h + f_comp->GetOffset_Internal());
        if (!comp) continue;
        const int flags = classify(comp);
        if (!flags) continue;
        terrain::Face f;
        f.z = ue::read_vec(h + f_point->GetOffset_Internal(), f_point->GetSize()).z;
        f.component = reinterpret_cast<std::uintptr_t>(comp);
        f.inside = ue::read_num(f_pen, h + f_pen->GetOffset_Internal()) != 0.0;
        f.flags = static_cast<std::uint32_t>(flags);
        faces.push_back(f);
    }
    return faces;
}

// Scans bands [b0, b1] of column idx with one trace pair and stores the result per band.
void scan(UObject* pawn, double plane_x, long long idx, long long b0, long long b1, double now) {
    const double y = static_cast<double>(idx) * facts::kTerrainColumnPitch;
    const double z0 = static_cast<double>(b0) * kBand, z1 = static_cast<double>(b1 + 1) * kBand;
    const auto tops = trace(pawn, plane_x, y, z1, z0);
    const auto bottoms = trace(pawn, plane_x, y, z0, z1);
    const auto solids = terrain::resolve_shells(terrain::component_intervals(tops, bottoms, z0, z1), z0, z1, [&](std::uint64_t id, double z) {
        const bool solid = inside(reinterpret_cast<UObject*>(id), plane_x, y, z);
        g.shells += solid ? 0u : 1u;
        return solid;
    });
    auto same = [](const std::vector<terrain::Interval>& a, const std::vector<terrain::Interval>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i].lo - b[i].lo) > terrain::kTol || std::abs(a[i].hi - b[i].hi) > terrain::kTol || a[i].flags != b[i].flags) return false;
        }
        return true;
    };
    Column& col = g.cols[idx];
    for (long long b = b0; b <= b1; ++b) {
        Cell& c = col[b];
        auto piece = terrain::clip_intervals(solids, static_cast<double>(b) * kBand, static_cast<double>(b + 1) * kBand);
        if (c.scanned_at == 0.0 || !same(c.solids, piece)) g.dirty = true;
        c.solids = std::move(piece);
        c.scanned_at = now;
    }
}

void update_window(UObject* pc, const V3& lilac, double foot) {
    UObject* cm = ue::obj(pc, L"PlayerCameraManager");
    Call cl(cm, L"GetCameraLocation");
    Call cf(cm, L"GetFOVAngle");
    double half_w = 2000.0, half_h = 1200.0; // fallback before the camera exists
    if (cl.go() && cf.go()) {
        const double depth = std::abs(lilac.x - cl.rv().x);
        const double vis_half_w = depth * std::tan(cf.rf() * 0.5 * 3.14159265358979323846 / 180.0);
        half_w = kWindowScreens * vis_half_w;
        half_h = kWindowScreens * vis_half_w * 9.0 / 16.0; // EM renders 16:9
    }
    g.half_w = half_w;
    g.half_h = half_h;
    g.foot = foot;
    g.b_lo = static_cast<long long>(std::floor((foot - half_h) / kBand));
    g.b_hi = static_cast<long long>(std::floor((foot + half_h) / kBand));
    g.first = static_cast<long long>(std::floor((lilac.y - half_w) / facts::kTerrainColumnPitch));
    g.last = static_cast<long long>(std::ceil((lilac.y + half_w) / facts::kTerrainColumnPitch));
}

void publish(double plane_x, double focus_y) {
    std::vector<terrain::Column> cols;
    cols.reserve(static_cast<std::size_t>(g.last - g.first + 1));
    for (long long i = g.first; i <= g.last; ++i) {
        std::vector<terrain::Interval> pieces;
        if (auto it = g.cols.find(i); it != g.cols.end()) {
            for (const auto& [b, c] : it->second) pieces.insert(pieces.end(), c.solids.begin(), c.solids.end());
        }
        cols.push_back({static_cast<double>(i) * facts::kTerrainColumnPitch, terrain::union_intervals(std::move(pieces))});
    }
    auto rects = terrain::merge_columns(cols, facts::kTerrainColumnPitch);
    terrain::clamp_to_capacity(rects, kMaxTerrainRects, focus_y, g.foot);
    std::sort(rects.begin(), rects.end(), [](const TerrainRect& a, const TerrainRect& b) { return a.y != b.y ? a.y < b.y : a.z < b.z; });
    if (rects.size() == g.published.size() && std::equal(rects.begin(), rects.end(), g.published.begin(), [](const TerrainRect& a, const TerrainRect& b) {
            return a.y == b.y && a.z == b.z && a.hy == b.hy && a.hz == b.hz && a.roll_deg == b.roll_deg && a.flags == b.flags;
        })) {
        return;
    }
    g.published = rects;
    ++g.revision;
    TerrainSnapshot& s = *g.snap;
    s.revision = g.revision;
    s.count = static_cast<std::uint32_t>(rects.size());
    s.host_x = plane_x;
    std::copy(rects.begin(), rects.end(), s.rects);
    if (World* w = shm::world()) w->terrain.store(s);
    counters().terrain_revision = g.revision;
    counters().terrain_rects = s.count;
}

} // namespace

void terrain_tick(UObject* pc, UObject* pawn) {
    const double t0 = clock::now_s();
    if (!g.have_profile) read_profile(pawn);
    if (!g.have_profile) return;
    if (t0 - g.verdict_at > kVerdictSeconds) {
        g.verdict.clear();
        g.verdict_at = t0;
    }
    const V3 l = ue::actor_location(pawn);
    const double hh = ue::num(ue::obj(pawn, L"CapsuleComponent"), L"CapsuleHalfHeight");
    update_window(pc, l, l.z - hh);
    std::erase_if(g.cols, [](const std::pair<const long long, Column>& kv) { return kv.first < g.first - 8 || kv.first > g.last + 8; });
    for (auto& [i, col] : g.cols) {
        if (std::erase_if(col, [](const std::pair<const long long, Cell>& kv) { return kv.first < g.b_lo - kBandKeep || kv.first > g.b_hi + kBandKeep; })) g.dirty = true;
    }

    auto age = [](const Column* col, long long b) {
        if (!col) return 0.0;
        auto it = col->find(b);
        return it == col->end() ? 0.0 : it->second.scanned_at;
    };
    auto wanted = [&](double at) { return at == 0.0 || t0 - at > kRefreshSeconds; };
    // Unscanned cells nearest to Lilac's feet first, then the stalest ones
    int done = 0;
    for (;;) {
        long long pick_i = 0, pick_b = 0;
        bool found = false;
        std::pair<int, double> best{2, 0.0}; // (0 unscanned: squared distance | 1 stale: scan time), lowest wins
        for (long long i = g.first; i <= g.last; ++i) {
            auto it = g.cols.find(i);
            const Column* col = it == g.cols.end() ? nullptr : &it->second;
            const double dy = static_cast<double>(i) * facts::kTerrainColumnPitch - l.y;
            for (long long b = g.b_lo; b <= g.b_hi; ++b) {
                const double at = age(col, b);
                if (!wanted(at)) continue;
                const double dz = std::max({0.0, static_cast<double>(b) * kBand - g.foot, g.foot - static_cast<double>(b + 1) * kBand});
                const std::pair<int, double> rank = at == 0.0 ? std::pair{0, dy * dy + dz * dz} : std::pair{1, at};
                if (rank < best) {
                    best = rank;
                    pick_i = i;
                    pick_b = b;
                    found = true;
                }
            }
        }
        if (!found) break;
        // one trace pair covers the contiguous run of wanted bands around the pick
        auto it = g.cols.find(pick_i);
        const Column* col = it == g.cols.end() ? nullptr : &it->second;
        long long b0 = pick_b, b1 = pick_b;
        while (b0 > g.b_lo && wanted(age(col, b0 - 1))) --b0;
        while (b1 < g.b_hi && wanted(age(col, b1 + 1))) ++b1;
        scan(pawn, l.x, pick_i, b0, b1, clock::now_s());
        ++done;
        if (done >= kMinColumnsPerTick && (clock::now_s() - t0) * 1000.0 > kBudgetMs) break;
    }
    bool complete = true;
    for (long long i = g.first; i <= g.last && complete; ++i) {
        auto it = g.cols.find(i);
        const Column* col = it == g.cols.end() ? nullptr : &it->second;
        for (long long b = g.b_lo; b <= g.b_hi && complete; ++b) complete = age(col, b) != 0.0;
    }
    if (complete && !g.ready) {
        g.ready = true;
        g.dirty = true;
    }
    if (g.ready && g.dirty) {
        publish(l.x, l.y);
        g.dirty = false;
    }
    g.cost_ms[g.cost_i++ % std::size(g.cost_ms)] = (clock::now_s() - t0) * 1000.0;
    if (g.cost_i % 32 == 0) {
        std::vector<double> v(std::begin(g.cost_ms), std::begin(g.cost_ms) + std::min<std::size_t>(g.cost_i, std::size(g.cost_ms)));
        std::sort(v.begin(), v.end());
        counters().terrain_ms_p99 = v[static_cast<std::size_t>(0.99 * static_cast<double>(v.size() - 1))];
    }
}

void terrain_reset() {
    g.cols.clear();
    g.verdict.clear();
    g.ready = false;
    g.dirty = true;
    g.have_profile = false;
}

bool terrain_ready() { return g.ready && g.revision > 0; }

std::string terrain_status() {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "terrain r%u rects=%zu cols=%zu window=[%lld,%lld]x[%lld,%lld] half=(%.0f,%.0f) ready=%d shells=%u p99=%.3fms",
                  g.revision, g.published.size(), g.cols.size(), g.first, g.last, g.b_lo, g.b_hi, g.half_w, g.half_h, g.ready, g.shells, counters().terrain_ms_p99);
    return buf;
}

} // namespace lie::host
