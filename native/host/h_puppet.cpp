// Lilac puppet (FR-MOV-1/2/3/5): every Host frame, before EM's camera ticks, Lilac is placed exactly where
// Lily's mapped pose says (no sweep, no floor probe, no snapping: the Host never solves physics).
// [UE 5.4 PrimitiveComponent.cpp MoveComponentImpl] a non-swept move still runs UpdateOverlaps, so EM triggers,
// water, interaction ranges and hazards keep reacting to the puppet.
//
// Anything else moving Lilac (door transition, respawn, cutscene) shows up as a deviation from the last
// placement and triggers a rebase: "Lilac's current foot <-> Lily's current foot" (answered by the Guest).

#include "host/host.h"

#include "core/pose_ring.h"
#include "rt/clock.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <cmath>

namespace lie::host {

namespace {

using ue::Call;
using ue::V3;

// [POLICY] the puppet is the only writer of Lilac's location; anything above float noise is an external move
constexpr double kExternalMoveTolerance = 1.0;

struct State {
    std::uint32_t rebase_seq = 0;
    std::uint32_t min_epoch = 0;
    bool mapped = false;
    bool have_placed = false;
    V3 placed{};
    int facing = 0;
    double last_t = 0.0;
    double host_dt = 1.0 / 60.0;
} g;

double capsule_hh(UObject* pawn) { return ue::num(ue::obj(pawn, L"CapsuleComponent"), L"CapsuleHalfHeight"); }

UObject* movement(UObject* pawn) {
    UObject* m = ue::obj(pawn, L"ZionCharacterMovement");
    return m ? m : ue::obj(pawn, L"CharacterMovement");
}

} // namespace

std::uint32_t puppet_rebase_seq() { return g.rebase_seq; }

void puppet_request_rebase(UObject* pawn, const char* why) {
    Control* c = shm::control();
    if (!c || !pawn) return;
    const V3 l = ue::actor_location(pawn);
    static std::uint32_t counter = 0;
    RebaseRequest rq;
    rq.seq = (static_cast<std::uint32_t>(GetCurrentProcessId()) << 16) | (++counter & 0xFFFFu); // unique across Host restarts
    g.rebase_seq = rq.seq;
    rq.host_y = l.y;
    rq.host_foot = l.z - capsule_hh(pawn);
    rq.host_x = l.x;
    c->rebase.store(rq);
    g.mapped = false;
    g.have_placed = false;
    log::info(L"rebase #%u requested (%hs) at EM (%.1f, %.1f)", rq.seq, why, rq.host_y, rq.host_foot);
}

bool puppet_mapping_ready() {
    if (g.mapped) return true;
    Control* c = shm::control();
    MapState ms{};
    if (!c || !c->map.read(ms) || ms.ack_rebase != g.rebase_seq || !ms.map.valid()) return false;
    g.mapped = true;
    g.min_epoch = ms.map.epoch;
    return true;
}

PuppetOut puppet_tick(UObject* pawn, bool drive) {
    PuppetOut out;
    Control* c = shm::control();
    const double now = clock::now_s();
    if (g.last_t > 0.0) g.host_dt = std::clamp(now - g.last_t, 1.0 / 240.0, 0.1);
    g.last_t = now;
    if (!c || !pawn) return out;

    const V3 cur = ue::actor_location(pawn);
    if (g.have_placed) {
        const double dev = std::hypot(cur.y - g.placed.y, cur.z - g.placed.z);
        if (dev > kExternalMoveTolerance) {
            puppet_request_rebase(pawn, "EM moved Lilac");
            return out;
        }
    }
    if (!drive || !puppet_mapping_ready()) return out;

    GuestStatus gs{};
    c->guest_status.read(gs);
    const auto s = pose::sample(c->pose, now - pose::sample_delay(gs.frame_dt_p95, g.host_dt));
    if (!s.valid || s.pose.epoch < g.min_epoch) return out;

    const double hh = capsule_hh(pawn);
    const V3 target{cur.x, s.pose.host_y, s.pose.host_foot + hh};
    ue::set_actor_location(pawn, target, false, true);
    const V3 after = ue::actor_location(pawn);
    g.placed = after;
    g.have_placed = true;
    out.placed = true;
    out.location = after;
    out.err = std::hypot(after.y - target.y, after.z - target.z);

    if (s.pose.facing != g.facing) {
        if (UObject* loco = ue::obj(pawn, L"LocomotionComponent")) {
            const int want = s.pose.facing >= 0 ? 2 : 1; // [SDK] EFacing: 1 Left, 2 Right
            Call gf(loco, L"GetFacing");
            if (!gf.go() || gf.ri() != want) Call(loco, L"SetFacing").i(want).go();
        }
        g.facing = s.pose.facing;
    }
    // FR-CAM-1: EM's camera look-ahead reads the movement velocity; give it Lily's
    const V3 vel{0.0, s.pose.vy, s.pose.vz};
    ue::set_vec(movement(pawn), L"Velocity", vel);
    if (UObject* cap = ue::obj(pawn, L"CapsuleComponent")) ue::set_vec(cap, L"ComponentVelocity", vel);
    return out;
}

void puppet_reset() {
    g.mapped = false;
    g.have_placed = false;
    g.facing = 0;
}

} // namespace lie::host
