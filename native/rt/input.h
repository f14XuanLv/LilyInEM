#pragma once

// Host-side device reading + scripted override on top of the routing tables in core/input_map.h
// (one physical layout, EM's own, mapped per context: docs/INPUT_MATRIX.md).

#include "core/input_map.h"
#include "core/protocol.h"

#include <cstdint>

namespace lie::input {

using XInputGetStateFn = unsigned long(__stdcall*)(unsigned long, void*);

// The mod's private XInputGetState (the trampoline once the game's export is detoured)
void set_xinput(XInputGetStateFn fn);
XInputGetStateFn xinput();

Pad read_pad();
Devices read_devices(); // keyboard (GetAsyncKeyState) + pad 0; the caller decides focus / routing

// EM's live layout (read on the game thread from EM's settings, used on the render thread); EM defaults until then
void set_layout(const Bindings& b, const UiKeys& u);
Bindings bindings();
UiKeys ui_keys();

// Scripted input merged over physical input for `frames` builds (AI pilot / tests)
void script(float x, float y, std::uint32_t held, std::uint32_t taps, int frames);
void script_menu(std::uint32_t held, int frames);

// Lily's action frame (Ctx::Gameplay); `d` is null when the frame is not routed to Lily.
// Rising edges increment tap_seq so the Guest never misses a press between two of its frames.
class FrameBuilder {
public:
    void build(InputFrame& out, const Devices* d);

private:
    std::uint32_t prev_raw_ = 0;
};

// Menu intents for Lily's menus (Ctx::LilyMenu): keyboard via menu_bits + scripted intents
std::uint32_t build_menu(const Devices* d);

} // namespace lie::input
