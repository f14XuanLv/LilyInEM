// Host orchestration (game thread, UEngine::Tick PRE: before EM's world and camera tick, FR-MOV-2).
// Decides the input owner (Ctx), locks Lilac out of her own controls, keeps EM menus that are world services
// and redirects the ones that belong to the character (EM bench menu -> Lily's native rest menu), and drives
// the puppet / terrain / combat modules and the in-world Lily quad.

#include "host/host.h"

#include "core/facts.h"
#include "rt/clock.h"
#include "rt/input.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace lie::host {

namespace {

using ue::Call;
using ue::V3;

// Lilac components that would act on their own while she is a puppet (Spine animation / bone components keep
// ticking: they drive the camera's TargetPivotComponent)
constexpr const wchar_t* kLockedComponents[] = {
    L"ZionInputComponent", L"InputBufferComponent", L"CommandComponent", L"AbilityComponent", L"SpineRootMotionComponent",
    L"Summon", L"SummonIdleSpirit", L"SummonRestPointSpirit", L"DashCharge", L"FallThrough", L"Hook Component",
    L"Wall Grab Component", L"Auto Climb Component",
};
// EM menus that hand input to EM
constexpr const wchar_t* kEmMenus[] = {
    L"WBP_Pause_C", L"WBP_GameMenu_C", L"WBP_RestMenu_C", L"WBP_Shop_C", L"WBP_Settings_C",
    L"WBP_Tutorial_C", L"WBP_Recollection_C", L"WBP_SkillUnlock_C", L"WBP_PassiveUnlock_C",
};
// EM bench menu (FR-MENU-EM-2): in Lily mode it stays alive but invisible and Lily's native rest menu stands in
// for it. None of its own entries may fire; fast travel is driven from Lily's menu through OpenGameMenu(7)
// (what its FastTravel button does), and closing either menu closes the other.
constexpr const wchar_t* kBenchMenu = L"WBP_RestMenu_C";
constexpr const wchar_t* kBenchBlocked[] = {
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_Exit_K2Node_ComponentBoundEvent_1_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_Save_K2Node_ComponentBoundEvent_2_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_FastTravelqa_K2Node_ComponentBoundEvent_4_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_DifficultyA_K2Node_ComponentBoundEvent_5_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_Extra_K2Node_ComponentBoundEvent_0_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_Extra_K2Node_ComponentBoundEvent_3_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_SetSkill_K2Node_ComponentBoundEvent_6_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_SetEquipment_K2Node_ComponentBoundEvent_7_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_SetPassive_K2Node_ComponentBoundEvent_8_OnPressed__DelegateSignature",
    L"BndEvt__WBP_RestMenu_WBP_RestMenu_Button_SkillUpgrade_K2Node_ComponentBoundEvent_9_OnPressed__DelegateSignature",
    L"OnPressContext", L"OnPressContextAlt",
};
constexpr int kBenchFastTravelPage = 7;
// EVT_RestPoint walks the player to the bench (EventAction_MoveTo_Actor, AcceptanceRadius 2). The puppet's
// movement never ticks, so that walk only ends on EM's block detection (seconds); a seated start ends it at once.
constexpr double kBenchSnapMin = 1.0;
constexpr const wchar_t* kLilacTabs[] = {L"WBP_GameMenu_Tab_Equipment", L"WBP_GameMenu_Tab_Skills", L"WBP_GameMenu_Tab_Passives"};
constexpr const wchar_t* kLilacPages[] = {L"WBP_GameMenu_Page_Equipment_C", L"WBP_GameMenu_Page_Skill_C", L"WBP_GameMenu_Page_Passive_C"};
// FR-HUD-1: Lilac-only parts of WBP_PlayerUI (currency and EXP stay: world services)
constexpr const wchar_t* kLilacHud[] = {
    L"WBP_PlayerHPGauge", L"WBP_PlayerSPGauge", L"WBP_SkillSet", L"WBP_HealingItems", L"WBP_LowHPFeedback", L"LowHPFeedbackOverlay", L"WBP_StatusEffects",
};
// [POLICY] interaction reach around Lily's feet when she presses Interact
constexpr double kInteractRadius = 200.0;

struct State {
    ue::Weak pc, pawn, rt, mid, quad;
    bool locked = false;
    bool active = false;
    bool resting = false;
    bool paused_by_us = false;
    bool hud_hidden = false;
    bool was_bridged = false;
    Ctx ctx = Ctx::Inactive;
    std::vector<ue::Weak> em_menus;
    int game_menu_page = 0;
    bool redirecting_page = false;
    std::uint32_t seen_interact = 0;
    ue::Weak bench_menu;            // the invisible EM bench menu Lily's rest menu stands in for
    std::int64_t bench_vis = 4;     // its own visibility, restored in Lilac mode
    int travel = 0;                 // 0 none / 1 EM fast-travel map requested / 2 map on screen
    bool travel_chosen = false;     // a destination was picked: the bench menu closes, Lily's menu is not woken
    HostFlags flags{};
    HostCounters k{};
    clock::Every pc_lookup{1.0}, relock{0.25}, hud{1.0}, counters_pub{0.25}, rest_spirits{0.1};
    RC::Unreal::FName fn_construct{}, fn_destruct{}, fn_page_changed{}, fn_travel_requested{}, fn_close_menu{};
    std::vector<RC::Unreal::FName> bench_blocked;
    bool names = false;
    double puppet_err_max = 0.0;
} g;

Control* ctl() { return shm::control(); }

bool guest_connected() {
    Control* c = ctl();
    FrameMeta fm{};
    const std::uint64_t now = clock::now_ms();
    return c && heartbeat_alive(c->guest_tick_ms.load(), now, facts::kHeartbeatTimeoutMs) && c->frame.read(fm) &&
           heartbeat_alive(fm.frame_ms, now, facts::kHeartbeatTimeoutMs); // a ticking-but-not-presenting Guest is not usable
}

bool guest_ready() {
    GuestStatus st{};
    return ctl()->guest_status.read(st) && st.ready;
}

// ---- Lilac lockout ----

void set_lilac_visible(UObject* pawn, bool visible) {
    if (UObject* r = ue::obj(pawn, L"SpineRendererComponent")) {
        Call(r, L"SetVisibility").b(visible).b(true).go();
        Call(r, L"SetHiddenInGame").b(!visible).b(true).go();
    }
}

void hide_spirit(UObject* sp, bool hidden) {
    Call(sp, L"SetActorHiddenInGame").b(hidden).go();
    if (!hidden) return;
    Call(sp, L"SetActorEnableCollision").b(false).go();
    if (UObject* cc = ue::obj(sp, L"CommandComponent")) Call(cc, L"ClearCommands").b(true).go();
}

void sweep_spirits(bool hidden) {
    std::vector<UObject*> all;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("CharacterZionSpirit"), all); // on lock / unlock only
    for (UObject* s : all) hide_spirit(s, hidden);
}

