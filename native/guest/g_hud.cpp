// EL HUD alpha channel (game thread): EL's own WBP_PlayerUI (HP gauge, prayers, level, special gauge, spirit
// decks, low-HP vignette) leaves the viewport for a UWidgetComponent. In world space its FWidgetRenderer paints
// the widget into our RTF_RGBA16f target cleared to transparent every tick: linear premult RGB + real coverage
// in A (Slate blends SrcAlpha / InvSrcAlpha, alpha One / InvSrcAlpha). g_frame grabs that RT's native texture
// and exports it as FrameMeta::hud_handle; the Host composites it in EM screen space. The HUD never reaches
// EL's back buffer again, so the world layer (bb.rgb / coverage) stays free of it.
// - the owner actor is hidden: no scene proxy, nothing in the studio or the capture; the component itself
//   stays IsVisible() (ShouldDrawWidget) and TickWhenOffscreen skips the "rendered recently" gate
// - the RT is ours (ClearColor transparent == ActualBackgroundColor, same size): UpdateRenderTarget never
//   re-inits it to its default PF_B8G8R8A8
// - SetWidget first, RemoveFromParent second: the virtual window keeps the Slate widget alive, so the UI's
//   Destruct / Construct (and its GameSettings delegate) never churn

#include "guest/guest.h"

#include "rt/clock.h"
#include "rt/log.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lie::guest {

namespace {

using ue::Call;

ue::Weak g_actor, g_comp, g_rt, g_ui;
std::int64_t g_w = 0, g_h = 0;
clock::Every g_adopt{0.25};

constexpr std::int64_t kRtfRgba16f = 6; // [SDK] ETextureRenderTargetFormat RTF_RGBA16f -> PF_FloatRGBA (linear)

bool ensure_component(UObject* pc) {
    if (g_comp.get()) return true;
    g_actor.reset();
    g_rt.reset();
    g_w = g_h = 0;
    UObject* world = ue::world_of(pc);
    UClass* actor_cls = ue::find_class(L"/Script/Engine.Actor");
    UClass* comp_cls = ue::find_class(L"/Script/UMG.WidgetComponent");
    if (!world || !actor_cls || !comp_cls) return false;

    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(actor_cls).xf({}).i(1 /*AlwaysSpawn*/).o(nullptr);
    if (!begin.go() || !begin.ro()) return false;
    UObject* actor = begin.ro();
    stage_register_owned(actor);
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(actor).xf({}).go();

    Call add(actor, L"AddComponentByClass");
    add.o(comp_cls).b(false).xf({}).b(true /*deferred*/);
    UObject* comp = add.go() ? add.ro() : nullptr;
    if (!comp) {
        Call(actor, L"K2_DestroyActor").go();
        return false;
    }
    ue::set_num(comp, L"Space", 0.0);         // EWidgetSpace::World: draws into RenderTarget
    ue::set_flag(comp, L"TickWhenOffscreen", true);
    ue::set_flag(comp, L"bReceiveHardwareInput", false);
    if (auto* ds = ue::field<std::int32_t>(comp, L"DrawSize")) ds[0] = ds[1] = 0; // no draw (and no engine RT) until ours exists
    Call(actor, L"FinishAddComponent").o(comp).b(false).xf({}).go();
    Call(comp, L"SetTickableWhenPaused").b(true).go(); // EL HUD fades keep running while EL pauses
    Call(comp, L"SetCollisionEnabled").i(0).go();
    Call(actor, L"SetActorEnableCollision").b(false).go();
    Call(actor, L"SetActorHiddenInGame").b(true).go(); // no scene proxy; the component stays IsVisible()
    g_actor.set(actor);
    g_comp.set(comp);
    log::info(L"HUD widget component spawned");
    return true;
}

// EL's UI is ScaleToFit on a 1920x1080 design and the virtual window is painted at DrawScale 1, so the
// component is sized in design units: the same layout EL's viewport shows at this aspect.
bool ensure_rt(UObject* pc, UObject* comp) {
    unsigned w = 0, h = 0;
    if (!frame_dims(w, h)) return false;
    const double ui = std::min(w / 1920.0, h / 1080.0);
    const auto dw = static_cast<std::int64_t>(std::lround(w / ui)), dh = static_cast<std::int64_t>(std::lround(h / ui));
    if (g_rt.get() && dw == g_w && dh == g_h) return true;
    if (!capture_rt_ready()) return false; // one CreateTexture2D grab armed at a time (descs may coincide)
    UObject* world = ue::world_of(pc);
    UObject* krl = ue::find(L"/Script/Engine.Default__KismetRenderingLibrary");
    if (!world || !krl) return false;
    hud_expect_rt(static_cast<unsigned>(dw), static_cast<unsigned>(dh));
    Call crt(krl, L"CreateRenderTarget2D");
    crt.o(world).i(dw).i(dh).i(kRtfRgba16f).c({0.0f, 0.0f, 0.0f, 0.0f}).b(false);
    UObject* rt = crt.go() ? crt.ro() : nullptr;
    if (!rt) return false;
    if (UObject** slot = ue::field<UObject*>(comp, L"RenderTarget")) *slot = rt;
    if (auto* ds = ue::field<std::int32_t>(comp, L"DrawSize")) {
        ds[0] = static_cast<std::int32_t>(dw);
        ds[1] = static_cast<std::int32_t>(dh);
    }
    ue::call(comp, L"RequestRedraw");
    g_rt.set(rt);
    g_w = dw;
    g_h = dh;
    log::info(L"HUD render target %lldx%lld (frame %ux%u)", dw, dh, w, h);
    return true;
}

// PC_Base::PlayerUI is EL's live HUD: InitializeUI creates + AddToViewport's a new one on game data load and on
// OnPawnRespawn (the old one is only RemoveFromParent'ed), so it is followed every tick (a field read)
void adopt(UObject* comp, UObject* ui, bool recheck) {
    if (ui != g_ui.get()) {
        Call(comp, L"SetWidget").o(ui).go();
        g_ui.set(ui);
        recheck = true;
        log::info(L"EL PlayerUI moved into the HUD channel");
    }
    if (!recheck) return;
    if (ue::call_bool(ui, L"IsInViewport")) Call(ui, L"RemoveFromParent").go(); // only after SetWidget
    // Out of the back buffer now: EL's own "display player UI" setting may show it (PlayerUIContainer)
    if (UObject* gs = ue::first(L"GameSettingsSubsystem")) {
        if (!ue::call_bool(gs, L"GetDisplayPlayerUI", true)) Call(gs, L"SetDisplayPlayerUI").b(true).go();
    }
}

} // namespace

void hud_tick(UObject* pc) {
    if (!pc || !ensure_component(pc)) return;
    UObject* comp = g_comp.get();
    UObject* ui = ue::obj(pc, L"PlayerUI");
    const bool rt = ensure_rt(pc, comp);
    if (rt && ui) adopt(comp, ui, g_adopt.due(clock::now_s()));
    hud_set_live(rt && ui && ui == g_ui.get());
}

void hud_reset() {
    g_ui.reset(); // re-adopt PC_Base::PlayerUI; the component and RT survive if the level does
    hud_set_live(false);
}

} // namespace lie::guest
