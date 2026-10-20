// Guest main tick (game thread). Lily's body is the only authority for her transform (FR-MOV-1):
// this file owns the EL<->EM mapping, publishes the pose ring / vitals, and settles Host requests.
//
// Lifecycle: title -> autoload slot 0 -> world ready -> anchor (studio, headless, unlocks) -> HOLD
// (movement disabled until the Host answered with a mapping and the mirrored terrain is placed) -> LIVE.

#include "guest/guest.h"

#include "core/el_items.h"
#include "core/facts.h"
#include "core/input_map.h"
#include "core/pose_ring.h"
#include "core/projection.h"
#include "rt/clock.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <algorithm>
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
// The rest menu keeps its first four entries (fast travel / spirits / relics / strengthen, FR-MENU-EL-2);
// save / leave / recollection / challenge are EM world business (leaving = cancel)
constexpr const wchar_t* kHiddenRestWidgets[] = {
    L"RestButton_Save", L"RestButton_Leave", L"RestButton_Recollection", L"RestButton_Challenge", L"HorizontalBox_Challenge", L"NewRecollectionIcon",
    L"BG", // full-screen dimmer meant for EL's own world: the EM world behind stays as it is
};
constexpr const wchar_t* kBlockedHandlers[] = {
    L"BndEvt__WBP_RestMenu_RestButton_Challenge_K2Node_ComponentBoundEvent_4_OnClicked__DelegateSignature",
    L"BndEvt__WBP_RestMenu_RestButton_Recollection_K2Node_ComponentBoundEvent_1_OnClicked__DelegateSignature",
    L"BndEvt__RestButton_Save_K2Node_ComponentBoundEvent_2_OnClicked__DelegateSignature",
    L"BndEvt__RestButton_Leave_K2Node_ComponentBoundEvent_8_OnClicked__DelegateSignature",
    // EL's Esc menu only opens on F5 (MenuKind::Pause -> PC_Base::TryPause). PC_Base's own entries: the Pause input
    // action (a forwarded Esc in any EL menu would open it), losing window focus (EL never has it) and the deferred
    // pause after a map load.
    L"InpActEvt_Pause_K2Node_InputActionEvent_4",
    L"OnApplicationWillDeactivate",
    L"TriggerOrDeferPause",
    // PC_Base's other world-input actions (DefaultInput.ini: MainMenu = Tab, Minimap = Q, DEBUG = One): Tab / Q are
    // Lily-menu keys (Context / Previous, EM's layout) pressed into EL's menus, and EL's main menu / minimap are not
    // opened by keys at all (the Host asks for menus by event; EM owns the map)
    L"InpActEvt_MainMenu_K2Node_InputActionEvent_5",
    L"InpActEvt_Minimap_K2Node_InputActionEvent_0",
    L"InpActEvt_DEBUG_K2Node_InputActionEvent_3",
    L"InpActEvt_DEBUG_Modifier_K2Node_InputActionEvent_2",
    L"InpActEvt_DEBUG_Modifier_K2Node_InputActionEvent_1",
};