UObject* movement(UObject* pawn) {
    UObject* m = ue::obj(pawn, L"ZionCharacterMovement");
    return m ? m : ue::obj(pawn, L"CharacterMovement");
}

void assert_lock(UObject* pc, UObject* pawn) {
    Call(pawn, L"DisableInput").o(pc).go();
    if (UObject* cc = ue::obj(pawn, L"CommandComponent")) Call(cc, L"ClearCommands").b(true).go();
    for (const wchar_t* n : kLockedComponents) {
        if (UObject* c = ue::obj(pawn, n)) Call(c, L"SetComponentTickEnabled").b(false).go();
    }
    if (UObject* m = movement(pawn)) Call(m, L"SetComponentTickEnabled").b(false).go();
}

void lock(UObject* pc, UObject* pawn, bool on) {
    if (on && !g.locked) {
        assert_lock(pc, pawn);
        for (const wchar_t* s : {L"Summon", L"SummonIdleSpirit"}) {
            if (UObject* c = ue::obj(pawn, s)) ue::call(c, L"FlushCachedSpirits");
        }
        if (UObject* m = movement(pawn)) ue::call(m, L"StopMovementImmediately");
        sweep_spirits(true);
        for (const wchar_t* cmd : {L"r.VSync 0", L"t.MaxFPS 60"}) ue::console(pc, cmd);
        g.locked = true;
        log::info(L"Lily mode: Lilac locked out");
        render::toast("莉莉模式 Lily Mode");
    } else if (!on && g.locked) {
        if (UObject* m = movement(pawn)) {
            ue::set_vec(m, L"Velocity", {});
            Call(m, L"SetComponentTickEnabled").b(true).go();
        }
        for (const wchar_t* n : kLockedComponents) {
            if (UObject* c = ue::obj(pawn, n)) Call(c, L"SetComponentTickEnabled").b(true).go();
        }
        Call(pawn, L"EnableInput").o(pc).go();
        sweep_spirits(false);
        g.locked = false;
        log::info(L"Lilac mode: native control restored");
        render::toast("莱拉克模式 Lilac Mode");
    }
    // EM events re-enable input / components on Lilac: re-assert
    if (g.locked && g.relock.due(clock::now_s())) assert_lock(pc, pawn);
}

