#pragma once

// LilyInEM shared-memory contract. Two named mappings:
//   Control (Local\LilyInEM_Control_v1): heartbeats, input ownership, mapping, pose ring, vitals, frame meta.
//   World   (Local\LilyInEM_World_v1)  : mirrored terrain, entity proxies, two one-shot event rings.
// Ownership rule: every block below has exactly one writer (H = Host / EM, G = Guest / EL).
// Consistent groups are published with Seqlock<T>; one-shot actions travel through SpscRing<Event>.

#include "core/frame_map.h"
#include "core/seqlock.h"
#include "core/spsc_ring.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace lie {

inline constexpr std::uint32_t kMagic = 0x4D45494Cu; // "LIEM"
inline constexpr std::uint32_t kProtocol = 2;
inline constexpr const wchar_t* kControlShmName = L"Local\\LilyInEM_Control_v1";
inline constexpr const wchar_t* kWorldShmName = L"Local\\LilyInEM_World_v1";

// EL ECommandInputTypes bit positions [SDK]
namespace cmd {
inline constexpr int Jump = 1;
inline constexpr int Dodge = 2;
inline constexpr int Attack = 3;
inline constexpr int Sub1 = 4;
inline constexpr int Sub2 = 5;
inline constexpr int SummonSwitch = 6;
inline constexpr int Hook = 7;
inline constexpr int Heal = 8;
inline constexpr int Interact = 9;
inline constexpr int Count = 10;
inline constexpr std::uint32_t Mask = 0x3FEu;
} // namespace cmd

// Who owns the player's input (decided by the Host only)
enum class Ctx : std::uint32_t {
    Gameplay = 0, // action keys -> Lily; EM keeps pause / menu / minimap / camera keys
    EmMenu = 1,   // EM UI, pause or event: everything -> EM, Lily frozen
    LilyMenu = 2, // EL native menu: everything -> EL, EM input swallowed, EM paused
    Overlay = 3,  // ImGui loadout panel: EM input swallowed, Lily frozen
    Inactive = 4, // Lilac mode / peer missing
};

// ---------------- Control block ----------------

struct InputFrame {                 // H
    float x = 0.0f, y = 0.0f;
    std::uint32_t raw = 0;          // held cmd bits
    std::uint32_t tap_seq[16]{};    // rising-edge counters per cmd
};

struct HostFlags {                  // H
    std::uint32_t ready = 0;        // Lilac is valid in a playable world
    std::uint32_t bridge_on = 0;    // F7
    std::uint32_t ctx = static_cast<std::uint32_t>(Ctx::Inactive);
    std::uint32_t guest_pause = 1;  // Guest must SetGamePaused(true)
    std::uint32_t rest_pose = 0;    // Lily kneels at an EM bench
    std::uint32_t using_gpu_tex = 0;
    std::uint32_t show_lilac = 0;   // F8 debug
    std::uint32_t debug_hud = 0;    // F10
    float quad_gain = 1.0f;
    float lilac_half_height = 0.0f; // live EM capsule
};

struct RebaseRequest {              // H: "Lilac is here now, re-anchor Lily to it"
    std::uint32_t seq = 0;
    double host_y = 0.0, host_foot = 0.0, host_x = 0.0;
};

struct MapState {                   // G
    FrameMap map{};
    std::uint32_t ack_rebase = 0;   // RebaseRequest::seq this map answers
};

struct PoseSample {                 // G, one per Guest frame
    std::uint32_t epoch = 0;        // FrameMap epoch used to fill host_*
    std::int32_t move_mode = 1;     // EMovementMode
    double t = 0.0;                 // QPC seconds (system-wide)
    double guest_y = 0.0, guest_foot = 0.0;
    double host_y = 0.0, host_foot = 0.0; // the same pose mapped into EM (single mapping function)
    float vy = 0.0f, vz = 0.0f;
    std::int32_t facing = 1;        // +1 right, -1 left
    std::int32_t pad = 0;
};

inline constexpr std::uint32_t kPoseSlots = 32;
struct PoseRing {                   // G
    std::atomic<std::uint32_t> head{0};
    std::array<Seqlock<PoseSample>, kPoseSlots> slots{};
};

struct SpiritSlot {
    std::int32_t limit_type = 0;    // 0 None / 1 CastCount / 2 Time / 3 AttackCount
    std::int32_t remaining = 0;
    float cooldown = 0.0f, recast = 0.0f;
};

struct Vitals {                     // G
    std::int32_t hp = 0, max_hp = 0, mp = 0, max_mp = 0;
    std::int32_t heals = 0, max_heals = 0;
    std::int32_t attack = 0;        // ParameterPlayerComponent attack (k_out denominator)
    std::int32_t summon_set = 1;
    std::int32_t special_unlocked = 0;
    std::int32_t passive_used = 0, passive_max = 0;
    std::uint64_t passives_mask = 0;
    char spirits[2][3][16]{};
    SpiritSlot slots[3]{};
    char anim[32]{};
};

