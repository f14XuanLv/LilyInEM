// EM terrain slice -> pools of hidden Static actors in Lily's plane (FR-MOV-4).
// Static mobility matters: [IDA] UAutoClimbComponent / UWallGrabComponent tick traces only query Static
// bodies, so with a Static pool EL's own ledge climb, wall grab, falling, landing, slopes and ceilings all
// happen natively. [UE 4.26 SceneComponent.cpp] a Static component refuses to move once registered, so an
// actor is switched to Movable, moved, and switched back (SetMobility re-registers the physics body).
// Which stand-in a rect gets is decided by the registry (core/terrain_kinds.h, terrain::mirror_of): Box rects
// become TriggerBoxes; OneWay rects become EL's own BP_OneWayPlatform_C, so jumping through from below and
// UCA_FallThrough drop-through are EL's native rules, not a re-implementation.
// Actors are keyed by their quantised placement: a new terrain revision only re-places actors that changed.

#include "guest/guest.h"

#include "core/facts.h"
#include "core/terrain_kinds.h"
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
// [POLICY] how far the one-way pass-through trigger reaches below the platform: more than Lily's rise per
// frame at jump speed, so her head enters the trigger before it can touch the platform. The trigger stops at
// mid-thickness ([CUE4P] like the asset's own Box), so a character standing on top never overlaps it.
constexpr double kOneWayTriggerBelow = 40.0;

enum Kind { kSolid = 0, kOneWay = 1, kKinds };

using Key = std::tuple<long long, long long, long long, long long, long long, std::uint32_t>;

Key key_of(double y, double z, double hy, double hz, double roll, std::uint32_t flags) {
    auto q = [](double v) { return std::llround(v * 10.0); };
    return {q(y), q(z), q(hy), q(hz), q(roll), flags};
}

struct Slot {
    ue::Weak actor;
    bool active = false;
    Key key{};
};

struct Want {
    Key key;
    ue::V3 loc, ext;
    double roll;
};

std::vector<Slot> g_pool[kKinds];
std::uint32_t g_seen_version = 0, g_seen_epoch = 0, g_applied_revision = 0;
std::unique_ptr<TerrainSnapshot> g_snap = std::make_unique<TerrainSnapshot>();
bool g_one_way_missing = false;

UObject* begin_spawn(UObject* world, UClass* cls, const ue::V3& at) {
    if (!cls || !world) return nullptr;
    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(cls).xf(at).i(1 /*AlwaysSpawn*/).o(nullptr);
    if (!begin.go() || !begin.ro()) return nullptr;
    UObject* actor = begin.ro();
    stage_register_owned(actor); // before FinishSpawning: BeginPlay must not isolate it
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(actor).xf(at).go();
    Call(actor, L"SetActorHiddenInGame").b(true).go();
    return actor;
}

UObject* spawn_solid(UObject* world, const ue::V3& at) {
    UObject* actor = begin_spawn(world, ue::find_class(L"/Script/Engine.TriggerBox"), at);
    if (UObject* c = actor ? ue::obj(actor, L"CollisionComponent") : nullptr) {
        Call(c, L"SetCollisionProfileName").n(L"BlockAll").b(true).go();
        Call(c, L"SetCollisionObjectType").i(facts::el::kChannelWorldStatic).go();
        Call(c, L"SetGenerateOverlapEvents").b(true).go();
        Call(c, L"SetCollisionEnabled").i(facts::el::kCollisionQueryAndPhysics).go();
    }
    return actor;
}

UObject* spawn_one_way(UObject* world, const ue::V3& at) {
    UClass* cls = ue::load_class(facts::el::kOneWayPlatformClass);
    if (!cls) {
        if (!g_one_way_missing) log::error(L"one-way class not loadable: %s (one-way rects mirrored as solid)", facts::el::kOneWayPlatformClass);
        g_one_way_missing = true;
        return nullptr;
    }
    UObject* actor = begin_spawn(world, cls, at);
    // the trigger is re-shaped on every placement; keep it Movable so it may move under a Static root
    if (UObject* box = actor ? ue::obj(actor, L"Box") : nullptr) Call(box, L"SetMobility").i(facts::el::kMobilityMovable).go();
    return actor;
}

UObject* root_of(UObject* actor, Kind kind) { return ue::obj(actor, kind == kSolid ? L"CollisionComponent" : L"StaticMeshComponent"); }