void hide_lilac_hud(UObject* pc, bool hidden) {
    UObject* ui = ue::obj(pc, L"PlayerUI");
    if (!ui) return;
    for (const wchar_t* part : kLilacHud) {
        if (UObject* w = ue::obj(ui, part)) Call(w, L"SetVisibility").i(hidden ? 1 /*Collapsed*/ : 4 /*SelfHitTestInvisible*/).go();
    }
    g.hud_hidden = hidden;
}

// ---- menus ----

bool em_menu_open(const wchar_t* cls) {
    for (const ue::Weak& w : g.em_menus) {
        if (ue::class_name(w.get()) == cls) return true;
    }
    return false;
}

void skip_lilac_page(UObject* menu, int index) {
    if (g.redirecting_page) return;
    UObject* sw = ue::obj(menu, L"PageSwitcher");
    auto lilac_page = [&](int i) {
        Call w(sw, L"GetWidgetAtIndex");
        const std::wstring cls = ue::class_name(w.i(i).go() ? w.ro() : nullptr);
        return std::ranges::any_of(kLilacPages, [&](const wchar_t* p) { return cls == p; });
    };
    if (!lilac_page(index)) {
        g.game_menu_page = index;
        return;
    }
    const int count = static_cast<int>(ue::call_num(menu, L"GetPageCount"));
    if (count <= 1) return;
    auto wrap = [&](int i) { return ((i % count) + count) % count; };
    const int dir = index == wrap(g.game_menu_page - 1) ? -1 : 1;
    int target = index;
    for (int k = 0; k < count && lilac_page(target); ++k) target = wrap(target + dir);
    g.redirecting_page = true;
    Call(menu, L"GoToPage").i(target).go();
    g.redirecting_page = false;
    g.game_menu_page = target;
}

// An EM bench rest is an event too: the player has no control while EM seats her, exactly like EM itself
Ctx compute_ctx(UObject* pc, bool is_bridged, bool in_event) {
    if (!is_bridged) return Ctx::Inactive;
    if (render::overlay_open()) return Ctx::Overlay;
    GuestStatus st{};
    if (ctl()->guest_status.read(st) && st.menu_open) return Ctx::LilyMenu;
    std::erase_if(g.em_menus, [](const ue::Weak& w) { return !w.get(); });
    Call paused(ue::gameplay_statics(), L"IsGamePaused");
    const bool em_paused = paused.o(pc).go() && paused.rb() && !g.paused_by_us;
    if (in_event || em_paused || !g.em_menus.empty()) return Ctx::EmMenu;
    return Ctx::Gameplay;
}

// ---- EM bench <-> Lily's rest menu ----

// The bench menu lives inside EVT_RestPoint's WBP_Event, whose open animation drives the menu's render opacity:
// only Hidden visibility keeps it off screen (re-asserted every tick in case the event touches it)
constexpr std::int64_t kHidden = 2;

void bench_hide(UObject* menu) {
    Call vis(menu, L"GetVisibility");
    if (vis.go() && vis.ri() != kHidden) Call(menu, L"SetVisibility").i(kHidden).go();
}

void bench_open(UObject* menu) {
    g.bench_menu.set(menu);
    g.travel = 0;
    g.travel_chosen = false;
    Call vis(menu, L"GetVisibility");
    g.bench_vis = vis.go() ? vis.ri() : 4;
    bench_hide(menu);
    send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Rest}, clock::now_s()));
    log::info(L"EM bench menu hidden -> Lily's rest menu");
}

// The hidden bench menu never finishes its show animation (Hidden widgets do not tick), so the UMG navigation lock
// it takes is never released and EM's fast-travel map would get neither focus nor input
void unlock_em_navigation() {
    if (UObject* umg = ue::first(L"UMGSubsystem")) ue::call(umg, L"ResetNavigationLocks");
}