// Settings pages EL must not show in Lily mode: tab, page. Graphics: EM owns video, EL's window is the Host's
// off-screen render target. The key binding pages stay: they are where Lily's action keys are set.
constexpr struct {
    const wchar_t* tab;
    const wchar_t* page;
} kPoppedSettings[] = {
    {L"GraphicsTab", L"WBP_Settings_Graphics"},
};
// Rows of EL's key binding pages that never apply in Lily mode (docs/INPUT_MATRIX.md), by widget name in the
// page's WidgetTree [CUE4P]: interact (EM's key), minimap / main menu / camera (EM keeps them), and the whole UI
// section (EL's menus are bound to EM's UI keys). Lily's movement rows stay.
constexpr struct {
    const wchar_t* page;
    const wchar_t* widget;
} kHiddenBindingRows[] = {
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_1"},  // Interact
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_12"}, // Minimap
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_16"}, // Main menu
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_27"}, // Camera Up
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_28"}, // Camera Down
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_29"}, // Camera Left
    {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindingEntry_C_30"}, // Camera Right
    {L"WBP_KeyBindings_Keyboard", L"HorizontalBox_174"},        // UI section title
    {L"WBP_KeyBindings_Keyboard", L"WrapBox_UIInputs"},
    {L"WBP_KeyBindings_Gamepad", L"WBP_KeyBindingEntry_C_1"},   // Interact
    {L"WBP_KeyBindings_Gamepad", L"WBP_KeyBindingEntry_C_8"},   // Minimap
};
// Rows of Lily's actions per conflict bit (input::conflict_bit: cmd 1..8, movement 16..19) [CUE4P]; the same
// widget names on both pages, the gamepad page has no movement rows
constexpr struct {
    std::uint32_t bit;
    const wchar_t* widget;
} kBindingRows[] = {
    {1u << cmd::Jump, L"WBP_KeyBindingEntry"},       {1u << cmd::Dodge, L"WBP_KeyBindingEntry_C"},
    {1u << cmd::Attack, L"WBP_KeyBindingEntry_C_2"}, {1u << cmd::Sub1, L"WBP_KeyBindingEntry_C_3"},
    {1u << cmd::Sub2, L"WBP_KeyBindingEntry_C_4"},   {1u << cmd::SummonSwitch, L"WBP_KeyBindingEntry_C_5"},
    {1u << cmd::Hook, L"WBP_KeyBindingEntry_C_6"},   {1u << cmd::Heal, L"WBP_KeyBindingEntry_C_7"},
    {1u << 16, L"WBP_KeyBindingEntry_C_8"},          {1u << 17, L"WBP_KeyBindingEntry_C_9"},
    {1u << 18, L"WBP_KeyBindingEntry_C_10"},         {1u << 19, L"WBP_KeyBindingEntry_C_11"},
};
constexpr const wchar_t* kBindingPages[2] = {L"WBP_KeyBindings_Keyboard", L"WBP_KeyBindings_Gamepad"};
// WBP_Settings_C handlers of the pages' OnKeyBindingModeChanged(KeyBindingEntry, bool IsSelectingKey @ +8)
constexpr const wchar_t* kKeySelectHandlers[] = {
    L"BndEvt__WBP_KeyBindings_Keyboard_K2Node_ComponentBoundEvent_7_OnKeyBindingModeChanged__DelegateSignature",
    L"BndEvt__WBP_KeyBindings_Gamepad_K2Node_ComponentBoundEvent_6_OnKeyBindingModeChanged__DelegateSignature",
};
constexpr std::size_t kIsSelectingKeyOffset = 8;
// Scoped to one widget class: these generated handler names are not globally unique (other widgets
// bind members named WBP_DefaultButton etc. the same way). WBP_Settings_Graphics (F5 -> settings):
// EM owns video settings; EL's window is a fixed off-screen render target the Host scales, so nothing
// on this page may commit. Option changes only stage values — Apply / Reset write back.
constexpr struct {
    const wchar_t* fn;
    const wchar_t* cls;
} kBlockedScoped[] = {
    {L"BndEvt__WBP_DefaultButton_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature", L"WBP_Settings_Graphics_C"},                        // 套用新设定
    {L"BndEvt__WBP_Settings_Graphics_WBP_DefaultButton_ResetToDefault_K2Node_ComponentBoundEvent_9_OnClicked__DelegateSignature", L"WBP_Settings_Graphics_C"}, // reset -> 1024x768
    {L"BndEvt__Resolution_K2Node_ComponentBoundEvent_5_OnOptionChanged__DelegateSignature", L"WBP_Settings_Graphics_C"},
    {L"BndEvt__DisplayMode_K2Node_ComponentBoundEvent_2_OnOptionChanged__DelegateSignature", L"WBP_Settings_Graphics_C"},
    {L"BndEvt__WBP_Settings_Graphics_ResolutionScale_K2Node_ComponentBoundEvent_8_OnValueChanged__DelegateSignature", L"WBP_Settings_Graphics_C"},
};
// Fast travel is EM's: the entry stays (always enabled) and opens EM's fast-travel map instead of EL's
constexpr const wchar_t* kTravelHandler = L"BndEvt__RestButton_Map_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature";
constexpr const wchar_t* kRestMenu = L"WBP_RestMenu_C";
// [SDK] EUINavigation
constexpr int kNavUp = 2, kNavDown = 3;
// EL's rest point is never used: its menu is titled with this name instead
constexpr const wchar_t* kRestTitle = L"\u70DF\u5C18\u4E4B\u56FD"; // 烟尘之国

// EM bench rest (EVT_ACT_CompanionRestStart): the current set's spirits stand at the rest point's
// SpiritLocation_1..3 [live BP_Interactable_RestingPoint: -170 / +170 / +340 UU from the kneeling spot,
// on the floor, the first facing right] and loop their "rest" animation on track 9
constexpr struct {
    double dy;
    bool facing_right;
} kRestSpirits[] = {{-170.0, true}, {170.0, false}, {340.0, false}};
// [POLICY] while resting the EL camera looks 280 UU left of Lily, so she and her spirits sit in the right part of
// the frame (UV ~0.47..0.97) and never meet the rest menu on the left (UV < 0.43). The world layer follows
// her projected foot, so the shift is invisible in EM.
constexpr double kRestCameraY = -280.0;
// world rect around the kneeling Lily and her spirits (UU from her foot) for FrameKind::Split
constexpr double kSplitLeft = 330.0, kSplitRight = 540.0, kSplitUp = 430.0, kSplitDown = 40.0;

// [POLICY] recenter before Lily leaves 40 % of the enlarged camera clamp volume (500000 UU, g_stage.cpp)
constexpr double kRecenterRadius = 200000.0;
// [POLICY] studio height above the loaded EL map: far outside any EL room yet well inside the enlarged camera
// clamp volume (500000) and the recenter radius, so the clamp keeps working and no recenter is triggered.
constexpr double kStudioLift = 60000.0;

struct State {
    ue::Weak pc, pawn;
    UObject* armed_pawn = nullptr;
    UObject* anchored_pawn = nullptr;
    bool anchored = false, held = false, paused = false, rest_pose = false, in_title = false;
    MapState map{};
    V3 origin{};
    std::vector<ue::Weak> menus;
    pose::FrameTimer<120> frame_timer;
    GuestCounters k{};
    clock::Every title_try{1.5}, title_check{0.5}, pc_lookup{1.0}, slow{0.25}, counters_pub{0.25}, res_poll{0.5}, ui_keys{2.0}, cursor_pin{1.0};
    std::uint32_t res_w = 0, res_h = 0; // last frame size the Host asked for (guest_res_w/h)
    int wake_taps = 0;
    double next_wake = 0.0;
    ue::Weak dormant;               // rest menu hidden while EM's fast-travel map is up
    std::int64_t dormant_vis = 4;
    bool closing = false;           // the Host closed the rest menu: no RestLeave echo
    FrameKind frame_kind = FrameKind::World;
    ue::Weak camera;
    float foot_u = 0.5f, foot_v = 0.5f, view_w = 0.0f, view_h = 0.0f; // last projection
    RC::Unreal::FName fn_construct{}, fn_destruct{}, fn_travel{};
    std::vector<RC::Unreal::FName> blocked, fn_key_select;
    bool selecting_key = false; // EL's key binding page waits for a key
    int publish_now = 0;        // frames left to publish EL's bindings at once (a rebinding just landed)
    ue::Weak settings;          // live WBP_Settings_C
    std::uint32_t tinted[2] = {~0u, ~0u}; // conflict masks last applied to the keyboard / gamepad page
    clock::Every el_keys{2.0};
    ElBindings el_bindings{};
    std::vector<std::pair<RC::Unreal::FName, std::wstring>> blocked_scoped;
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
    g.frame_kind = FrameKind::World;
    const float none[4]{};
    stage_menu(FrameKind::World, none, 1.0f);
    g.map = {};
    stage_reset();
    terrain_reset();
    combat_reset();
    capture_reset();
    hud_reset();
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

void set_camera_offset(double y) {
    if (!g.camera) g.camera.set(ue::first(L"BP_GameplayCamera_C"));
    UObject* cam = g.camera.get();
    if (!cam) return;
    for (const wchar_t* settings : {L"DefaultSettings", L"CurrentSettings"}) {
        ue::FProperty* sp = ue::prop(cam->GetClassPrivate(), settings);
        ue::FProperty* off = ue::member_of(sp, L"WorldSpaceLocationOffset");
        if (sp && off) ue::write_vec(reinterpret_cast<std::uint8_t*>(cam) + sp->GetOffset_Internal() + off->GetOffset_Internal(), off->GetSize(), {0.0, y, 0.0});
    }
}

std::vector<UObject*> companions(UObject* pc) {
    std::vector<UObject*> out;
    Call list(ue::obj(pc, L"SpiritCompanionComponent"), L"GetOrderedCurrentCompanions");
    if (!list.go() || !list.ret_prop()) return out;
    const auto* arr = static_cast<const ue::RawArray*>(list.slot(list.ret_prop()));
    for (std::int32_t i = 0; arr->data && i < arr->num && out.size() < std::size(kRestSpirits); ++i) {
        UObject* c = static_cast<UObject**>(arr->data)[i];
        if (ue::alive(c)) out.push_back(c);
    }
    return out;
}

// The studio keeps the spirit companions hidden; at an EM bench they gather and rest like at an EL rest point
void rest_spirits(UObject* pc, UObject* pawn, bool on) {
    const V3 at = ue::actor_location(pawn);
    const double foot = at.z - ue::num(ue::obj(pawn, L"CapsuleComponent"), L"CapsuleHalfHeight");
    const std::vector<UObject*> list = companions(pc);
    for (std::size_t i = 0; i < list.size(); ++i) {
        UObject* c = list[i];
        Call(c, L"SetActorHiddenInGame").b(!on).go();
        if (!on) {
            ue::call(c, L"ClearIdleAnimationOverride");
            continue;
        }
        if (UObject* mask = ue::obj(c, L"LightEmulatorMask")) Call(mask, L"SetHiddenInGame").b(true).b(true).go(); // dark halo on the studio
        const double hh = ue::num(ue::obj(c, L"CapsuleComponent"), L"CapsuleHalfHeight");
        Call(c, L"SetMoveTargetWithTime").o(nullptr).v({at.x, at.y + kRestSpirits[i].dy, foot + hh}).f(0.5).b(true).b(kRestSpirits[i].facing_right).go();
        Call(c, L"SetIdleAnimationOverride").s(L"rest").go();
        Call(ue::obj(c, L"SpineAnimationComponent"), L"PlayAnimation").s(L"rest").i(9).b(true).f(0.0).f(0.0).b(false).go();
    }
    if (!on) ue::call(ue::obj(pc, L"SpiritCompanionComponent"), L"ResetCurrentSetStateAndLocation");
}

void rest(UObject* pc, UObject* pawn, bool on) {
    kneel(pawn, on);
    rest_spirits(pc, pawn, on);
    set_camera_offset(on ? kRestCameraY : 0.0);
}

// Menu-only frames: Lily (and her resting spirits) leave the exported frame
void hide_rest_actors(UObject* pc, UObject* pawn, bool hidden) {
    Call(pawn, L"SetActorHiddenInGame").b(hidden).go();
    if (!g.rest_pose) return;
    for (UObject* c : companions(pc)) Call(c, L"SetActorHiddenInGame").b(hidden).go();
}

FrameKind frame_kind(bool menu_open) {
    if (!menu_open || !host_alive()) return FrameKind::World;
    int visible = 0;
    bool rest_only = true;
    for (const ue::Weak& w : g.menus) {
        UObject* m = w.get();
        if (!m || m == g.dormant.get()) continue;
        ++visible;
        rest_only = rest_only && ue::class_name(m) == kRestMenu;
    }
    return (g.rest_pose && visible == 1 && rest_only) ? FrameKind::Split : FrameKind::Menu;
}

void publish_frame_kind(UObject* pc, UObject* pawn, FrameKind kind) {
    if ((kind == FrameKind::Menu) != (g.frame_kind == FrameKind::Menu)) hide_rest_actors(pc, pawn, kind == FrameKind::Menu);
    g.frame_kind = kind;
    float split[4]{};
    if (kind == FrameKind::Split && g.view_w > 1.0f && g.view_h > 1.0f) {
        split[0] = g.foot_u - static_cast<float>(kSplitLeft) / g.view_w;
        split[1] = g.foot_v - static_cast<float>(kSplitUp) / g.view_h;
        split[2] = g.foot_u + static_cast<float>(kSplitRight) / g.view_w;
        split[3] = g.foot_v + static_cast<float>(kSplitDown) / g.view_h;
    }
    // WBP_*_INST animate the root widget's ColorAndOpacity; the Host needs it because the collapsed BG
    // means the rest menu's own dimming is synthesized rather than keyed out of the frame
    float alpha = 1.0f;
    if (kind != FrameKind::World) {
        for (const ue::Weak& w : g.menus) {
            UObject* m = w.get();
            if (m && m != g.dormant.get()) {
                if (const float* c = ue::field<float>(m, L"ColorAndOpacity")) alpha = c[3];
                break;
            }
        }
    }
    stage_menu(kind, split, alpha);
}

UObject* active_child(UObject* menu, const wchar_t* switcher) { return ue::call_obj(ue::obj(menu, switcher), L"GetActiveWidget"); }

void tidy_rest_menu(UObject* menu) {
    for (const wchar_t* b : kHiddenRestWidgets) {
        if (UObject* w = ue::obj(menu, b)) Call(w, L"SetVisibility").i(1 /*Collapsed*/).go();
    }
    // EL's own fast-travel unlock does not apply: EM decides inside its map
    Call(ue::obj(menu, L"WidgetSwitcher_RestButton_Map"), L"SetActiveWidgetIndex").i(0).go();
    // the four entries wrap around instead of navigating into the hidden ones
    UObject* top = ue::obj(menu, L"RestButton_Map");
    UObject* bottom = active_child(menu, L"WidgetSwitcher_Strengthen");
    if (top && bottom) {
        Call(top, L"SetNavigationRuleExplicit").i(kNavUp).o(bottom).go();
        Call(bottom, L"SetNavigationRuleExplicit").i(kNavDown).o(top).go();
    }
}

UObject* rest_menu() {
    for (const ue::Weak& w : g.menus) {
        if (ue::class_name(w.get()) == kRestMenu) return w.get();
    }
    return nullptr;
}

// Fast travel: the rest menu sleeps (Lily stays seated) while EM's fast-travel map is on screen
void rest_sleep(UObject* menu) {
    Call vis(menu, L"GetVisibility");
    g.dormant_vis = vis.go() ? vis.ri() : 4;
    Call(menu, L"SetVisibility").i(1 /*Collapsed*/).go();
    g.dormant.set(menu);
    if (World* w = shm::world()) w->to_host.push(Event::make(EventType::RestTravel, MenuPayload{}, clock::now_s()));
    log::info(L"rest menu asleep: EM fast travel");
}

bool rest_wake() {
    UObject* menu = g.dormant.get();
    if (!menu) return false;
    g.dormant.reset();
    Call(menu, L"SetVisibility").i(g.dormant_vis).go();
    if (UObject* btn = ue::obj(menu, L"RestButton_Map")) ue::call(btn, L"SetFocus");
    log::info(L"rest menu awake");
    return true;
}

bool rest_close() {
    UObject* menu = rest_menu();
    if (!menu) return false;
    g.dormant.reset();
    g.closing = true;
    Call(menu, L"CloseMenu").b(false).go(); // RemoveMenu -> RemoveFromParent
    g.closing = false;
    log::info(L"rest menu closed by the EM bench");
    return true;
}

// The hidden Leave button can be EL's remembered focus: give focus to the first remaining entry so forwarded keys navigate
void keep_menu_focus() {
    std::erase_if(g.menus, [](const ue::Weak& w) { return !w.get(); });
    if (g.menus.size() != 1) return;
    UObject* menu = g.menus.front().get();
    if (menu == g.dormant.get() || ue::class_name(menu) != kRestMenu || ue::call_bool(menu, L"HasFocusedDescendants")) return;
    for (const wchar_t* b : {L"RestButton_Map", L"RestButton_Equipment", L"RestButton_Equipment_dummy"}) {
        UObject* btn = ue::obj(menu, b);
        if (btn && ue::call_bool(btn, L"IsVisible")) {
            ue::call(btn, L"SetFocus");
            return;
        }
    }
}

// ---- direct menus ----

// Lily's menus skip EL's sit-down event entirely (Lily already kneels on the EM bench) and are spawned
// straight into the viewport. This mirrors the native rest-menu button handlers (WBP_RestMenu ubergraph):
// SetCanChangeEquipment -> Create -> SetIntPropertyByName(DefaultPageIndex) -> AddToViewport(10).
// DefaultPageIndex is ExposeOnSpawn and read by WBP_MainMenu::Construct, so it must be written
// between Create and AddToViewport. WBP_RestMenu itself needs nothing from the event (no EventContext);
// its Construct only titles it with EL's map name, which is replaced by the EM bench's name.
constexpr const wchar_t* kRestMenuClass = L"/Game/_Zenith/UI/Rest/WBP_RestMenu.WBP_RestMenu_C";
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

void set_rest_title(UObject* menu, const wchar_t* title) {
    Call conv(ue::find(L"/Script/Engine.Default__KismetTextLibrary"), L"Conv_StringToText");
    if (conv.s(title).go()) Call(ue::obj(menu, L"RestPointName"), L"SetText").copy(conv.slot(conv.ret_prop())).go();
}

void open_rest_menu(UObject* pc) {
    if (rest_wake() || rest_menu()) return;
    UObject* menu = spawn_menu(pc, kRestMenuClass, -1);
    if (!menu) {
        log::warn(L"failed to open Lily's rest menu");
        return;
    }
    set_rest_title(menu, kRestTitle);
    log::info(L"opened Lily's rest menu");
}

void open_menu(UObject* pc, MenuKind kind) {
    if (kind == MenuKind::Rest) {
        open_rest_menu(pc);
        return;
    }
    if (!g.menus.empty()) return; // never stack a second EL menu
    if (kind == MenuKind::Pause) {
        ue::call(pc, L"TryPause"); // native path: CanOpenMenu -> WBP_Pause -> input action set 2
        log::info(L"opened Lily's Esc menu");
        return;
    }
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

void drain_events(UObject* pc, UObject* pawn, V3& loc) {
    World* w = shm::world();
    if (!w) return;
    Event e;
    while (w->to_guest.pop(e)) {
        switch (e.type) {
        case EventType::Hurt: combat_on_hurt(pawn, e.as<HurtPayload>()); break;
        case EventType::Restore: full_restore(pc, pawn); break;
        case EventType::OpenMenu: open_menu(pc, e.as<MenuPayload>().kind); break;
        case EventType::Equip: apply_equip(pc, pawn, e.as<EquipPayload>()); break;
        case EventType::CloseMenu: rest_close(); break;
        case EventType::Shift: // same offset as the puppet: the mapping stays valid
            loc.y += e.as<ShiftPayload>().dy;
            ue::set_actor_location(pawn, loc);
            if (UObject* sc = ue::obj(pc, L"SpiritCompanionComponent")) ue::call(sc, L"ResetCurrentSetStateAndLocation");
            break;
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
    g.foot_u = static_cast<float>(s.u);
    g.foot_v = static_cast<float>(s.v);
    g.view_w = static_cast<float>(w);
    g.view_h = static_cast<float>(w / aspect);
    stage_frame(g.foot_u, g.foot_v, g.view_w, g.view_h);
    capture_tick(pc); // SceneCapture alpha channel: same POV drives the second shared texture
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

// EL's UI bindings follow EM's live UI keys (Control::ui_layout, EM's defaults until the Host has read them), so
// EL's prompts show what the player presses (docs/INPUT_MATRIX.md); navigation stays on EL's arrows / d-pad.
// UMGSubsystem::OverrideUIInput replaces the binding of the key's device only (keyboard or pad). Checked against
// the key EL *displays* (GetKeyForUIInput): VALIDATION / CANCEL also accept Enter / Escape by default but show
// Z / X. Re-asserted because loading EL's settings brings its own bindings back once after boot.
void sync_el_menu_keys() {
    UObject* umg = ue::first(L"UMGSubsystem");
    if (!umg) return;
    struct KeyArg { // FKey: FName KeyName + TSharedPtr<FKeyDetails> (null: resolved by name)
        RC::Unreal::FName name;
        void* details[2]{};
    };
    UiLayout l{};
    const bool live = ctl() && ctl()->ui_layout.read(l) && l.valid;
    for (int i = 0; i < menu::Count; ++i) {
        const std::uint8_t type = input::kElUiType[i];
        if (!type) continue;
        for (const bool pad : {false, true}) {
            const char* name = pad ? (live ? l.pad[i] : "") : (live && l.kb[i][0] ? l.kb[i] : input::kElMenuKbDefault[i]);
            if (!name[0]) continue;
            const std::wstring wname(name, name + std::strlen(name));
            const KeyArg key{ue::fname(wname.c_str())};
            Call shown(umg, L"GetKeyForUIInput");
            ue::FProperty* rv = shown.param(L"ReturnValue");
            if (shown.i(type).b(pad).go() && rv && static_cast<const KeyArg*>(shown.slot(rv))->name == key.name) continue;
            Call(umg, L"OverrideUIInput").i(type).copy(&key).go();
            log::info(L"EL UI key %d (%s) -> %s", type, pad ? L"pad" : L"keyboard", wname.c_str());
        }
    }
}

// Lily's action keys = the player's EL bindings (EL's key binding pages): InputSettingsSubsystem
// GetInputChordsForAction / GetInputChordsForAxis -> Control::el_bindings, composed by the Host with EM's keys.
// Chords with Shift / Ctrl / Alt / Cmd are skipped (EL's pages do not make them; the Host matches single keys).
void read_chords(UObject* settings, const wchar_t* fn, const wchar_t* name, const float* scale, Input kb[2], Input pad[2]) {
    struct KeyArg {
        RC::Unreal::FName name;
        void* details[2]{};
    };
    Call c(settings, fn);
    ue::FProperty* rv = c.param(L"ReturnValue");
    ue::FProperty* inner = rv ? ue::array_inner(rv) : nullptr;
    c.n(name);
    if (scale) c.f(*scale);
    if (!inner || !c.go()) return;
    const auto* arr = static_cast<const ue::RawArray*>(c.slot(rv));
    const std::int32_t stride = inner->GetElementSize();
    int nk = 0, np = 0;
    for (std::int32_t i = 0; arr && arr->data && i < arr->num; ++i) {
        const auto* e = static_cast<const std::uint8_t*>(arr->data) + static_cast<std::ptrdiff_t>(i) * stride;
        if (e[sizeof(KeyArg)] & 0x0F) continue; // FInputChord bShift / bCtrl / bAlt / bCmd follow the FKey
        std::string key;
        for (const wchar_t ch : ue::str(reinterpret_cast<const KeyArg*>(e)->name)) key.push_back(static_cast<char>(ch));
        const Input in = input::input_from_name(key.c_str());
        if (in.vk && nk < 2) kb[nk++] = in;
        else if (!in.vk && in.valid() && np < 2) pad[np++] = in;
    }
}

void publish_el_bindings() {
    UObject* s = ue::first(L"InputSettingsSubsystem");
    UObject* save = ue::first(L"SaveSubsystem");
    Control* c = ctl();
    // before EL's settings are loaded the subsystem holds DefaultInput.ini, not the player's bindings
    if (!s || !c || !save || !ue::call_bool(save, L"IsSettingsLoaded")) return;
    ElBindings b{};
    b.valid = 1;
    for (int k = 1; k < cmd::Count; ++k) {
        if (!input::kElAction[k]) continue;
        const std::string a = input::kElAction[k];
        read_chords(s, L"GetInputChordsForAction", std::wstring(a.begin(), a.end()).c_str(), nullptr, b.kb[k], b.pad[k]);
    }
    for (int m = 0; m < 4; ++m) {
        Input pad[2]{};
        const std::string a = input::kElMoveAxis[m].axis;
        read_chords(s, L"GetInputChordsForAxis", std::wstring(a.begin(), a.end()).c_str(), &input::kElMoveAxis[m].scale, b.kb_move[m], pad);
    }
    if (!(b == g.el_bindings)) log::info(L"EL bindings published");
    g.el_bindings = b;
    c->el_bindings.store(b);
}

// A widget inside one of WBP_Settings_C's pages, by its name in that page's WidgetTree
UObject* page_widget(UObject* settings, const wchar_t* page_name, const wchar_t* widget) {
    UObject* page = ue::obj(settings, page_name);
    if (!page) return nullptr;
    const std::wstring full = page->GetFullName(); // "<Class> <path>"
    return ue::find(full.substr(full.find(L' ') + 1) + L".WidgetTree." + widget);
}

// Lily bindings EM overrides show where they are set: the row turns red on EL's key binding page (the Host lists
// them next to it). Returns which binding page is on screen (GuestStatus::binding_page).
std::uint32_t binding_pages_tick() {
    UObject* s = g.settings.get();
    Control* c = ctl();
    if (!s || !c) return 0;
    const std::uint32_t masks[2] = {c->conflicts_kb.load(), c->conflicts_pad.load()};
    for (int p = 0; p < 2; ++p) {
        if (masks[p] == g.tinted[p]) continue;
        bool found = false;
        for (const auto& r : kBindingRows) {
            UObject* w = page_widget(s, kBindingPages[p], r.widget);
            if (!w) continue;
            const bool bad = (masks[p] & r.bit) != 0;
            const ue::Color col = bad ? ue::Color{1.0f, 0.32f, 0.28f, 1.0f} : ue::Color{1.0f, 1.0f, 1.0f, 1.0f};
            found = Call(w, L"SetColorAndOpacity").copy(&col).go() || found;
        }
        if (found) g.tinted[p] = masks[p];
    }
    UObject* active = ue::call_obj(ue::obj(s, L"SettingsSwitcher"), L"GetActiveWidget");
    for (int p = 0; p < 2; ++p) {
        if (active && active == ue::obj(s, kBindingPages[p])) return static_cast<std::uint32_t>(p + 1);
    }
    return 0;
}

// ================= tick =================

void tick() {
    Control* c = ctl();
    if (!c) return;
    const double now = clock::now_s();
    if (!g.names) {
        g.fn_construct = ue::fname(L"Construct");
        g.fn_destruct = ue::fname(L"Destruct");
        g.fn_travel = ue::fname(kTravelHandler);
        for (const wchar_t* h : kBlockedHandlers) g.blocked.push_back(ue::fname(h));
        for (const auto& h : kBlockedScoped) g.blocked_scoped.emplace_back(ue::fname(h.fn), h.cls);
        for (const wchar_t* h : kKeySelectHandlers) g.fn_key_select.push_back(ue::fname(h));
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
    // Headless: EL never owns a mouse, but states keep re-raising bShowMouseCursor. Left on, Slate draws a
    // software cursor tracking the OS cursor through the off-screen window -> a pointer trapped in the
    // top-right of the composited frame.
    if (pc && g.cursor_pin.due(now) && ue::flag(pc, L"bShowMouseCursor", false)) ue::set_flag(pc, L"bShowMouseCursor", false);
    // The Host publishes the EL frame size it wants (EM back buffer x its res scale). r.setres would
    // clamp to the desktop (1707x1067 logical on a 2560x1600/150% display), so we resize the off-screen
    // window raw: WM_SIZE -> swap chain ResizeBuffers -> shared textures + capture RT rebuilt.
    if (g.res_poll.due(now)) {
        const std::uint32_t rw = c->guest_res_w.load(), rh = c->guest_res_h.load();
        unsigned cur_w = 0, cur_h = 0;
        if (rw >= 64 && rh >= 64 && frame_dims(cur_w, cur_h)) {
            if (rw != cur_w || rh != cur_h) request_frame_size(rw, rh);
            // The window is resized raw, so GameUserSettings keeps its defaults (FullscreenMode=1
            // "borderless" + DesktopResolution) and EL's settings page lies; the engine also rewrites
            // FullscreenMode once the resize lands. Idempotent field sync wins either way.
            if (UObject* gs = ue::first(L"GameUserSettings")) {
                for (const wchar_t* n : {L"ScreenResolution", L"LastConfirmedScreenResolution"}) {
                    if (auto* p = static_cast<std::int32_t*>(ue::field_ptr(gs, n))) {
                        p[0] = static_cast<std::int32_t>(rw);
                        p[1] = static_cast<std::int32_t>(rh);
                    }
                }
                ue::set_num(gs, L"FullscreenMode", 2); // windowed: -windowed launch + off-screen window
                ue::set_num(gs, L"LastConfirmedFullscreenMode", 2);
            }
            if (rw != g.res_w || rh != g.res_h) {
                log::info(L"guest res -> %ux%u (window resize)", rw, rh);
                g.res_w = rw;
                g.res_h = rh;
            }
        }
    }
    if (g.ui_keys.due(now)) sync_el_menu_keys();
    if (g.el_keys.due(now) || g.publish_now > 0) {
        publish_el_bindings();
        if (g.publish_now > 0) --g.publish_now;
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
    hud_tick(pc); // after the capture: its RT grab goes first
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
            rest(pc, pawn, g.rest_pose);
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
    st.menu_open = std::ranges::any_of(g.menus, [](const ue::Weak& w) { return w.get() && w.get() != g.dormant.get(); }) ? 1u : 0u;
    publish_frame_kind(pc, pawn, frame_kind(st.menu_open != 0));
    if (!st.menu_open) g.selecting_key = false;
    st.selecting_key = g.selecting_key ? 1u : 0u;
    st.binding_page = st.menu_open ? binding_pages_tick() : 0u;
    st.half_height = static_cast<float>(hh);
    st.radius = static_cast<float>(radius);
    st.frame_dt_p95 = g.frame_timer.p95();
    c->guest_status.store(st);
    if (g.counters_pub.due(now)) publish_counters();
}

bool on_process_event_pre(UObject* ctx, ue::UFunction* fn) {
    if (!g.anchored || !g.names || !ctx || !fn) return false;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    if (n == g.fn_travel) {
        if (host_alive()) rest_sleep(ctx);
        return true;
    }
    for (const auto& h : g.blocked) {
        if (n == h) {
            log::info(L"blocked EL world-business button %s", fn->GetName().c_str());
            return true;
        }
    }
    for (const auto& h : g.blocked_scoped) {
        if (n == h.first && ue::class_name(ctx) == h.second) {
            log::info(L"blocked %s on %s", fn->GetName().c_str(), h.second.c_str());
            return true;
        }
    }
    return false;
}

void on_process_event_post(UObject* ctx, ue::UFunction* fn, void* parms) {
    if (!g.names || !ctx || !fn) return;
    const RC::Unreal::FName n = fn->GetNamePrivate();
    if (parms && std::ranges::find(g.fn_key_select, n) != g.fn_key_select.end()) {
        g.selecting_key = *(static_cast<const std::uint8_t*>(parms) + kIsSelectingKeyOffset) != 0;
        if (!g.selecting_key) g.publish_now = 3; // the Host recomposes (and reports a conflict) within a frame
        log::info(L"EL key binding: %s", g.selecting_key ? L"waiting for a key (raw keys)" : L"done");
        return;
    }
    const bool construct = n == g.fn_construct;
    if (!construct && n != g.fn_destruct) return;
    const std::wstring cls = ue::class_name(ctx);
    if (!construct && cls == L"WBP_Settings_C") g.selecting_key = false;
    if (construct && cls == L"WBP_Settings_C") {
        g.settings.set(ctx);
        g.tinted[0] = g.tinted[1] = ~0u;
        for (const auto& r : kHiddenBindingRows) {
            if (UObject* w = page_widget(ctx, r.page, r.widget)) Call(w, L"SetVisibility").i(1).go();
        }
        // Pop pages (kPoppedSettings) the way EL's own non-PC branch of SetupSettings does: tab + page
        // RemoveFromParent and drop the tab from TabArray, which SelectTab / GoToSideTab index (wrap over
        // TabArray.Length, switcher index == TabArray index). Collapsing alone leaves a blank page in the cycle.
        auto* arr = static_cast<ue::RawArray*>(ue::field_ptr(ctx, L"TabArray"));
        auto cur = static_cast<std::int32_t>(ue::num(ctx, L"CurrTabIndex"));
        bool popped = false;
        for (const auto& s : kPoppedSettings) {
            UObject* tab = ue::obj(ctx, s.tab);
            if (!tab || !arr || !arr->data) continue;
            auto** items = static_cast<UObject**>(arr->data);
            for (std::int32_t i = 0; i < arr->num; ++i) {
                if (items[i] != tab) continue;
                std::memmove(items + i, items + i + 1, sizeof(UObject*) * (arr->num - i - 1));
                --arr->num;
                if (cur > i) --cur;
                Call(tab, L"RemoveFromParent").go();
                if (UObject* page = ue::obj(ctx, s.page)) Call(page, L"RemoveFromParent").go();
                popped = true;
                break;
            }
        }
        if (popped) Call(ctx, L"SelectTab").i(cur >= 0 && cur < arr->num ? cur : 0).go();
    }
    bool menu = false;
    for (const wchar_t* m : kMenuClasses) menu = menu || cls == m;
    if (!menu) return;
    if (construct) {
        g.menus.emplace_back(ctx);
        if (cls == kRestMenu) tidy_rest_menu(ctx);
        // EL's map describes its own world — EM owns maps, so the category tab stays hidden (FR-MENU-EL-2)
        else if (cls == L"WBP_MainMenu_C") {
            if (UObject* tab = ue::obj(ctx, L"Map")) Call(tab, L"SetVisibility").i(1).go();
        }
    } else {
        std::erase_if(g.menus, [&](const ue::Weak& w) { return w.get() == ctx || !w.get(); });
        if (cls == kRestMenu) {
            if (ctx == g.dormant.get()) g.dormant.reset();
            if (!g.closing && host_alive()) { // the player left Lily's rest menu: the EM bench ends with it
                if (World* w = shm::world()) w->to_host.push(Event::make(EventType::RestLeave, MenuPayload{}, clock::now_s()));
            }
            if (!g.rest_pose) kneel(g.pawn.get(), false);
        }
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
    else if (cmd == L"menu" && pc) open_rest_menu(pc);
    else if (cmd == L"menu2" && pc) open_menu(pc, MenuKind::Spirits);
    else if (cmd == L"menu3" && pc) open_menu(pc, MenuKind::Relics);
    else if (cmd == L"menu4" && pc) open_menu(pc, MenuKind::Strengthen);
    else if (cmd == L"terrain") terrain_tick(ue::world_of(pc), g.map.map, g.origin.x, true);
}

} // namespace lie::guest
