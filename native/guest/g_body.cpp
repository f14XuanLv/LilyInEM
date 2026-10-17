// Guest main tick (game thread). Lily's body is the only authority for her transform (FR-MOV-1):
// this file owns the EL<->EM mapping, publishes the pose ring / vitals, and settles Host requests.
//
// Lifecycle: title -> autoload slot 0 -> world ready -> anchor (studio, headless, unlocks) -> HOLD
// (movement disabled until the Host answered with a mapping and the mirrored terrain is placed) -> LIVE.

#include "guest/guest.h"

#include "core/el_items.h"
#include "core/facts.h"
#include "core/pose_ring.h"
#include "core/projection.h"
#include "rt/clock.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lie::guest {

const binary::Manifest* g_manifest = nullptr; // set by guest_mod after the binary check

namespace {

using ue::Call;
using ue::V3;

// EL menus that switch the Host to LilyMenu routing
constexpr const wchar_t* kMenuClasses[] = {
    L"WBP_RestMenu_C", L"WBP_EquipmentMenu_C", L"WBP_PassiveMenu_C", L"WBP_Strengthen_C", L"WBP_MainMenu_C", L"WBP_Pause_C",
    L"WBP_FastTravel_C", L"WBP_ChallengeMenu_C", L"WBP_RecollectionMenu_C", L"WBP_DialogYesNo_C", L"WBP_DialogOK_C", L"WBP_DialogTutorial_C",
};
// EL world-business entries in the rest menu: EM owns maps / travel / challenges / recollection (FR-MENU-EL-2)
constexpr const wchar_t* kHiddenRestButtons[] = {L"RestButton_Map", L"RestButton_Map_dummy", L"RestButton_Challenge", L"RestButton_Recollection"};
constexpr const wchar_t* kBlockedHandlers[] = {
    L"BndEvt__RestButton_Map_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature",
    L"BndEvt__WBP_RestMenu_RestButton_Challenge_K2Node_ComponentBoundEvent_4_OnClicked__DelegateSignature",
    L"BndEvt__WBP_RestMenu_RestButton_Recollection_K2Node_ComponentBoundEvent_1_OnClicked__DelegateSignature",
};

// [POLICY] recenter before Lily leaves 40 % of the enlarged camera clamp volume (500000 UU, g_stage.cpp)
constexpr double kRecenterRadius = 200000.0;
// [POLICY] studio height above the loaded EL map: far outside any EL room yet well inside the enlarged camera
// clamp volume (500000) and the recenter radius, so the clamp keeps working and no recenter is triggered.
constexpr double kStudioLift = 60000.0;

struct State {
    ue::Weak pc, pawn, rest_point;
    UObject* armed_pawn = nullptr;
    UObject* anchored_pawn = nullptr;
    bool anchored = false, held = false, paused = false, rest_pose = false, in_title = false;
    MapState map{};
    V3 origin{};
    std::vector<ue::Weak> menus;
    pose::FrameTimer<120> frame_timer;
    GuestCounters k{};
    clock::Every title_try{1.5}, title_check{0.5}, pc_lookup{1.0}, slow{0.25}, counters_pub{0.25};
    int wake_taps = 0;
    double next_wake = 0.0;
    RC::Unreal::FName fn_construct{}, fn_destruct{};
    std::vector<RC::Unreal::FName> blocked;
    bool names = false;
    Vitals vitals{};
} g;

Control* ctl() { return shm::control(); }

bool world_ready() {
    UObject* wl = ue::first(L"WorldLoaderSubsystem");
    if (!wl) return false;
    Call loading(wl, L"IsLoading");
    Call ready(wl, L"IsGameMapReady");
    return loading.b(true).go() && !loading.rb() && ready.b(true).go() && ready.rb();
}

void title_autoload() {
    UObject* title = ue::first(L"TitleMap_C");
    if (!title || !ue::obj(title, L"TitleMenu")) return;
    UObject* save = ue::first(L"SaveSubsystem");
    Call has(save, L"HasSaveDataInSlotIndex");
    if (!save || !ue::call_bool(save, L"IsSettingsLoaded") || !has.i(0).go() || !has.rb()) return;
    log::info(L"title: loading save slot 0");
    Call(title, L"OnSaveSlotSelected").i(0).i(2).go();
}

UObject* cmc_of(UObject* pawn) { return ue::obj(pawn, L"CharacterMovement"); }

void set_held(UObject* pawn, bool hold) {
    if (hold == g.held) return;
    g.held = hold;
    UObject* cmc = cmc_of(pawn);
    if (hold) {
        ue::call(cmc, L"StopMovementImmediately");
        Call(cmc, L"SetMovementMode").i(0 /*MOVE_None*/).i(0).go();
    } else {
        Call(cmc, L"SetMovementMode").i(facts::el::kMoveFalling).i(0).go();
        if (g.wake_taps == 0) {
            g.wake_taps = 3; // FR-INP-6: stand up from the bench sleep pose
            g.next_wake = clock::now_s() + 0.3;
        }
    }
    log::info(hold ? L"Lily held (waiting for mapping + terrain)" : L"Lily released (terrain r%u)", terrain_status().applied_revision);
}

void full_restore(UObject* pc, UObject* pawn) {
    Call(pc, L"FullyRestore").o(pawn).go();
    if (UObject* heal = ue::obj(pc, L"HealComponent")) ue::call(heal, L"FullyRestoreHeals");
}

void add_item(UObject* pc, const wchar_t* table, const char* row) {
    struct RowHandle {
        UObject* table;
        RC::Unreal::FName row;
    };
    UObject* inv = ue::obj(pc, L"InventoryComponent");
    UObject* dt = ue::find(table);
    if (!inv || !dt) return;
    wchar_t w[64]{};
    std::swprintf(w, 64, L"%hs", row);
    RowHandle h{dt, ue::fname(w)};
    Call(inv, L"AddItem").bytes(&h, sizeof(h)).go();
}

void unlock_aptitudes(UObject* pc, UObject* pawn) {
    ue::call(pc, L"UnlockAllAptitudes");
    if (UObject* se = ue::obj(pc, L"SpiritEquipComponent")) Call(se, L"SetCanChangeEquipment").b(true).go();
    if (UObject* wg = ue::obj(pawn, L"BP_WallGrabComponent")) Call(wg, L"SetComponentTickEnabled").b(true).go();
}

void unlock_all(UObject* pc, UObject* pawn) {
    unlock_aptitudes(pc, pawn);
    for (const auto& s : el_items::kSpirits) add_item(pc, facts::el::kSpiritTable, s.id);
    for (const auto& p : el_items::kPassives) add_item(pc, facts::el::kPassiveTable, p.id);
    if (UObject* pe = ue::obj(pc, L"PassiveEquipComponent")) {
        const auto max_slots = ue::call_num(pe, L"GetMaxSlotCount", 0.0);
        if (max_slots > 0.0) Call(pe, L"SetSlotCount").i(static_cast<std::int64_t>(max_slots)).go();
    }
    full_restore(pc, pawn);
    log::info(L"unlocked all spirits / relics / aptitudes");
}

void anchor(UObject* pc, UObject* pawn, V3& loc) {
    stage_isolate_all(pc, pawn);
    // The studio floats far above the loaded EL map: whatever the isolation misses (late spawns, streamed
    // sub-levels, volumes) can never overlap Lily or the mirrored terrain. Lily is held, so this is invisible.
    loc.z += kStudioLift;
    ue::set_actor_location(pawn, loc);
    g.origin = loc;
    stage_headless(pc, pawn);
    unlock_aptitudes(pc, pawn);
    full_restore(pc, pawn);
    if (UObject* sc = ue::obj(pc, L"SpiritCompanionComponent")) ue::call(sc, L"ResetCurrentSetStateAndLocation");
    g.anchored = true;
    g.anchored_pawn = pawn;
    g.held = false;
    set_held(pawn, true);
    log::info(L"studio anchored at (%.1f, %.1f, %.1f)", loc.x, loc.y, loc.z);
}

void reset_anchor() {
    g.anchored = false;
    g.anchored_pawn = nullptr;
    g.held = false;
    g.menus.clear();
    g.map = {};
    stage_reset();
    terrain_reset();
    combat_reset();
    if (Control* c = ctl()) c->map.store(g.map);
}

// ---- menus / rest ----

UObject* spine_anim(UObject* pawn) { return ue::obj(pawn, L"SpineAnimation"); }

void kneel(UObject* pawn, bool on) {
    UObject* sa = spine_anim(pawn);
    if (!sa) return;
    if (on) {
        Call(sa, L"PlayAnimation").s(L"rest_loop").i(facts::el::kSpineLayerEvent).b(true).f(0.0).f(0.18).b(false).go();
        return;
    }
    for (int layer : {facts::el::kSpineLayerEvent, facts::el::kSpineLayerEvent - 1, facts::el::kSpineLayerEvent - 2}) {
        Call(sa, L"StopAnimation").i(layer).b(true).f(0.15).go();
    }
    ue::call(sa, L"OnEventStop");
    ue::call(sa, L"RequestRefreshPlayingAnimation");
}

void open_rest_menu(UObject* pc, const V3& at) {
    if (!g.rest_point) g.rest_point.set(ue::first(L"BP_Interactable_RestingPoint_C"));
    UObject* rp = g.rest_point.get();
    if (!rp) {
        log::warn(L"no EL rest point in this map: cannot open Lily's menu");
        return;
    }
    Call(rp, L"SetActorEnableCollision").b(true).go();
    ue::set_actor_location(rp, at);
    if (UObject* ps = ue::obj(rp, L"PlayerStart")) ue::set_actor_location(ps, at); // EVT_ACT_MoveToTarget finishes at once
    Call(rp, L"OnInteract").o(pc).go();
    log::info(L"opened Lily's rest menu");
}

void tidy_rest_menu(UObject* menu) {
    for (const wchar_t* b : kHiddenRestButtons) {
        if (UObject* btn = ue::obj(menu, b)) Call(btn, L"SetVisibility").i(1 /*Collapsed*/).go();
    }
}

// The hidden Map button is EL's default focus: give focus to the first remaining entry so forwarded keys navigate
void keep_menu_focus() {
    std::erase_if(g.menus, [](const ue::Weak& w) { return !w.get(); });
    if (g.menus.size() != 1) return;
    UObject* menu = g.menus.front().get();
    if (ue::class_name(menu) != L"WBP_RestMenu_C" || ue::call_bool(menu, L"HasFocusedDescendants")) return;
    for (const wchar_t* b : {L"RestButton_Equipment", L"RestButton_Equipment_dummy", L"RestButton_Leave"}) {
        UObject* btn = ue::obj(menu, b);
        if (btn && ue::call_bool(btn, L"IsVisible")) {
            ue::call(btn, L"SetFocus");
            return;
        }
    }
}

// ---- direct menus ----

// EM bench buttons skip EL's sit-down event entirely and spawn the target menu straight into the
// viewport. This mirrors the native rest-menu button handlers (WBP_RestMenu ubergraph):
// SetCanChangeEquipment -> Create -> SetIntPropertyByName(DefaultPageIndex) -> AddToViewport(10).
// DefaultPageIndex is ExposeOnSpawn and read by WBP_MainMenu::Construct, so it must be written
// between Create and AddToViewport.
constexpr const wchar_t* kMainMenuClass = L"/Game/_Zenith/UI/MainMenu/WBP_MainMenu.WBP_MainMenu_C";
constexpr const wchar_t* kStrengthenClass = L"/Game/_Zenith/UI/Strengthen/WBP_Strengthen.WBP_Strengthen_C";

UObject* spawn_menu(UObject* pc, const wchar_t* class_path, int page) {
    UObject* lib = ue::find(L"/Script/UMG.Default__WidgetBlueprintLibrary");
    UClass* cls = ue::load_class(class_path);
    if (!lib || !cls || !pc) return nullptr;
    Call create(lib, L"Create");
    if (!create.o(pc).o(cls).o(pc).go()) return nullptr;
    UObject* w = create.ro();
    if (!w) return nullptr;
    if (page >= 0) ue::set_num(w, L"DefaultPageIndex", static_cast<double>(page));
    int zorder = 500; // GetUMGZOrder(EUMGZOrderLayer::Menu=10) -> 500, same layer the event uses
    if (UObject* rbfl = ue::find(L"/Script/Zenith.Default__RenderBPFLibrary")) {
        Call z(rbfl, L"GetUMGZOrder");
        if (z.i(10).go()) zorder = static_cast<int>(z.ri());
    }
    Call(w, L"AddToViewport").i(zorder).go();
    return w;
}

void open_menu(UObject* pc, const V3& at, MenuKind kind) {
    if (kind == MenuKind::Rest) {
        open_rest_menu(pc, at);
        return;
    }
    if (!g.menus.empty()) return; // never stack a second EL menu
    // same unlock the rest menu applies before opening its loadout pages (also the OPMod's toggle)
    if (UObject* se = ue::obj(pc, L"SpiritEquipComponent")) Call(se, L"SetCanChangeEquipment").b(true).go();
    const wchar_t* cls = nullptr;
    int page = -1;
    switch (kind) {
    case MenuKind::Spirits: cls = kMainMenuClass; page = 1; break;
    case MenuKind::Relics: cls = kMainMenuClass; page = 2; break;
    case MenuKind::Strengthen: cls = kStrengthenClass; break;
    default: return;
    }
    if (spawn_menu(pc, cls, page)) log::info(L"opened Lily's menu (kind=%d)", static_cast<int>(kind));
    else log::warn(L"failed to open Lily's menu (kind=%d)", static_cast<int>(kind));
}

// ---- loadout ----

void read_loadout(UObject* pc, UObject* pawn, Vitals& v) {
    std::memset(v.spirits, 0, sizeof(v.spirits));
    if (UObject* se = ue::obj(pc, L"SpiritEquipComponent")) {
        v.summon_set = static_cast<std::int32_t>(ue::call_num(se, L"GetCurrentSummonSet", 1.0));
        for (int set = 1; set <= 2; ++set) {
            Call c(se, L"GetSummonSetDataAsReadOnly");
            if (!c.i(set).go() || !c.ret_prop()) continue;
            ue::FProperty* spirits = ue::member_of(c.ret_prop(), L"Spirits");
            if (!spirits) continue;
            auto* base = static_cast<std::uint8_t*>(c.slot(c.ret_prop())) + spirits->GetOffset_Internal();
            ue::map_each(base, spirits, [&](void* k, void* val, ue::FProperty* kp, ue::FProperty* vp) {
                const int slot = static_cast<int>(ue::read_num(kp, k)) - cmd::Attack; // ECommandInputTypes 3..5
                ue::FProperty* idp = ue::member_of(vp, L"SpiritID");
                if (slot < 0 || slot > 2 || !idp) return;
                const std::wstring id = ue::str(*reinterpret_cast<RC::Unreal::FName*>(static_cast<std::uint8_t*>(val) + idp->GetOffset_Internal()));
                if (id != L"None") std::snprintf(v.spirits[set - 1][slot], 16, "%ls", id.c_str());
            });
        }
    }
    v.passives_mask = 0;
    if (UObject* pe = ue::obj(pc, L"PassiveEquipComponent")) {
        v.passive_used = static_cast<std::int32_t>(ue::call_num(pe, L"GetUsedSlotCount"));
        v.passive_max = static_cast<std::int32_t>(ue::call_num(pe, L"GetSlotCount"));
        for (const auto& p : el_items::kPassives) {
            wchar_t w[64]{};
            std::swprintf(w, 64, L"%hs", p.id);
            Call c(pe, L"IsPassiveEquipped");
            if (c.n(w).go() && c.rb()) v.passives_mask |= 1ull << p.bit;
        }
    }
    Call apt(pc, L"IsAptitudeUnlocked");
    v.special_unlocked = (apt.i(facts::el::kAptitudeSpecialAttack).go() && apt.rb()) ? 1 : 0;
    if (UObject* ppc = ue::obj(pc, L"ParameterPlayerComponent")) {
        Call atk(ppc, L"GetAttack");
        if (atk.b(false).go()) v.attack = static_cast<std::int32_t>(atk.ri());
    }
    (void)pawn;
}

void apply_equip(UObject* pc, UObject* pawn, const EquipPayload& e) {
    UObject* se = ue::obj(pc, L"SpiritEquipComponent");
    UObject* pe = ue::obj(pc, L"PassiveEquipComponent");
    wchar_t id[40]{};
    std::swprintf(id, 40, L"%hs", e.id);
    switch (e.op) {
    case EquipOp::EquipSpirit:
        if (!se || !id[0]) break;
        add_item(pc, facts::el::kSpiritTable, e.id);
        Call(se, L"UnequipSpirit").i(e.set).i(e.slot).go();
        Call(se, L"EquipSpirit").i(e.set).n(id).i(e.slot).go();
        break;
    case EquipOp::UnequipSpirit:
        if (se) Call(se, L"UnequipSpirit").i(e.set).i(e.slot).go();
        break;
    case EquipOp::TogglePassive: {
        if (!pe || !id[0]) break;
        add_item(pc, facts::el::kPassiveTable, e.id);
        Call eq(pe, L"IsPassiveEquipped");
        const bool on = eq.n(id).go() && eq.rb();
        Call(pe, on ? L"UnequipPassive" : L"EquipPassive").n(id).go();
        break;
    }
    case EquipOp::UnlockAll: unlock_all(pc, pawn); break;
    case EquipOp::SwitchSet:
        if (se) Call(se, L"SwitchSummonSet").i(e.set).go();
        break;
    }
}

// ---- Host events ----

void drain_events(UObject* pc, UObject* pawn, const V3& loc) {
    World* w = shm::world();
    if (!w) return;
    Event e;
    while (w->to_guest.pop(e)) {
        switch (e.type) {
        case EventType::Hurt: combat_on_hurt(pawn, e.as<HurtPayload>()); break;
        case EventType::Restore: full_restore(pc, pawn); break;
        case EventType::OpenMenu: open_menu(pc, loc, e.as<MenuPayload>().kind); break;
        case EventType::Equip: apply_equip(pc, pawn, e.as<EquipPayload>()); break;
        default: break;
        }
    }
}

// ---- publishing ----

void publish_projection(UObject* pc, const V3& foot) {
    UObject* cm = ue::obj(pc, L"PlayerCameraManager");
    Call cl(cm, L"GetCameraLocation");
    Call cr(cm, L"GetCameraRotation");
    Call cf(cm, L"GetFOVAngle");
    if (!cl.go() || !cr.go() || !cf.go()) return;
    const V3 cam = cl.rv();
    const V3 rot = cr.rv(); // FRotator read as (pitch, yaw, roll)
    FrameMeta fm{};
    double aspect = 16.0 / 9.0;
    if (ctl()->frame.read(fm) && fm.width && fm.height) aspect = double(fm.width) / double(fm.height);
    const auto s = proj::project({foot.x, foot.y, foot.z}, {cam.x, cam.y, cam.z}, rot.x, rot.y, rot.z, cf.rf(), aspect);
    if (!s.valid) return;
    const double w = proj::view_width(s.depth, cf.rf());
    stage_frame(static_cast<float>(s.u), static_cast<float>(s.v), static_cast<float>(w), static_cast<float>(w / aspect));
}

void publish_vitals(UObject* pc, UObject* pawn, const std::wstring& anim, bool slow) {
    Vitals& v = g.vitals;
    UObject* hp = ue::obj(pawn, L"HPComponent");
    UObject* mp = ue::obj(pawn, L"MPComponent");
    v.hp = static_cast<std::int32_t>(ue::num(hp, L"CurrHP"));
    v.max_hp = static_cast<std::int32_t>(ue::num(hp, L"MaxHP"));
    v.mp = static_cast<std::int32_t>(ue::num(mp, L"CurrMP"));
    v.max_mp = static_cast<std::int32_t>(ue::num(mp, L"MaxMP"));
    if (UObject* heal = ue::obj(pc, L"HealComponent")) {
        v.heals = static_cast<std::int32_t>(ue::call_num(heal, L"GetRemainingHealCount"));
        v.max_heals = static_cast<std::int32_t>(ue::call_num(heal, L"GetMaxHealCount"));
    }
    std::snprintf(v.anim, sizeof(v.anim), "%ls", anim.c_str());
    if (slow) read_loadout(pc, pawn, v);
    ctl()->vitals.store(v);
}

std::wstring current_anim(UObject* pawn) {
    Call cur(ue::obj(pawn, L"SpineSkeletonAnimationEx"), L"GetCurrent");
    if (cur.i(0).go() && cur.ro()) {
        Call n(cur.ro(), L"GetAnimationName");
        if (n.go()) return n.rs();
    }
    return L"";
}

} // namespace

