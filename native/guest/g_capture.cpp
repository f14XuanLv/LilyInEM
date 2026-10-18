// SceneCapture alpha channel (game thread): one USceneCaptureComponent2D on a throwaway actor renders
// the same camera view into an RTF_RGBA16f target with CaptureSource = SCS_SceneColorHDR (premult RGB +
// inverse opacity in A -> coverage = 1 - a). g_frame's CreateTexture2D detour grabs the RT's native
// texture and exports it as FrameMeta::chars_handle; the Host composes bb.rgb / cov instead of keying.
// The visible set matches the back buffer exactly: the studio hides every EL actor that is not Lily,
// her spirits or their abilities, and hidden actors render in neither view (PRM_RenderScenePrimitives).

#include "guest/guest.h"

#include "rt/log.h"

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <cstdint>

namespace lie::guest {

namespace {

using ue::Call;

ue::Weak g_actor, g_comp, g_rt;
std::int64_t g_w = 0, g_h = 0;

// [SDK] ETextureRenderTargetFormat / ESceneCaptureSource / ESceneCapturePrimitiveRenderMode
constexpr std::int64_t kRtfRgba16f = 6;          // RTF_RGBA16f -> PF_FloatRGBA
constexpr std::int64_t kSceneColorHdr = 0;       // SCS_SceneColorHDR
constexpr std::int64_t kRenderScenePrimitives = 1;

struct Pov {
    ue::V3 loc, rot;
    double fov = 90.0, ortho = 0.0, proj = 0.0;
};

// APlayerCameraManager::CameraCachePrivate.POV (FMinimalViewInfo): the same view the back buffer uses.
bool read_pov(UObject* pc, Pov& out) {
    UObject* cm = ue::obj(pc, L"PlayerCameraManager");
    if (!cm) return false;
    ue::FProperty* ccp = ue::prop(cm->GetClassPrivate(), L"CameraCachePrivate");
    ue::FProperty* pov = ccp ? ue::member_of(ccp, L"POV") : nullptr;
    if (!pov) return false;
    auto* base = reinterpret_cast<std::uint8_t*>(cm);
    const std::size_t root = ccp->GetOffset_Internal() + pov->GetOffset_Internal();
    auto field = [&](const wchar_t* n, double def = 0.0) -> double {
        ue::FProperty* p = ue::member_of(pov, n);
        return p ? ue::read_num(p, base + root + p->GetOffset_Internal()) : def;
    };
    if (ue::FProperty* p = ue::member_of(pov, L"Location")) out.loc = ue::read_vec(base + root + p->GetOffset_Internal(), p->GetSize());
    if (ue::FProperty* p = ue::member_of(pov, L"Rotation")) out.rot = ue::read_vec(base + root + p->GetOffset_Internal(), p->GetSize());
    out.fov = field(L"FOV", out.fov);
    out.ortho = field(L"OrthoWidth");
    out.proj = field(L"ProjectionMode");
    return true;
}

bool ensure_capture(UObject* pc) {
    unsigned w = 0, h = 0;
    if (!frame_dims(w, h)) return false;
    if (g_comp.get() && g_rt.get() && static_cast<std::int64_t>(w) == g_w && static_cast<std::int64_t>(h) == g_h) return true;
    g_actor.reset();
    g_comp.reset();
    g_rt.reset();

    UObject* world = ue::world_of(pc);
    UClass* actor_cls = ue::find_class(L"/Script/Engine.Actor");
    UClass* comp_cls = ue::find_class(L"/Script/Engine.SceneCaptureComponent2D");
    UObject* krl = ue::find(L"/Script/Engine.Default__KismetRenderingLibrary");
    if (!world || !actor_cls || !comp_cls || !krl) return false;

    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(actor_cls).xf({}).i(1 /*AlwaysSpawn*/).o(nullptr);
    if (!begin.go() || !begin.ro()) return false;
    UObject* actor = begin.ro();
    stage_register_owned(actor); // before FinishSpawning: BeginPlay must not isolate it
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(actor).xf({}).go();

    Call add(actor, L"AddComponentByClass");
    add.o(comp_cls).b(false).xf({}).b(true /*deferred: props first, then FinishAddComponent*/);
    UObject* comp = add.go() ? add.ro() : nullptr;
    if (!comp) {
        Call(actor, L"K2_DestroyActor").go();
        return false;
    }
    ue::set_num(comp, L"CaptureSource", static_cast<double>(kSceneColorHdr));
    ue::set_num(comp, L"PrimitiveRenderMode", static_cast<double>(kRenderScenePrimitives));
    ue::set_flag(comp, L"bCaptureEveryFrame", false); // CaptureScene() is driven from the tick
    ue::set_flag(comp, L"bCaptureOnMovement", false);

    capture_expect_rt(w, h); // armed before the RT's internal CreateTexture2D lands
    Call crt(krl, L"CreateRenderTarget2D");
    crt.o(world).i(static_cast<std::int64_t>(w)).i(static_cast<std::int64_t>(h)).i(kRtfRgba16f).c({0.0f, 0.0f, 0.0f, 0.0f}).b(false);
    UObject* rt = crt.go() ? crt.ro() : nullptr;
    if (!rt) {
        Call(actor, L"K2_DestroyActor").go();
        return false;
    }
    if (UObject** slot = ue::field<UObject*>(comp, L"TextureTarget")) *slot = rt;
    Call(actor, L"FinishAddComponent").o(comp).b(false).xf({}).go();

    g_actor.set(actor);
    g_comp.set(comp);
    g_rt.set(rt);
    g_w = w;
    g_h = h;
    log::info(L"scene capture armed (%ux%u)", w, h);
    return true;
}

} // namespace

void capture_tick(UObject* pc) {
    if (!pc || !ensure_capture(pc)) return;
    UObject* comp = g_comp.get();
    Pov p{};
    if (!comp || !read_pov(pc, p)) return;
    Call(comp, L"K2_SetWorldLocationAndRotation").v(p.loc).r({p.rot.x, p.rot.y, p.rot.z}).b(false).skip().b(true).go();
    ue::set_num(comp, L"FOVAngle", p.fov);
    ue::set_num(comp, L"ProjectionType", p.proj);
    ue::set_num(comp, L"OrthoWidth", p.ortho);
    Call(comp, L"CaptureScene").go();
}

void capture_reset() {
    if (UObject* a = g_actor.get()) Call(a, L"K2_DestroyActor").go();
    g_actor.reset();
    g_comp.reset();
    g_rt.reset();
}

} // namespace lie::guest
