// EM terrain slice -> pool of hidden Static TriggerBoxes in Lily's plane (FR-MOV-4).
// Static mobility matters: [IDA] UAutoClimbComponent / UWallGrabComponent tick traces only query Static
// bodies, so with a Static pool EL's own ledge climb, wall grab, falling, landing, slopes and ceilings all
// happen natively. [UE 4.26 SceneComponent.cpp] a Static component refuses to move once registered, so a box
// is switched to Movable, moved, and switched back (SetMobility re-registers the physics body).
// Boxes are keyed by their quantised placement: a new terrain revision only re-places boxes that changed.

#include "guest/guest.h"

#include "core/facts.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace lie::guest {

namespace {

using ue::Call;

// [POLICY] depth of the mirror slabs along X: anything comfortably larger than Lily's capsule and the
// AutoClimb probe box; boxes are invisible and the studio has no other collision, so depth is free.
constexpr double kSlabHalfDepth = 1000.0;
// [POLICY] horizontal overlap that closes float seams between neighbouring boxes (no vertical padding:
// it would lift Lily's feet off Lilac's surface).
constexpr double kSeamPad = 0.5;

using Key = std::tuple<long long, long long, long long, long long, long long, std::uint32_t>;

Key key_of(double y, double z, double hy, double hz, double roll, std::uint32_t flags) {
    auto q = [](double v) { return std::llround(v * 10.0); };
    return {q(y), q(z), q(hy), q(hz), q(roll), flags};
}

struct Box {
    ue::Weak actor;
    bool active = false;
    Key key{};
};

std::vector<Box> g_pool;
std::uint32_t g_seen_version = 0, g_seen_epoch = 0, g_applied_revision = 0;
std::unique_ptr<TerrainSnapshot> g_snap = std::make_unique<TerrainSnapshot>();

UObject* spawn_box(UObject* world, const ue::V3& at) {
    UClass* cls = ue::find_class(L"/Script/Engine.TriggerBox");
    if (!cls || !world) return nullptr;
    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(cls).xf(at).i(1 /*AlwaysSpawn*/).o(nullptr);
    if (!begin.go() || !begin.ro()) return nullptr;
    UObject* actor = begin.ro();
    stage_register_owned(actor); // before FinishSpawning: BeginPlay must not isolate it
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(actor).xf(at).go();
    Call(actor, L"SetActorHiddenInGame").b(true).go();
    if (UObject* c = ue::obj(actor, L"CollisionComponent")) {
        Call(c, L"SetCollisionProfileName").n(L"BlockAll").b(true).go();
        Call(c, L"SetCollisionObjectType").i(facts::el::kChannelWorldStatic).go();
        Call(c, L"SetGenerateOverlapEvents").b(true).go();
        Call(c, L"SetCollisionEnabled").i(facts::el::kCollisionQueryAndPhysics).go();
    }
    return actor;
}

void place(UObject* actor, const ue::V3& loc, double roll, const ue::V3& extent) {
    UObject* c = ue::obj(actor, L"CollisionComponent");
    if (!c) return;
    Call(c, L"SetMobility").i(facts::el::kMobilityMovable).go();
    Call(actor, L"K2_SetActorLocationAndRotation").v(loc).r({0.0, 0.0, roll}).b(false).skip().b(true).go();
    Call(c, L"SetBoxExtent").v(extent).b(true).go();
    Call(c, L"SetMobility").i(facts::el::kMobilityStatic).go();
    Call(actor, L"SetActorEnableCollision").b(true).go();
}

void park(Box& b) {
    if (UObject* a = b.actor.get()) Call(a, L"SetActorEnableCollision").b(false).go();
    b.active = false;
}

} // namespace

void terrain_tick(UObject* world, const FrameMap& map, double plane_x, bool force) {
    World* w = shm::world();
    if (!w || !map.valid()) return;
    const std::uint32_t version = w->terrain.version();
    if (!force && version == g_seen_version && map.epoch == g_seen_epoch) return;
    if (!w->terrain.read(*g_snap)) return;
    g_seen_version = version;
    g_seen_epoch = map.epoch;

    struct Want {
        Key key;
        ue::V3 loc, ext;
        double roll;
    };
    std::vector<Want> wants;
    wants.reserve(g_snap->count);
    for (std::uint32_t i = 0; i < g_snap->count && i < kMaxTerrainRects; ++i) {
        const TerrainRect& r = g_snap->rects[i];
        // [UE] FRotationMatrix: roll +a turns +Y towards -Z, so a surface rising at +a needs roll = -a
        const double roll = -static_cast<double>(r.roll_deg);
        const double hy = r.hy + (r.roll_deg == 0.0f ? kSeamPad : 0.0);
        const ue::V3 loc{plane_x, map.guest_y(r.y), map.guest_z(r.z)};
        wants.push_back({key_of(loc.y, loc.z, hy, r.hz, roll, r.flags), loc, {kSlabHalfDepth, hy, r.hz}, roll});
    }

    std::map<Key, std::size_t> live;
    for (std::size_t i = 0; i < g_pool.size(); ++i) {
        if (g_pool[i].active && g_pool[i].actor.get()) live.emplace(g_pool[i].key, i);
    }
    std::vector<bool> keep(g_pool.size(), false);
    std::vector<const Want*> todo;
    for (const Want& want : wants) {
        if (auto it = live.find(want.key); it != live.end() && !keep[it->second]) keep[it->second] = true;
        else todo.push_back(&want);
    }
    std::size_t moved = 0;
    for (std::size_t i = 0; i < g_pool.size() && !todo.empty(); ++i) {
        if (keep[i]) continue;
        Box& b = g_pool[i];
        UObject* a = b.actor.get();
        if (!a) continue;
        const Want* want = todo.back();
        todo.pop_back();
        place(a, want->loc, want->roll, want->ext);
        b.key = want->key;
        b.active = keep[i] = true;
        ++moved;
    }
    for (const Want* want : todo) {
        if (g_pool.size() >= kMaxTerrainRects) break;
        UObject* a = spawn_box(world, want->loc);
        if (!a) break;
        place(a, want->loc, want->roll, want->ext);
        g_pool.push_back({ue::Weak(a), true, want->key});
        keep.push_back(true);
        ++moved;
    }
    std::uint32_t active = 0;
    for (std::size_t i = 0; i < g_pool.size(); ++i) {
        if (!keep[i] && g_pool[i].active) park(g_pool[i]);
        active += g_pool[i].active ? 1u : 0u;
    }
    g_applied_revision = g_snap->revision;
    GuestCounters& k = counters();
    k.terrain_revision_applied = g_applied_revision;
    k.terrain_boxes = active;
    if (moved) log::info(L"terrain r%u: %u boxes (%zu placed)", g_applied_revision, active, moved);
}

TerrainStatus terrain_status() {
    std::uint32_t active = 0;
    for (const Box& b : g_pool) active += b.active ? 1u : 0u;
    return {g_applied_revision, active};
}

void terrain_reset() {
    for (Box& b : g_pool) park(b);
    g_seen_version = g_seen_epoch = g_applied_revision = 0;
}

} // namespace lie::guest