// ================= shared helpers =================

bool host_alive() {
    Control* c = ctl();
    if (!c) return false;
    HostFlags f{};
    return heartbeat_alive(c->host_tick_ms.load(), clock::now_ms(), facts::kHeartbeatTimeoutMs) && c->host_flags.read(f) && f.ready && f.bridge_on;
}

Ctx host_ctx() {
    Control* c = ctl();
    HostFlags f{};
    if (!c || !heartbeat_alive(c->host_tick_ms.load(), clock::now_ms(), facts::kHeartbeatTimeoutMs) || !c->host_flags.read(f) || !f.ready) return Ctx::Inactive;
    return static_cast<Ctx>(f.ctx);
}

GuestCounters& counters() { return g.k; }
void publish_counters() {
    if (Control* c = ctl()) c->guest_counters.store(g.k);
}
UObject* lily() { return g.pawn.get(); }

// ================= tick =================

void tick() {
    Control* c = ctl();
    if (!c) return;
    const double now = clock::now_s();
    if (!g.names) {
        g.fn_construct = ue::fname(L"Construct");
        g.fn_destruct = ue::fname(L"Destruct");
        for (const wchar_t* h : kBlockedHandlers) g.blocked.push_back(ue::fname(h));
        g.names = true;
    }
    c->guest_pid.store(GetCurrentProcessId());
    c->guest_tick_ms.store(clock::now_ms());
    stage_window_tick(now);
    g.frame_timer.tick(now);

    UObject* pc = g.pc.get();
    if (!pc && g.pc_lookup.due(now)) {
        g.pc.set(ue::first(L"ZenithPlayerController"));
        pc = g.pc.get();
    }
    UObject* pawn = pc ? ue::obj(pc, L"Pawn") : nullptr;
    if (!g.anchored && g.title_check.due(now)) g.in_title = ue::first(L"TitleMap_C") != nullptr;
    GuestStatus st{};
    if (!pawn || (!g.anchored && g.in_title)) {
        if (g.anchored) reset_anchor();
        if (g.title_try.due(now)) title_autoload();
        c->guest_status.store(st);
        return;
    }
    if (g.anchored && g.anchored_pawn != pawn) reset_anchor(); // EL respawned Lily or reloaded the map
    if (!g.anchored && !world_ready()) return;
    g.pawn.set(pawn);
    if (g.armed_pawn != pawn && g_manifest && arm_input(*g_manifest, pawn)) {
        g.armed_pawn = pawn;
        log::info(L"input armed for %s", pawn->GetName().c_str());
    }

    V3 loc = ue::actor_location(pawn);
    UObject* cap = ue::obj(pawn, L"CapsuleComponent");
    const double hh = ue::num(cap, L"CapsuleHalfHeight");
    const double radius = ue::num(cap, L"CapsuleRadius");
    UObject* world = ue::world_of(pc);
    if (!g.anchored) anchor(pc, pawn, loc);

    // ---- mapping: rebase on Host request, recenter when far from the studio origin ----
    RebaseRequest rq{};
    if (c->rebase.read(rq) && rq.seq != 0 && rq.seq != g.map.ack_rebase) {
        g.map.map = FrameMap::rebase(g.map.map.epoch, rq.host_y, rq.host_foot, loc.y, loc.z - hh);
        g.map.ack_rebase = rq.seq;
        c->map.store(g.map);
        ++g.k.rebases;
        log::info(L"rebase #%u: EM (%.1f, %.1f) <-> EL (%.1f, %.1f) epoch %u", rq.seq, rq.host_y, rq.host_foot, loc.y, loc.z - hh, g.map.map.epoch);
    }
    if (g.map.map.valid() && (std::abs(loc.y - g.origin.y) > kRecenterRadius || std::abs(loc.z - g.origin.z) > kRecenterRadius)) {
        const double dy = g.origin.y - loc.y, dz = g.origin.z - loc.z;
        loc = {loc.x, loc.y + dy, loc.z + dz};
        ue::set_actor_location(pawn, loc);
        g.map.map = g.map.map.recentered(dy, dz);
        c->map.store(g.map);
        if (UObject* sc = ue::obj(pc, L"SpiritCompanionComponent")) ue::call(sc, L"ResetCurrentSetStateAndLocation");
        ++g.k.recenters;
    }
    terrain_tick(world, g.map.map, g.origin.x, false);
    const bool live = host_alive() && g.map.map.valid() && terrain_status().applied_revision != 0;
    set_held(pawn, !live);

    // ---- pose ----
    UObject* cmc = cmc_of(pawn);
    const int mode = static_cast<int>(ue::num(cmc, L"MovementMode", facts::el::kMoveWalking));
    Call vc(pawn, L"GetVelocity");
    const V3 vel = vc.go() ? vc.rv() : V3{};
    const int facing = ue::call_bool(ue::obj(pawn, L"LocomotionComponent"), L"IsFacingRight", true) ? 1 : -1;
    const double foot = loc.z - hh;
    if (g.map.map.valid()) {
        PoseSample s;
        s.epoch = g.map.map.epoch;
        s.move_mode = mode;
        s.t = now;
        s.guest_y = loc.y;
        s.guest_foot = foot;
        s.host_y = g.map.map.host_y(loc.y);
        s.host_foot = g.map.map.host_z(foot);
        s.vy = static_cast<float>(vel.y);
        s.vz = static_cast<float>(vel.z);
        s.facing = facing;
        pose::publish(c->pose, s);
        ++g.k.pose_published;
        if (live && mode == facts::el::kMoveFalling) ++g.k.falling_frames;
    }
    publish_projection(pc, {loc.x, loc.y, foot});
    const bool slow = g.slow.due(now);
    const std::wstring anim = current_anim(pawn);
    publish_vitals(pc, pawn, anim, slow);

    // ---- Host requests / combat ----
    HostFlags hf{};
    c->host_flags.read(hf);
    if (host_alive()) {
        drain_events(pc, pawn, loc);
        if ((hf.rest_pose != 0) != g.rest_pose) {
            g.rest_pose = hf.rest_pose != 0;
            kneel(pawn, g.rest_pose);
        }
    } else if (World* w = shm::world()) {
        w->to_guest.drain(); // stale requests from a previous Host session are never replayed
    }
    combat_tick(world, pc, pawn, g.map.map, g.origin.x);
    keep_menu_focus();

    // ---- wake taps after release ----
    if (g.wake_taps > 0 && !g.held && now >= g.next_wake) {
        local_input(0.0f, 0.0f, 0, 1u << cmd::Jump, 2);
        --g.wake_taps;
        g.next_wake = now + 0.3;
    }

    // ---- pause sync (FR-HDL-5) ----
    const bool want_pause = host_alive() && hf.guest_pause && g.menus.empty();
    if (want_pause != g.paused) {
        Call(ue::gameplay_statics(), L"SetGamePaused").o(world).b(want_pause).go();
        g.paused = want_pause;
    }

    st.ready = (live && !g.held) ? 1u : 0u;
    st.menu_open = g.menus.empty() ? 0u : 1u;
    st.half_height = static_cast<float>(hh);
    st.radius = static_cast<float>(radius);
    st.frame_dt_p95 = g.frame_timer.p95();
    c->guest_status.store(st);
    if (g.counters_pub.due(now)) publish_counters();
}

