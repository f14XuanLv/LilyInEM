// Black studio (FR-HDL-1/2/3/6): every EL actor that is not Lily, her spirits, their abilities, engine
// infrastructure or one of our own mirror objects is hidden and loses collision, once at anchor time and
// then incrementally from BeginPlay. EL world services (HUD, BGM, ambience, shake) are switched off and
// the EL window lives off-screen (never minimised: a minimised UE4 window stops presenting).

#include "guest/guest.h"

#include "core/facts.h"
#include "rt/log.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "rt/clock.h"
#include "rt/d3d.h"

#include <unordered_set>
#include <vector>

namespace lie::guest {

namespace {

using ue::Call;

std::unordered_set<const UObject*> g_owned;
ue::Weak g_pc, g_lily;
bool g_window_done = false;
clock::Every g_window_check{1.0};

// [UE/EL] Destroying BP_CameraClampVolume_C or disabling its collision triggers
// ZenithPlayerController::OnNoClampVolumeWarning (MOVE_Flying after 1.5 s, map reload after 5 s).
// Keep it alive and enlarge it instead; only its four blockers lose collision.
constexpr double kClampExtent = 500000.0;

void box_extent(UObject* box, const ue::V3& e) { Call(box, L"SetBoxExtent").v(e).b(true).go(); }

void keep_clamp_volume(UObject* cv) {
    Call(cv, L"SetActorHiddenInGame").b(true).go();
    box_extent(ue::obj(cv, L"ActivationBoxComponent"), {10000.0, kClampExtent, kClampExtent});
    box_extent(ue::obj(cv, L"ClampComponent"), {10000.0, kClampExtent, kClampExtent});
    for (const wchar_t* b : {L"Blocker_Up", L"Blocker_Down", L"Blocker_Left", L"Blocker_Right"}) {
        if (UObject* blocker = ue::obj(cv, b)) Call(blocker, L"SetCollisionEnabled").i(0).go();
    }
}

void lock_camera(UObject* cam) {
    if (UObject* cc = ue::obj(cam, L"CameraComponent")) ue::set_num(cc, L"PostProcessBlendWeight", 0.0);
    for (const wchar_t* settings : {L"DefaultSettings", L"CurrentSettings"}) {
        ue::FProperty* sp = ue::prop(cam->GetClassPrivate(), settings);
        ue::FProperty* off = ue::member_of(sp, L"SoloForwardWorldOffset");
        if (sp && off) ue::write_vec(reinterpret_cast<std::uint8_t*>(cam) + sp->GetOffset_Internal() + off->GetOffset_Internal(), off->GetSize(), {});
    }
}

bool owned_by(UObject* actor, UObject* a, UObject* b) {
    UObject* cur = actor;
    for (int depth = 0; depth < 5 && cur; ++depth) {
        UObject* next = ue::obj(cur, L"Owner");
        if (!next) next = ue::obj(cur, L"Instigator");
        if (!next) return false;
        if (next == a || next == b || ue::is_a(next, ue::find_class(L"/Script/Zenith.ZenithSpirit"))) return true;
        cur = next;
    }
    return false;
}

void silence(UObject* actor, bool keep_collision) {
    Call(actor, L"SetActorHiddenInGame").b(true).go();
    if (!keep_collision) Call(actor, L"SetActorEnableCollision").b(false).go();
}

void classify(UObject* actor) {
    UObject* lily = g_lily.get();
    UObject* pc = g_pc.get();
    if (!ue::alive(actor) || actor == lily || actor == pc || g_owned.contains(actor)) return;
    if (ue::is_a(actor, ue::find_class(L"/Script/Zenith.ZenithSpirit"))) return;
    const std::wstring cls = ue::class_name(actor);
    auto has = [&](const wchar_t* f) { return cls.find(f) != std::wstring::npos; };

    if (has(L"CameraClampVolume")) return keep_clamp_volume(actor);
    if (cls == L"BP_GameplayCamera_C") return lock_camera(actor);
    if (has(L"PostProcessVolume")) {
        ue::set_flag(actor, L"bEnabled", false);
        return;
    }
    if (has(L"WorldSettings")) {
        ue::set_flag(actor, L"bEnableWorldBoundsChecks", false);
        return;
    }
    if (has(L"Ability") || has(L"Projectile")) {
        // Lily's / spirits' abilities stay; anything else (EL enemies) never reaches Lily
        if (!owned_by(actor, lily, pc)) silence(actor, false);
        return;
    }
    if (owned_by(actor, lily, pc)) return;
    for (const wchar_t* keep : {L"Camera", L"GameMode", L"GameState", L"PlayerState", L"LevelScript", L"HUD", L"PhysicsVolume", L"GameSession",
                                L"GameNetworkManager", L"ParticleEventManager", L"NavData", L"Brush", L"WorldLoader"}) {
        if (has(keep)) return;
    }
    if (ue::is_a(actor, ue::find_class(L"/Script/Zenith.ZenithCharacter"))) {
        Call(actor, L"SetActorTickEnabled").b(false).go(); // EL enemies / companions: frozen and gone
        silence(actor, false);
        return;
    }
    // The EL rest point drives Lily's native kneel animation at EM benches: hidden, collision kept
    silence(actor, has(L"RestingPoint") || has(L"PlayerStart"));
}

} // namespace

bool stage_owns(const UObject* actor) { return g_owned.contains(actor); }
void stage_register_owned(UObject* actor) { g_owned.insert(actor); }

void stage_isolate_all(UObject* pc, UObject* lily) {
    g_pc.set(pc);
    g_lily.set(lily);
    std::vector<UObject*> actors;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("Actor"), actors); // once per anchor (FR-ENG-2)
    for (UObject* a : actors) classify(a);
    log::info(L"studio isolated: %zu actors scanned", actors.size());
}