void focus_game_menu() {
    unlock_em_navigation();
    if (UObject* gm = ue::obj(g.bench_menu.get(), L"GameMenu")) ue::call(gm, L"SetFocus");
}

void bench_travel() {
    UObject* menu = g.bench_menu.get();
    if (menu && Call(menu, L"OpenGameMenu").i(kBenchFastTravelPage).go()) {
        focus_game_menu();
        g.travel = 1;
        log::info(L"Lily's fast travel -> EM fast-travel map");
        return;
    }
    send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Rest}, clock::now_s())); // nothing to show: wake Lily's menu again
}

// The fast-travel map was dismissed without a destination: back to Lily's rest menu
void bench_tick() {
    if (UObject* menu = g.bench_menu.get()) bench_hide(menu);
    if (g.travel == 0) return;
    const bool map_open = em_menu_open(L"WBP_GameMenu_C");
    if (g.travel == 1 && map_open) {
        g.travel = 2;
        focus_game_menu();
    }
    if (g.travel == 2 && !map_open) {
        g.travel = 0;
        if (g.bench_menu.get() && !g.travel_chosen) send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Rest}, clock::now_s()));
    }
}

void apply_ctx(UObject* pc, Ctx ctx) {
    if (ctx != g.ctx) log::info(L"input context %d -> %d", static_cast<int>(g.ctx), static_cast<int>(ctx));
    g.ctx = ctx;
    g.flags.ctx = static_cast<std::uint32_t>(ctx);
    g.flags.guest_pause = (ctx == Ctx::Gameplay || ctx == Ctx::LilyMenu || g.resting) ? 0u : 1u;
    const bool want = ctx == Ctx::LilyMenu || ctx == Ctx::Overlay; // EM world waits while Lily's menu is up
    if (want != g.paused_by_us) {
        Call(ue::gameplay_statics(), L"SetGamePaused").o(pc).b(want).go();
        g.paused_by_us = want;
    }
}

// ---- interaction ----

std::vector<UObject*> overlap_actors(UObject* pawn, const V3& at, double radius, UClass* filter) {
    std::vector<UObject*> out;
    Call c(ue::system_library(), L"SphereOverlapActors");
    if (!c || !filter) return out;
    std::vector<std::uint8_t> types;
    for (std::uint8_t i = 0; i < 32; ++i) types.push_back(i);
    std::vector<UObject*> ignore{pawn};
    c.o(pawn).v(at).f(radius).arr(types).o(filter).arr(ignore);
    ue::FProperty* outp = c.param(L"OutActors");
    if (!c.go() || !outp) return out;
    auto* arr = static_cast<ue::RawArray*>(c.slot(outp));
    for (std::int32_t i = 0; arr && arr->data && i < arr->num; ++i) {
        UObject* a = static_cast<UObject**>(arr->data)[i];
        if (ue::alive(a)) out.push_back(a);
    }
    return out;
}

void interact(UObject* pc, UObject* pawn) {
    const V3 at = ue::actor_location(pawn);
    UObject* best = nullptr;
    double best_d2 = kInteractRadius * kInteractRadius;
    for (UObject* it : overlap_actors(pawn, at, kInteractRadius, ue::find_class(L"/Script/Zion.Interactable"))) {
        const V3 l = ue::actor_location(it);
        const double d2 = (l.y - at.y) * (l.y - at.y) + (l.z - at.z) * (l.z - at.z);
        if (d2 < best_d2 && ue::call_bool(it, L"CanBeInteracted", true)) best = it, best_d2 = d2;
    }
    if (!best) return;
    const bool bench = ue::class_contains(best, L"RestPoint");
    if (bench) {
        g.resting = true; // the rest event starts inside OnInteract
        // seat Lily (both worlds, same offset: the mapping is unchanged) before EM's walk-to-bench starts
        const double dy = ue::actor_location(best).y - at.y;
        if (std::abs(dy) > kBenchSnapMin) {
            puppet_shift(pawn, dy);
            send_to_guest(Event::make(EventType::Shift, ShiftPayload{dy}, clock::now_s()));
        }
    }
    Call(best, L"OnInteract").o(pc).go();
    if (bench) send_to_guest(Event::make(EventType::Restore, LethalPayload{}, clock::now_s())); // FR-MENU-EM-4
    log::info(L"interacted with %s", ue::class_name(best).c_str());
}