bool on_process_event_pre(UObject* ctx, ue::UFunction* fn) {
    if (!g.anchored || !g.names || !ctx || !fn) return false;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    for (const auto& h : g.blocked) {
        if (n == h) {
            log::info(L"blocked EL world-business button %s", fn->GetName().c_str());
            return true;
        }
    }
    return false;
}

void on_process_event_post(UObject* ctx, ue::UFunction* fn) {
    if (!g.names || !ctx || !fn) return;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    const bool construct = n == g.fn_construct;
    if (!construct && n != g.fn_destruct) return;
    const std::wstring cls = ue::class_name(ctx);
    bool menu = false;
    for (const wchar_t* m : kMenuClasses) menu = menu || cls == m;
    if (!menu) return;
    if (construct) {
        g.menus.emplace_back(ctx);
        if (cls == L"WBP_RestMenu_C") tidy_rest_menu(ctx);
        // EL's map describes its own world — EM owns maps, so the category tab stays hidden (FR-MENU-EL-2)
        else if (cls == L"WBP_MainMenu_C") {
            if (UObject* tab = ue::obj(ctx, L"Map")) Call(tab, L"SetVisibility").i(1).go();
        }
    } else {
        std::erase_if(g.menus, [&](const ue::Weak& w) { return w.get() == ctx || !w.get(); });
        if (cls == L"WBP_RestMenu_C" && !g.rest_pose) kneel(g.pawn.get(), false);
    }
}