void stage_on_begin_play(UObject* actor) {
    if (g_lily.get()) classify(actor);
}

void stage_headless(UObject* pc, UObject* lily) {
    for (const wchar_t* cmd : {L"r.BloomQuality 0", L"r.VSync 0", L"t.MaxFPS 60"}) ue::console(pc, cmd);
    if (UObject* gs = ue::first(L"GameSettingsSubsystem")) {
        for (const wchar_t* fn : {L"SetDisplayPlayerUI", L"SetDisplayDamageValues", L"SetDisplayHealValues", L"SetDisplayEnemyFloatingGauges",
                                  L"SetUseCameraShake", L"SetUseForceFeedback", L"SetEnableDamageAreaPostProcess"}) {
            Call(gs, fn).b(false).go();
        }
    }
    if (UObject* snd = ue::first(L"SoundSubsystem")) {
        Call(snd, L"SetVolume").i(facts::el::kSoundBgm).f(0.0).go();
        Call(snd, L"SetVolume").i(facts::el::kSoundAmbience).f(0.0).go();
    }
    // Lily's fake light / light emulator paint a dark halo on the black studio: off
    for (const wchar_t* comp : {L"FakeLight", L"LightEmulatorMask"}) {
        if (UObject* c = ue::obj(lily, comp)) {
            Call(c, L"SetVisibility").b(false).b(true).go();
            Call(c, L"SetHiddenInGame").b(true).b(true).go();
        }
    }
    if (UObject* le = ue::obj(lily, L"LightEmulator")) {
        ue::set_flag(le, L"bEnable", false);
        Call(le, L"SetComponentTickEnabled").b(false).go();
    }
}

void stage_window_tick(double now) {
    if (!g_window_check.due(now)) return;
    HWND hwnd = static_cast<HWND>(window());
    if (!hwnd) hwnd = d3d::find_game_window(); // game thread: does not depend on Present running
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE); // must keep presenting
    if (g_window_done) return;
    g_window_done = true;
    wchar_t buf[4]{};
    if (GetEnvironmentVariableW(L"LIE_SHOW_GUEST", buf, 4) && buf[0] == L'1') return;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW);
    RECT r{};
    GetWindowRect(hwnd, &r);
    SetWindowPos(hwnd, HWND_BOTTOM, GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 64, r.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    log::info(L"headless: EL window moved off-screen (LIE_SHOW_GUEST=1 keeps it visible)");
}

void stage_reset() {
    g_lily.reset();
    g_pc.reset();
}

} // namespace lie::guest