// ---- in-world Lily quad (FR-CMP-1) ----

void build_quad(UObject* quad) {
    struct D3 { double x, y, z; };
    struct D2 { double x, y; };
    struct Tangent { D3 axis; bool flip; std::uint8_t pad[7]; };
    // unit quad in local Y (right) / Z (up), top-left at the origin; scaled per frame
    const std::vector<D3> verts{{0, 0, 0}, {0, 1, 0}, {0, 1, -1}, {0, 0, -1}};
    const std::vector<std::int32_t> tris{0, 2, 1, 0, 3, 2};
    const std::vector<D3> normals(4, D3{-1, 0, 0});
    const std::vector<D2> uvs{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::vector<ue::Color> colors(4, ue::Color{1, 1, 1, 1});
    const std::vector<Tangent> tangents(4, Tangent{{0, 1, 0}, false, {}});
    Call c(quad, L"CreateMeshSection_LinearColor");
    c.set_n(L"SectionIndex", 0).set_arr(L"Vertices", verts).set_arr(L"Triangles", tris).set_arr(L"Normals", normals);
    for (const wchar_t* uv : {L"UV0", L"UV1", L"UV2", L"UV3"}) c.set_arr(uv, uvs);
    c.set_arr(L"VertexColors", colors).set_arr(L"Tangents", tangents).set_b(L"bCreateCollision", false).go();
}

void ensure_world_layer(UObject* pawn) {
    UObject* world = ue::world_of(pawn);
    if (!g.rt) {
        Call c(ue::find(L"/Script/Engine.Default__KismetRenderingLibrary"), L"CreateRenderTarget2D");
        c.o(world).i(facts::kWorldRtWidth).i(facts::kWorldRtHeight).i(3 /*RTF_RGBA8_SRGB*/).c({0, 0, 0, 0}).b(false).b(false);
        if (c.go() && c.ro()) {
            c.ro()->SetRootSet();
            g.rt.set(c.ro());
        }
    }
    if (!g.mid && g.rt) {
        UObject* parent = ue::find(facts::em::kSpineMaterial);
        Call c(ue::find(L"/Script/Engine.Default__KismetMaterialLibrary"), L"CreateDynamicMaterialInstance");
        if (parent && c.o(world).o(parent).go() && c.ro()) {
            UObject* mid = c.ro();
            mid->SetRootSet();
            g.mid.set(mid);
            Call(mid, L"SetTextureParameterValue").n(L"SpriteTexture").o(g.rt.get()).go();
            Call(mid, L"SetVectorParameterValue").n(L"HighlightColor").c({0, 0, 0, 0}).go();
            Call(mid, L"SetScalarParameterValue").n(L"HighlightAlpha").f(0.0).go();
            Call(mid, L"SetScalarParameterValue").n(L"ColorOverrideAlpha").f(0.0).go();
        }
    }
    if (!g.quad && g.mid) {
        UClass* pmc = ue::find_class(L"/Script/ProceduralMeshComponent.ProceduralMeshComponent");
        Call c(pawn, L"AddComponentByClass");
        if (pmc && c.o(pmc).b(true).xf({}).b(false).go() && c.ro()) {
            UObject* quad = c.ro();
            g.quad.set(quad);
            build_quad(quad);
            Call(quad, L"SetMaterial").i(0).o(g.mid.get()).go();
            Call(quad, L"SetCollisionEnabled").i(0).go();
            Call(quad, L"SetCastShadow").b(false).go();
            // same translucency layer as Lilac's own Spine renderer
            const double prio = ue::num(ue::obj(pawn, L"SpineRendererComponent"), L"TranslucencySortPriority");
            Call(quad, L"SetTranslucentSortPriority").i(static_cast<std::int64_t>(prio)).go();
            for (const wchar_t* fn : {L"SetUsingAbsoluteLocation", L"SetUsingAbsoluteRotation", L"SetUsingAbsoluteScale"}) Call(quad, fn).b(true).go();
            log::info(L"world-layer quad attached to Lilac (sort priority %.0f)", prio);
        }
    }
}

void update_world_layer(UObject* pawn, const V3& lilac, bool show) {
    ensure_world_layer(pawn);
    UObject* quad = g.quad.get();
    FrameMeta fm{};
    const bool ok = show && quad && render::world_rt_captured() && ctl()->frame.read(fm) && fm.view_w_uu > 1.0f;
    render::set_world_layer(ok);
    if (!quad) return;
    Call(quad, L"SetVisibility").b(ok).b(false).go();
    if (!ok) return;
    const double w = fm.view_w_uu, h = fm.view_h_uu;
    const double foot = lilac.z - ue::num(ue::obj(pawn, L"CapsuleComponent"), L"CapsuleHalfHeight");
    const V3 origin{lilac.x - 1.0, lilac.y - fm.foot_u * w, foot + fm.foot_v * h};
    Call(quad, L"K2_SetWorldLocationAndRotation").v(origin).r({}).b(false).skip().b(true).go();
    Call(quad, L"SetWorldScale3D").v({1.0, w, h}).go();
}

void drain_guest_events(UObject* pc, UObject* pawn) {
    World* w = shm::world();
    if (!w) return;
    Event e;
    while (w->to_host.pop(e)) {
        switch (e.type) {
        case EventType::RestTravel: bench_travel(); break;
        case EventType::RestLeave:
            if (UObject* menu = g.bench_menu.get()) Call(menu, L"CloseMenu").go();
            break;
        default: combat_on_event(pc, pawn, e); break;
        }
    }
}

void deactivate(UObject* pc, UObject* pawn) {
    if (UObject* menu = g.bench_menu.get()) {
        Call(menu, L"SetVisibility").i(g.bench_vis).go(); // Lilac mode: EM's own bench menu is back
        send_to_guest(Event::make(EventType::CloseMenu, MenuPayload{}, clock::now_s()));
        g.bench_menu.reset();
    }
    if (pawn) {
        lock(pc, pawn, false);
        set_lilac_visible(pawn, true);
        if (g.hud_hidden) hide_lilac_hud(pc, false);
    }
    render::set_world_layer(false);
    puppet_reset();
    g.active = false;
}

} // namespace

