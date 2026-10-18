#pragma once

// MagnoliaHost internal interface. EM owns the world (terrain, enemies, camera, menus, death/respawn);
// Lilac becomes a puppet placed by Lily's absolute pose, and EM-side settlement for hits on Lilac is
// intercepted and handed to EL. Game-thread modules: h_world (orchestration), h_puppet, h_terrain, h_combat.
// h_render runs on the render thread (Present hook).

#include "core/binary_manifest.h"
#include "core/protocol.h"
#include "rt/ue.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lie::host {

using ue::UClass;
using ue::UObject;

// ---- h_puppet.cpp ----
struct PuppetOut {
    bool placed = false;
    ue::V3 location{};
    double err = 0.0; // |placed - mapped pose| (oracle: must be 0)
};
void puppet_request_rebase(UObject* pawn, const char* why);
bool puppet_mapping_ready();             // Guest answered the current rebase
PuppetOut puppet_tick(UObject* pawn, bool drive);
void puppet_shift(UObject* pawn, double dy);   // move Lilac along Y without triggering a rebase
void puppet_reset();
std::uint32_t puppet_rebase_seq();

// ---- h_terrain.cpp ----
void terrain_tick(UObject* pc, UObject* pawn);
void terrain_reset();                    // new world / rebase: rescan everything nearest-first
bool terrain_ready();                    // the whole window has been scanned at least once
std::string terrain_status();

// ---- h_combat.cpp ----
void combat_arm(const binary::Manifest& m);
void combat_tick(UObject* pc, UObject* pawn, bool bridged);
void combat_on_begin_play(UObject* actor);
void combat_on_event(UObject* pc, UObject* pawn, const Event& e);
void combat_reset();
std::uint32_t entity_id_of(UObject* actor);

// ---- h_world.cpp ----
void tick_pre(float dt);
void on_begin_play(UObject* actor);
bool on_process_event_pre(UObject* ctx, ue::UFunction* fn, void* parms);
void on_process_event_post(UObject* ctx, ue::UFunction* fn, void* parms);
std::string status();
void debug(const std::wstring& cmd);
void scripted_input(float x, float y, std::uint32_t held, std::uint32_t taps, int frames);

// shared helpers
UObject* lilac();
bool bridged();
HostCounters& counters();
void send_to_guest(const Event& e);

} // namespace lie::host

namespace lie::render {

// Written by render-thread hotkeys / overlay, consumed by the game thread (which owns HostFlags and the rings)
struct Toggles {
    std::atomic<bool> bridge_on{true};
    std::atomic<bool> show_lilac{false};
    std::atomic<bool> debug_hud{false};
    std::atomic<float> quad_gain{1.0f};
    std::atomic<int> pause_requests{0}; // F5: EL's Esc menu
};
Toggles& toggles();
std::vector<EquipPayload> take_equip_commands();

void install();                       // DXGI Present/Resize + ID3D11Device::CreateTexture2D hooks
void shutdown();
void set_guest_res_scale(float s);    // EL frame = EM back buffer x this (default 5/3, LilyInEM.ini ResScale)
bool world_rt_captured();             // the native texture of our world-layer render target was seen
void set_world_layer(bool active);    // render thread writes the keyed Lily frame into the RT
bool overlay_open();                  // F6 loadout panel (Overlay context)
void toast(const char* utf8);

} // namespace lie::render