void place(UObject* actor, Kind kind, const Want& w) {
    UObject* root = root_of(actor, kind);
    if (!root) return;
    Call(root, L"SetMobility").i(facts::el::kMobilityMovable).go();
    if (kind == kSolid) {
        Call(actor, L"K2_SetActorLocationAndRotation").v(w.loc).r({0.0, 0.0, w.roll}).b(false).skip().b(true).go();
        Call(root, L"SetBoxExtent").v(w.ext).b(true).go();
    } else {
        // Cube_BottomOrigin: the actor origin is the bottom face, scale = extent / mesh half size.
        // [UE] FRotationMatrix: roll a maps +Z to (0, sin a, cos a)
        const double a = w.roll * 3.14159265358979323846 / 180.0;
        const ue::V3 up{0.0, std::sin(a), std::cos(a)};
        const ue::V3 bottom{w.loc.x, w.loc.y - up.y * w.ext.z, w.loc.z - up.z * w.ext.z};
        const double h = facts::el::kOneWayMeshHalf;
        const ue::V3 scale{w.ext.x / h, w.ext.y / h, w.ext.z / h};
        Call(actor, L"SetActorScale3D").v(scale).go();
        Call(actor, L"K2_SetActorLocationAndRotation").v(bottom).r({0.0, 0.0, w.roll}).b(false).skip().b(true).go();
        // trigger in the root's unscaled space: from kOneWayTriggerBelow under the bottom face to mid-thickness
        if (UObject* box = ue::obj(actor, L"Box")) {
            const double lo = -kOneWayTriggerBelow / scale.z, hi = w.ext.z / scale.z;
            Call(box, L"K2_SetRelativeLocation").v({0.0, 0.0, (lo + hi) * 0.5}).b(false).skip().b(true).go();
            Call(box, L"SetBoxExtent").v({h, h, (hi - lo) * 0.5}).b(true).go();
        }
    }
    Call(root, L"SetMobility").i(facts::el::kMobilityStatic).go();
    Call(actor, L"SetActorEnableCollision").b(true).go();
}

void park(Slot& s) {
    if (UObject* a = s.actor.get()) Call(a, L"SetActorEnableCollision").b(false).go();
    s.active = false;
}

// Re-places only the actors whose key changed; returns how many were placed.
std::size_t sync_pool(UObject* world, Kind kind, const std::vector<Want>& wants) {
    std::vector<Slot>& pool = g_pool[kind];
    std::map<Key, std::size_t> live;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].active && pool[i].actor.get()) live.emplace(pool[i].key, i);
    }
    std::vector<bool> keep(pool.size(), false);
    std::vector<const Want*> todo;
    for (const Want& want : wants) {
        if (auto it = live.find(want.key); it != live.end() && !keep[it->second]) keep[it->second] = true;
        else todo.push_back(&want);
    }
    std::size_t moved = 0;
    for (std::size_t i = 0; i < pool.size() && !todo.empty(); ++i) {
        if (keep[i]) continue;
        UObject* a = pool[i].actor.get();
        if (!a) continue;
        const Want* want = todo.back();
        todo.pop_back();
        place(a, kind, *want);
        pool[i].key = want->key;
        pool[i].active = keep[i] = true;
        ++moved;
    }
    for (const Want* want : todo) {
        if (pool.size() >= kMaxTerrainRects) break;
        UObject* a = kind == kSolid ? spawn_solid(world, want->loc) : spawn_one_way(world, want->loc);
        if (!a) break;
        place(a, kind, *want);
        pool.push_back({ue::Weak(a), true, want->key});
        keep.push_back(true);
        ++moved;
    }
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (!keep[i] && pool[i].active) park(pool[i]);
    }
    return moved;
}

std::uint32_t active_count() {
    std::uint32_t n = 0;
    for (const auto& pool : g_pool) {
        for (const Slot& s : pool) n += s.active ? 1u : 0u;
    }
    return n;
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

    std::vector<Want> wants[kKinds];
    for (std::uint32_t i = 0; i < g_snap->count && i < kMaxTerrainRects; ++i) {
        const TerrainRect& r = g_snap->rects[i];
        const Kind kind = terrain::mirror_of(r.flags) == terrain::Mirror::OneWay && !g_one_way_missing ? kOneWay : kSolid;
        // [UE] FRotationMatrix: roll +a turns +Y towards -Z, so a surface rising at +a needs roll = -a
        const double roll = -static_cast<double>(r.roll_deg);
        const double hy = r.hy + (r.roll_deg == 0.0f ? kSeamPad : 0.0);
        const ue::V3 loc{plane_x, map.guest_y(r.y), map.guest_z(r.z)};
        wants[kind].push_back({key_of(loc.y, loc.z, hy, r.hz, roll, r.flags), loc, {kSlabHalfDepth, hy, r.hz}, roll});
    }
    std::size_t moved = 0;
    for (int k = 0; k < kKinds; ++k) moved += sync_pool(world, static_cast<Kind>(k), wants[k]);
    g_applied_revision = g_snap->revision;
    GuestCounters& c = counters();
    c.terrain_revision_applied = g_applied_revision;
    c.terrain_boxes = active_count();
    if (moved) log::info(L"terrain r%u: %u actors (%zu placed, %zu one-way)", g_applied_revision, c.terrain_boxes, moved, wants[kOneWay].size());
}

TerrainStatus terrain_status() { return {g_applied_revision, active_count()}; }

void terrain_reset() {
    for (auto& pool : g_pool) {
        for (Slot& s : pool) park(s);
    }
    g_seen_version = g_seen_epoch = g_applied_revision = 0;
}

} // namespace lie::guest