// ================= shared helpers =================

UObject* lilac() { return g.pawn.get(); }
bool bridged() { return g.active; }
HostCounters& counters() { return g.k; }
void send_to_guest(const Event& e) {
    if (World* w = shm::world()) w->to_guest.push(e);
}

// ================= tick =================

void tick_pre(float) {
    Control* c = ctl();
    if (!c) return;
    const double now = clock::now_s();
    if (!g.names) {
        g.fn_construct = ue::fname(L"Construct");
        g.fn_destruct = ue::fname(L"Destruct");
        g.fn_page_changed = ue::fname(L"OnPageChanged");
        g.fn_travel_requested = ue::fname(L"OnFastTravelRequested");
        g.fn_close_menu = ue::fname(L"CloseMenu");
        for (const wchar_t* h : kBenchBlocked) g.bench_blocked.push_back(ue::fname(h));
        g.names = true;
    }
    c->host_pid.store(GetCurrentProcessId());
    c->host_tick_ms.store(clock::now_ms());
    // Hotkeys live on the render thread; HostFlags has a single writer (this thread)
    render::Toggles& tg = render::toggles();
    g.flags.bridge_on = tg.bridge_on.load() ? 1u : 0u;
    g.flags.show_lilac = tg.show_lilac.load() ? 1u : 0u;
    g.flags.debug_hud = tg.debug_hud.load() ? 1u : 0u;
    g.flags.quad_gain = tg.quad_gain.load();
    for (int n = tg.pause_requests.exchange(0); n > 0; --n) send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Pause}, now));
    for (const EquipPayload& e : render::take_equip_commands()) send_to_guest(Event::make(EventType::Equip, e, now));

    UObject* pc = g.pc.get();
    if (!pc && g.pc_lookup.due(now)) {
        g.pc.set(ue::first(L"PlayerControllerZion"));
        pc = g.pc.get();
    }
    UObject* pawn = pc ? ue::obj(pc, L"Pawn") : nullptr;
    if (!pawn || ue::class_name(pawn) != facts::em::kLilacClass) {
        if (g.active) deactivate(pc, nullptr);
        g.flags.ready = 0;
        g.flags.ctx = static_cast<std::uint32_t>(Ctx::Inactive);
        g.flags.guest_pause = 1;
        c->host_flags.store(g.flags);
        return;
    }
    if (g.pawn.get() != pawn) {
        if (g.pawn.get()) send_to_guest(Event::make(EventType::Restore, LethalPayload{}, now)); // EM respawned / reloaded: Lily restored
        g.pawn.set(pawn);
        g.quad.reset();
        g.locked = false;
        g.active = false;
        puppet_reset();
        terrain_reset();
        combat_reset();
    }

    const bool is_bridged = g.flags.bridge_on && guest_connected();
    const bool in_event = ue::call_bool(pc, L"IsInEvent");
    if (g.resting && !in_event && !em_menu_open(L"WBP_RestMenu_C")) g.resting = false;
    const bool lily_event = in_event && g.resting; // an EM bench rest is Lily's event: she kneels, Lilac stays hidden
    if (is_bridged && !g.active) {
        g.active = true;
        if (World* w = shm::world()) w->to_host.drain(); // never replay a previous session's hits
        puppet_request_rebase(pawn, "activation");
    }
    if (!is_bridged && g.active) deactivate(pc, pawn);

    const Ctx ctx = compute_ctx(pc, is_bridged, in_event);
    apply_ctx(pc, ctx);
    g.flags.ready = 1;
    g.flags.rest_pose = (g.active && g.resting) ? 1u : 0u;
    g.flags.lilac_half_height = static_cast<float>(ue::num(ue::obj(pawn, L"CapsuleComponent"), L"CapsuleHalfHeight"));

    const bool lily_shown = g.active && guest_ready() && (!in_event || lily_event);
    if (g.active) {
        lock(pc, pawn, true);
        set_lilac_visible(pawn, !lily_shown || g.flags.show_lilac);
        if (g.hud.due(now) || !g.hud_hidden) hide_lilac_hud(pc, lily_shown);

        terrain_tick(pc, pawn);
        const PuppetOut po = puppet_tick(pawn, ctx == Ctx::Gameplay);
        if (po.placed) {
            ++g.k.puppet_frames;
            g.puppet_err_max = std::max(g.puppet_err_max, po.err);
            g.k.puppet_err_max = g.puppet_err_max;
        }
        drain_guest_events(pc, pawn);
        bench_tick();
        // EVT_RestPoint gathers Lilac's spirits around the bench (BP_*_RestPoint_C); Lily's own spirits rest in EL
        if (g.resting && !g.flags.show_lilac && g.rest_spirits.due(now)) sweep_spirits(true);

        InputFrame in{};
        if (c->input.read(in)) {
            if (in.tap_seq[cmd::Interact] != g.seen_interact && ctx == Ctx::Gameplay) interact(pc, pawn);
            g.seen_interact = in.tap_seq[cmd::Interact];
        }
        // skip Lilac-only game-menu pages while the menu is open (EM ignores GoToPage during its opening animation)
        for (const ue::Weak& w : g.em_menus) {
            UObject* menu = w.get();
            if (ue::class_name(menu) == L"WBP_GameMenu_C") skip_lilac_page(menu, static_cast<int>(ue::call_num(menu, L"GetCurrentPageIndex")));
        }
    }
    combat_tick(pc, pawn, g.active);
    // stays on under Lily's menus: the render thread freezes the layer on menu frames (Lily kneels behind the menu)
    update_world_layer(pawn, ue::actor_location(pawn), lily_shown);
    c->host_flags.store(g.flags);
    if (g.counters_pub.due(now)) c->host_counters.store(g.k);
}