// EL menus: every frame is composited on its own (h_render ps_menu). EL's back buffer alpha cannot be used
// (A2B10G10R10, written opaque); alternating black / white studios (two-background matting) flickered because
// EL's per-frame flag is not reliably aligned with the frame content.
enum class FrameKind : std::uint32_t {
    World = 0, // no EL menu: the whole frame is the world layer
    Menu = 1,  // full-screen EL menu, Lily hidden: menu overlay only, the world layer keeps its last frame
    Split = 2, // rest menu: the rect around Lily and her resting spirits is the world layer, the rest is the menu
};

struct FrameMeta {                  // G, published from Present
    std::uint64_t shared_handle = 0;
    // Second shared texture: the SceneCapture2D output (SCS_SceneColorHDR -> premult RGB + inverse opacity
    // in A, so coverage = 1 - a). The Host unpremultiplies the back buffer RGB by it: s = bb.rgb / cov.
    // 0 while the capture is not running yet -> Host falls back to luminance keying.
    std::uint64_t chars_handle = 0;
    std::uint32_t width = 0, height = 0;
    float foot_u = 0.5f, foot_v = 0.5f;   // Lily's foot in the exported frame
    float view_w_uu = 0.0f, view_h_uu = 0.0f; // world extent of the frame at Lily's depth
    std::uint64_t frame_ms = 0;
    std::uint64_t frame_seq = 0;
    std::uint32_t menu_frame = 0;   // FrameKind: what the Host takes from this frame
    float split[4]{};               // FrameKind::Split: world rect (u0, v0, u1, v1) in frame UV
    float menu_alpha = 1.0f;        // visible menu's ColorAndOpacity.A (Show/Close/HideUI animate it)
};

struct GuestStatus {                // G
    std::uint32_t ready = 0;        // Lily pawn valid and stage anchored
    std::uint32_t menu_open = 0;    // an EL navigable menu is on screen
    float half_height = 0.0f, radius = 0.0f; // live EL capsule
    double frame_dt_p95 = 0.0;      // seconds, drives the pose sampling delay
};

// Mechanical oracle counters (FR-ENG-8), monotonically increasing
struct GuestCounters {
    std::uint32_t pose_published = 0;
    std::uint32_t rebases = 0, recenters = 0;
    std::uint32_t terrain_revision_applied = 0, terrain_boxes = 0;
    std::uint32_t proxy_hits = 0, hit_events = 0;
    std::uint32_t hurt_received = 0, hurt_replayed = 0, knockback_replayed = 0, lethal_sent = 0;
    std::uint32_t falling_frames = 0, autoclimbs = 0;
};

struct HostCounters {
    std::uint32_t puppet_frames = 0;
    double puppet_err_max = 0.0;    // |placed - mapped| worst case since last reset
    std::uint32_t terrain_revision = 0, terrain_rects = 0;
    double terrain_ms_p99 = 0.0;
    std::uint32_t dmg_intercepted = 0, kb_intercepted = 0, lilac_hp_corrections = 0;
    std::uint32_t hit_events_applied = 0, enemy_damage_calls = 0;
};

struct Control {
    std::uint32_t magic = 0, protocol = 0;

    // ---- Host -> Guest ----
    std::atomic<std::uint64_t> host_tick_ms{0};
    std::atomic<std::uint32_t> host_pid{0};
    // Desired EL frame size = EM back buffer x the Host's guest_res_scale (0 = no request yet).
    // The Guest applies it by resizing its off-screen window (r.setres would clamp to the desktop);
    // from FrameMeta, so a resize propagates through the shared textures on its own.
    std::atomic<std::uint32_t> guest_res_w{0}, guest_res_h{0};
    Seqlock<HostFlags> host_flags;
    Seqlock<InputFrame> input;
    Seqlock<RebaseRequest> rebase;
    Seqlock<HostCounters> host_counters;

    // ---- Guest -> Host ----
    std::atomic<std::uint64_t> guest_tick_ms{0};
    std::atomic<std::uint32_t> guest_pid{0};
    Seqlock<GuestStatus> guest_status;
    Seqlock<MapState> map;
    PoseRing pose;
    Seqlock<Vitals> vitals;
    Seqlock<FrameMeta> frame;
    Seqlock<GuestCounters> guest_counters;
};

// ---------------- World block ----------------

enum TerrainFlags : std::uint32_t {
    kTerrainSolid = 1u << 0,
    kTerrainOneWay = 1u << 1,
    kTerrainWater = 1u << 2,
};

struct TerrainRect {                // EM coordinates: centre + half extents in the Y/Z plane
    float y = 0.0f, z = 0.0f, hy = 0.0f, hz = 0.0f;
    float roll_deg = 0.0f;          // rotation about X (slopes)
    std::uint32_t flags = kTerrainSolid;
};

inline constexpr std::uint32_t kMaxTerrainRects = 512;
struct TerrainSnapshot {            // H
    std::uint32_t revision = 0;
    std::uint32_t count = 0;
    double host_x = 0.0;            // Lilac's plane
    TerrainRect rects[kMaxTerrainRects]{};
};