std::string status() {
    Control* c = ctl();
    if (!c) return "shm=null";
    const TerrainStatus ts = terrain_status();
    char buf[700];
    std::snprintf(buf, sizeof(buf),
                  "anchored=%d held=%d hostAlive=%d ctx=%u epoch=%u ack=%u map=(H %.1f,%.1f | G %.1f,%.1f) terrain=r%u/%u boxes "
                  "hp=%d/%d mp=%d/%d heals=%d/%d atk=%d set=%d menus=%zu paused=%d rebases=%u recenters=%u falling=%u hurt=%u/%u hits=%u lethal=%u dtp95=%.4f",
                  g.anchored, g.held, host_alive(), static_cast<unsigned>(host_ctx()), g.map.map.epoch, g.map.ack_rebase, g.map.map.h0_y, g.map.map.h0_foot,
                  g.map.map.g0_y, g.map.map.g0_foot, ts.applied_revision, ts.boxes, g.vitals.hp, g.vitals.max_hp, g.vitals.mp, g.vitals.max_mp,
                  g.vitals.heals, g.vitals.max_heals, g.vitals.attack, g.vitals.summon_set, g.menus.size(), g.paused, g.k.rebases, g.k.recenters,
                  g.k.falling_frames, g.k.hurt_received, g.k.hurt_replayed, g.k.hit_events, g.k.lethal_sent, g.frame_timer.p95());
    return buf;
}

void debug(const std::wstring& cmd) {
    UObject* pc = g.pc.get();
    UObject* pawn = g.pawn.get();
    if (cmd == L"unlock" && pc && pawn) unlock_all(pc, pawn);
    else if (cmd == L"restore" && pc && pawn) full_restore(pc, pawn);
    else if (cmd == L"reanchor") reset_anchor();
    else if (cmd == L"isolate" && pc && pawn) stage_isolate_all(pc, pawn);
    else if (cmd == L"menu" && pc && pawn) open_rest_menu(pc, ue::actor_location(pawn));
    else if (cmd == L"menu2" && pc) open_menu(pc, ue::actor_location(pawn), MenuKind::Spirits);
    else if (cmd == L"menu3" && pc) open_menu(pc, ue::actor_location(pawn), MenuKind::Relics);
    else if (cmd == L"menu4" && pc) open_menu(pc, ue::actor_location(pawn), MenuKind::Strengthen);
    else if (cmd == L"terrain") terrain_tick(ue::world_of(pc), g.map.map, g.origin.x, true);
}

} // namespace lie::guest