void on_begin_play(UObject* actor) {
    if (!ue::alive(actor)) return;
    if (g.locked && ue::is_a(actor, ue::find_class(L"/Script/Zion.CharacterZionSpirit"))) hide_spirit(actor, !g.flags.show_lilac);
    combat_on_begin_play(actor);
}

bool on_process_event_pre(UObject* ctx, ue::UFunction* fn, void*) {
    if (!g.names || !g.locked || !ctx || !fn) return false;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    if (n == g.fn_travel_requested && ctx == g.bench_menu.get()) {
        g.travel_chosen = true; // EM closes its map and the bench menu, then runs the fast travel
        return false;
    }
    // CloseMenu plays HideAnim and removes the menu when it finishes, but a Hidden widget never ticks its
    // animations: remove it directly (what the animation's OnFinish does), so the rest event continues
    if (n == g.fn_close_menu && ctx == g.bench_menu.get()) {
        Call(ctx, L"RemoveFromParent").go();
        return true;
    }
    if (std::ranges::find(g.bench_blocked, n) == g.bench_blocked.end() || !g.bench_menu.get() || ue::class_name(ctx) != kBenchMenu) return false;
    log::info(L"blocked EM bench entry %s", fn->GetName().c_str());
    return true;
}

void on_process_event_post(UObject* ctx, ue::UFunction* fn, void* parms) {
    if (!g.names || !ctx || !fn) return;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    if (n == g.fn_construct || n == g.fn_destruct) {
        const std::wstring cls = ue::class_name(ctx);
        if (std::ranges::none_of(kEmMenus, [&](const wchar_t* m) { return cls == m; })) return;
        if (n == g.fn_construct) {
            g.em_menus.emplace_back(ctx);
            if (cls == L"WBP_GameMenu_C" && g.locked) {
                for (const wchar_t* t : kLilacTabs) {
                    if (UObject* tab = ue::obj(ctx, t)) Call(tab, L"SetVisibility").i(1).go();
                }
                g.game_menu_page = static_cast<int>(ue::num(ctx, L"InitialPageIndex"));
            }
            if (cls == kBenchMenu && g.locked && g.active) bench_open(ctx);
        } else {
            std::erase_if(g.em_menus, [&](const ue::Weak& w) { return w.get() == ctx || !w.get(); });
            if (ctx == g.bench_menu.get()) {
                g.bench_menu.reset();
                unlock_em_navigation(); // its show-animation lock never ran out
                send_to_guest(Event::make(EventType::CloseMenu, MenuPayload{}, clock::now_s()));
            }
        }
        return;
    }
    if (n == g.fn_page_changed && g.locked && parms && ue::class_name(ctx) == L"WBP_GameMenu_C") skip_lilac_page(ctx, *static_cast<std::int32_t*>(parms));
}

