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
// Columns are refreshed round-robin under a per-frame time budget; a new revision is published only when
// the merged rectangle set actually changes. tools\collision_check.ps1 diffs EM against the EL mirror.

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
// [POLICY] a fully scanned column is refreshed after this long (doors, breakable walls, moving blocks)
constexpr double kRefreshSeconds = 1.0;
constexpr int kBlock = 2; // [UE] ECollisionResponse::ECR_Block
// [POLICY] verdicts (does this component block Lilac, which mechanism) are re-read this often: doors and
// breakables change their responses at runtime
constexpr double kVerdictSeconds = 2.0;
// [POLICY] probe radius for "is this point inside the component's own shape"
constexpr double kInsideProbeRadius = 1.0;

struct Column {
    double scanned_at = 0.0;
    double z_center = 0.0;
    std::vector<terrain::Interval> solids;
};

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
    long long first = 0, last = -1;
    double z_center = 0.0;
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

void scan(UObject* pawn, double plane_x, long long idx, double now) {
    const double y = static_cast<double>(idx) * facts::kTerrainColumnPitch;
    const double z0 = g.z_center - g.half_h, z1 = g.z_center + g.half_h;
    const auto tops = trace(pawn, plane_x, y, z1, z0);
    const auto bottoms = trace(pawn, plane_x, y, z0, z1);
    Column& col = g.cols[idx];
    auto solids = terrain::resolve_shells(terrain::component_intervals(tops, bottoms, z0, z1), z0, z1, [&](std::uint64_t id, double z) {
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
    if (col.scanned_at == 0.0 || !same(col.solids, solids)) g.dirty = true;
    col.solids = std::move(solids);
    col.scanned_at = now;
    col.z_center = g.z_center;
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
    // Vertical window re-centres only when Lilac drifts by a quarter of it (keeps columns valid while jumping)
    if (std::abs(foot - g.z_center) > half_h * 0.25 || g.cols.empty()) {
        if (!g.cols.empty()) {
            for (auto& [i, c] : g.cols) c.scanned_at = 0.0; // stale vertical range: rescan
            g.ready = false;
        }
        g.z_center = foot;
    }
    g.first = static_cast<long long>(std::floor((lilac.y - half_w) / facts::kTerrainColumnPitch));
    g.last = static_cast<long long>(std::ceil((lilac.y + half_w) / facts::kTerrainColumnPitch));
}

void publish(double plane_x, double focus_y) {
    std::vector<terrain::Column> cols;
    cols.reserve(static_cast<std::size_t>(g.last - g.first + 1));
    for (long long i = g.first; i <= g.last; ++i) {
        auto it = g.cols.find(i);
        cols.push_back({static_cast<double>(i) * facts::kTerrainColumnPitch, it == g.cols.end() ? std::vector<terrain::Interval>{} : it->second.solids});
    }
    auto rects = terrain::merge_columns(cols, facts::kTerrainColumnPitch);
    terrain::clamp_to_capacity(rects, kMaxTerrainRects, focus_y, g.z_center);
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

    // Unscanned columns nearest to Lilac first, then the stalest ones
    const long long centre = static_cast<long long>(std::llround(l.y / facts::kTerrainColumnPitch));
    int done = 0;
    for (;;) {
        long long pick = 0;
        double oldest = 1e300;
        bool found = false;
        for (long long r = 0; centre - r >= g.first || centre + r <= g.last; ++r) {
            for (long long idx : {centre - r, centre + r}) {
                if (idx < g.first || idx > g.last) continue;
                auto it = g.cols.find(idx);
                const double at = it == g.cols.end() ? 0.0 : it->second.scanned_at;
                if (at == 0.0) { pick = idx; found = true; oldest = 0.0; break; }
                if (t0 - at > kRefreshSeconds && at < oldest) { oldest = at; pick = idx; found = true; }
            }
            if (found && oldest == 0.0) break;
        }
        if (!found) break;
        scan(pawn, l.x, pick, clock::now_s());
        ++done;
        if (done >= kMinColumnsPerTick && (clock::now_s() - t0) * 1000.0 > kBudgetMs) break;
    }
    bool complete = true;
    for (long long i = g.first; i <= g.last && complete; ++i) {
        auto it = g.cols.find(i);
        complete = it != g.cols.end() && it->second.scanned_at != 0.0;
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
    std::snprintf(buf, sizeof(buf), "terrain r%u rects=%zu cols=%zu window=[%lld,%lld] half=(%.0f,%.0f) ready=%d shells=%u p99=%.3fms", g.revision,
                  g.published.size(), g.cols.size(), g.first, g.last, g.half_w, g.half_h, g.ready, g.shells, counters().terrain_ms_p99);
    return buf;
}

} // namespace lie::host
