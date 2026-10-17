#pragma once

// Physical input -> Lily command frame (Host side), plus a scripted override for automation.
// Default layout follows EM's own IMC_Default so muscle memory carries over, and never touches EM's
// Pause (Esc/Start), Menu (Tab/Back), Minimap (R/RS) or CameraOffset (arrows / right stick) keys.

#include "core/protocol.h"

#include <cstdint>

namespace lie::input {

using XInputGetStateFn = unsigned long(__stdcall*)(unsigned long, void*);

// The mod's private XInputGetState (the trampoline once the game's export is detoured)
void set_xinput(XInputGetStateFn fn);
XInputGetStateFn xinput();

struct Pad {
    bool connected = false;
    float lx = 0.0f, ly = 0.0f;
    std::uint16_t buttons = 0;
    std::uint8_t lt = 0, rt = 0;
};
Pad read_pad();

// Scripted input merged over physical input for `frames` builds (AI pilot / tests)
void script(float x, float y, std::uint32_t held, std::uint32_t taps, int frames);

// Builds the next frame: physical devices only when allowed (focused + routed to Lily).
// Rising edges increment tap_seq so the Guest never misses a press between two of its frames.
class FrameBuilder {
public:
    void build(InputFrame& out, bool devices);

private:
    std::uint32_t prev_raw_ = 0;
};

} // namespace lie::input