std::string status() {
    Control* c = ctl();
    GuestStatus gs{};
    if (c) c->guest_status.read(gs);
    char buf[900];
    std::snprintf(buf, sizeof(buf),
                  "active=%d locked=%d ctx=%d guestReady=%u guestMenu=%u resting=%d bench=%d travel=%d rebase=%u mapped=%d puppetFrames=%u puppetErrMax=%.4f %s "
                  "dmgIntercepted=%u kbIntercepted=%u hpCorrections=%u hitsApplied=%u enemyDamage=%u quad=%d rt=%d",
                  g.active, g.locked, static_cast<int>(g.ctx), gs.ready, gs.menu_open, g.resting, g.bench_menu ? 1 : 0, g.travel, puppet_rebase_seq(), puppet_mapping_ready(), g.k.puppet_frames,
                  g.k.puppet_err_max, terrain_status().c_str(), g.k.dmg_intercepted, g.k.kb_intercepted, g.k.lilac_hp_corrections, g.k.hit_events_applied,
                  g.k.enemy_damage_calls, g.quad ? 1 : 0, render::world_rt_captured() ? 1 : 0);
    return buf;
}

void debug(const std::wstring& cmd) {
    UObject* pawn = g.pawn.get();
    if (cmd == L"rebase" && pawn) puppet_request_rebase(pawn, "debug");
    else if (cmd == L"menu") send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{}, clock::now_s()));
    else if (cmd == L"menu2") send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Spirits}, clock::now_s()));
    else if (cmd == L"menu3") send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Relics}, clock::now_s()));
    else if (cmd == L"menu4") send_to_guest(Event::make(EventType::OpenMenu, MenuPayload{MenuKind::Strengthen}, clock::now_s()));
    else if (cmd == L"restore") send_to_guest(Event::make(EventType::Restore, LethalPayload{}, clock::now_s()));
    else if (cmd == L"kill") combat_on_event(g.pc.get(), pawn, Event::make(EventType::Lethal, LethalPayload{}, clock::now_s()));
    else if (cmd == L"terrain") terrain_reset();
    else if (cmd == L"lilac") render::toggles().show_lilac = !render::toggles().show_lilac.load();
    else if (cmd == L"bridge") render::toggles().bridge_on = !render::toggles().bridge_on.load();
}

void scripted_input(float x, float y, std::uint32_t held, std::uint32_t taps, int frames) { input::script(x, y, held, taps, frames); }

} // namespace lie::host
