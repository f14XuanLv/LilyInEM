#pragma once

// LiliesGuest internal interface. EL is a headless character engine: it owns Lily's body, skills,
// HP and settlement of hits on her; the EM world only reaches it as mirrored geometry, proxies and events.
// Everything runs on the game thread except g_frame (render thread Present hook).

#include "core/binary_manifest.h"
#include "core/protocol.h"
#include "rt/ue.h"

#include <string>

namespace lie::guest {

using ue::UClass;
using ue::UObject;

// ---- g_input.cpp: InputConsume / camera / XInput detours ----
bool arm_input(const binary::Manifest& m, UObject* pawn); // layout-checked, fail-closed
void arm_xinput();
void local_input(float x, float y, std::uint32_t held, std::uint32_t taps, int frames);

// ---- g_frame.cpp: Present export (render thread) ----
void arm_present();
void stage_frame(float foot_u, float foot_v, float view_w_uu, float view_h_uu); // game thread -> next Present
void stage_menu(FrameKind kind, const float split[4], float menu_alpha);        // how the next Presents are composited
bool frame_dims(unsigned& w, unsigned& h);   // exported frame size (0 until the first Present)
void capture_expect_rt(unsigned w, unsigned h); // arm the CreateTexture2D grab for the capture RT
bool capture_rt_ready();
void* window();
void shutdown_frame();

// ---- g_capture.cpp: SceneCapture2D -> shared texture with real coverage alpha (game thread) ----
// The capture renders exactly what the black-studio frame shows (hidden actors render in neither),
// giving the Host premult RGB + inverse-opacity alpha instead of a luminance key.
void capture_tick(UObject* pc); // spawn/sync/drive the capture; safe to call every tick
void capture_reset();

// ---- g_stage.cpp: black studio + headless EL services ----
void stage_isolate_all(UObject* pc, UObject* lily);
void stage_on_begin_play(UObject* actor);
void stage_headless(UObject* pc, UObject* lily);
void stage_window_tick(double now);
void stage_reset();
bool stage_owns(const UObject* actor); // our boxes / proxies are never isolated
void stage_register_owned(UObject* actor);

// ---- g_terrain.cpp: EM terrain slice -> Static box pool ----
struct TerrainStatus {
    std::uint32_t applied_revision = 0;
    std::uint32_t boxes = 0;
};
void terrain_tick(UObject* world, const FrameMap& map, double plane_x, bool force);
TerrainStatus terrain_status();
void terrain_reset();

// ---- g_combat.cpp: proxies, hit capture, hurt replay ----
void combat_arm(const binary::Manifest& m);
void combat_tick(UObject* world, UObject* pc, UObject* lily, const FrameMap& map, double plane_x);
void combat_on_hurt(UObject* lily, const HurtPayload& hurt);
void combat_reset();

// ---- g_body.cpp: main tick ----
void tick();
bool on_process_event_pre(UObject* ctx, ue::UFunction* fn);
void on_process_event_post(UObject* ctx, ue::UFunction* fn);
std::string status();
void debug(const std::wstring& cmd);

// shared helpers
Ctx host_ctx();
bool host_alive();
GuestCounters& counters();
void publish_counters();
UObject* lily();

} // namespace lie::guest