enum class EntityKind : std::uint16_t { Enemy = 1, Breakable = 2, MovingPlatform = 3, HookPoint = 4 };

struct Entity {                     // EM coordinates
    std::uint32_t id = 0;
    EntityKind kind = EntityKind::Enemy;
    std::uint16_t flags = 0;
    float y = 0.0f, z = 0.0f, hy = 0.0f, hz = 0.0f; // hurtbox centre + half extents
};

inline constexpr std::uint32_t kMaxEntities = 64;
struct EntitySnapshot {             // H
    std::uint32_t revision = 0;     // bumps when the set changes (positions change every frame)
    std::uint32_t count = 0;
    Entity items[kMaxEntities]{};
};

enum class EventType : std::uint32_t {
    None = 0,
    // H -> G
    Hurt = 1,       // EM hit Lilac: EL settles it natively
    Restore = 2,    // EM bench / respawn: FullyRestore Lily
    OpenMenu = 3,   // open a Lily menu (MenuKind selects which)
    Equip = 4,      // loadout command from the overlay panel
    CloseMenu = 5,  // the EM bench menu ended: close Lily's rest menu
    Shift = 6,      // move Lily along Y (ShiftPayload): snap to the EM bench seat
    // G -> H
    HitEnemy = 100, // EL hitbox hit an EM-enemy proxy: EM settles it natively
    Lethal = 101,   // Lily would die: EM runs its own death / respawn flow
    RestTravel = 102, // Lily's rest menu "fast travel": open EM's fast-travel map
    RestLeave = 103,  // Lily's rest menu was closed by the player: close the EM bench menu
};

struct ShiftPayload {
    double dy = 0.0;
};

struct HurtPayload {
    std::uint32_t source_id = 0;    // Entity id (0 = hazard / unknown)
    std::int32_t damage_em = 0;
    std::int32_t max_hp_em = 0;     // Lilac's real maximum at hit time
    std::uint8_t element = 0, type = 0, knockback = 0, pad = 0;
    float dir_y = 0.0f;             // sign of the push along Y (+1 right)
};

enum class MenuKind : std::uint32_t {
    Rest = 1,        // native rest menu shown at an EM bench (wakes a dormant one)
    Spirits = 2,     // WBP_MainMenu page 1 (Page_Equipment): spirit loadout
    Relics = 3,      // WBP_MainMenu page 2 (Page_Passive): relic loadout
    Strengthen = 4,  // WBP_Strengthen: spirit strengthening
    Pause = 5,       // EL's Esc menu (WBP_Pause via PC_Base::TryPause), F5 only: EL's own entries are blocked
};
struct MenuPayload {
    MenuKind kind = MenuKind::Rest;
};

enum class EquipOp : std::uint32_t { EquipSpirit = 1, UnequipSpirit = 2, TogglePassive = 3, UnlockAll = 4, SwitchSet = 5 };
struct EquipPayload {
    EquipOp op = EquipOp::EquipSpirit;
    std::int32_t set = 1, slot = 0;
    char id[32]{};
};

struct HitEnemyPayload {
    std::uint32_t entity_id = 0;
    std::int32_t damage_el = 0;     // EL native final damage (relics / spirit level / stance included)
    std::int32_t attack_el = 0;     // EL attack at hit time (k_out)
    std::uint8_t knockback = 0, pad[3]{};
    float dir_y = 0.0f;
};

struct LethalPayload {
    std::int32_t damage_el = 0;
};

struct Event {
    EventType type = EventType::None;
    std::uint32_t epoch = 0;
    double t = 0.0;
    std::uint8_t payload[48]{};

    template <class P>
    static Event make(EventType type, const P& p, double t = 0.0, std::uint32_t epoch = 0) {
        static_assert(sizeof(P) <= sizeof(payload) && std::is_trivially_copyable_v<P>);
        Event e;
        e.type = type;
        e.t = t;
        e.epoch = epoch;
        std::memcpy(e.payload, &p, sizeof(P));
        return e;
    }
    template <class P>
    P as() const {
        P p;
        std::memcpy(&p, payload, sizeof(P));
        return p;
    }
};
static_assert(sizeof(Event) == 64);

inline constexpr std::uint32_t kEventSlots = 64;
struct World {
    std::uint32_t magic = 0, protocol = 0;
    Seqlock<TerrainSnapshot> terrain;           // H
    Seqlock<EntitySnapshot> entities;           // H
    SpscRing<Event, kEventSlots> to_guest;      // producer H, consumer G
    SpscRing<Event, kEventSlots> to_host;       // producer G, consumer H
};

inline bool heartbeat_alive(std::uint64_t tick_ms, std::uint64_t now_ms, std::uint64_t timeout_ms) {
    return tick_ms != 0 && now_ms >= tick_ms && now_ms - tick_ms < timeout_ms;
}

} // namespace lie
