#include "rt/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace lie::input {

namespace {

std::atomic<XInputGetStateFn> g_xinput{nullptr};

struct Scripted {
    std::mutex m;
    float x = 0.0f, y = 0.0f;
    std::uint32_t held = 0, taps = 0;
    int frames = 0;
} g_script;

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

XInputGetStateFn resolve() {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        HMODULE m = GetModuleHandleA(dll);
        if (!m) m = LoadLibraryA(dll);
        if (m) {
            if (auto* f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) return f;
        }
    }
    return nullptr;
}

constexpr std::uint32_t bit(int c) { return 1u << c; }

// [POLICY] stick dead zone matching XInput's recommended left-thumb dead zone (7849 / 32767)
constexpr float kDeadZone = 7849.0f / 32767.0f;

} // namespace

void set_xinput(XInputGetStateFn fn) { g_xinput.store(fn); }

XInputGetStateFn xinput() {
    XInputGetStateFn f = g_xinput.load();
    if (!f) {
        f = resolve();
        g_xinput.store(f);
    }
    return f;
}

Pad read_pad() {
    Pad p;
    XInputGetStateFn f = xinput();
    XINPUT_STATE st{};
    if (!f || f(0, &st) != ERROR_SUCCESS) return p;
    p.connected = true;
    p.lx = std::clamp(st.Gamepad.sThumbLX / 32767.0f, -1.0f, 1.0f);
    p.ly = std::clamp(st.Gamepad.sThumbLY / 32767.0f, -1.0f, 1.0f);
    p.buttons = st.Gamepad.wButtons;
    p.lt = st.Gamepad.bLeftTrigger;
    p.rt = st.Gamepad.bRightTrigger;
    return p;
}

void script(float x, float y, std::uint32_t held, std::uint32_t taps, int frames) {
    std::scoped_lock lk(g_script.m);
    g_script.x = std::clamp(x, -1.0f, 1.0f);
    g_script.y = std::clamp(y, -1.0f, 1.0f);
    g_script.held = held & cmd::Mask;
    g_script.taps |= taps & cmd::Mask;
    g_script.frames = std::max(frames, 1);
}

void FrameBuilder::build(InputFrame& out, bool devices) {
    float x = 0.0f, y = 0.0f;
    std::uint32_t raw = 0, taps = 0;
    if (devices) {
        const Pad pad = read_pad();
        if (pad.connected) {
            if (std::hypot(pad.lx, pad.ly) > kDeadZone) x = pad.lx, y = pad.ly;
            const int dx = ((pad.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) - ((pad.buttons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0);
            const int dy = ((pad.buttons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0) - ((pad.buttons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0);
            if (dx || dy) x = static_cast<float>(dx), y = static_cast<float>(dy);
            if (pad.buttons & XINPUT_GAMEPAD_A) raw |= bit(cmd::Jump);
            if (pad.buttons & XINPUT_GAMEPAD_X) raw |= bit(cmd::Attack);
            if (pad.buttons & XINPUT_GAMEPAD_Y) raw |= bit(cmd::Sub1);
            if (pad.buttons & XINPUT_GAMEPAD_B) raw |= bit(cmd::Sub2);
            if (pad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) raw |= bit(cmd::SummonSwitch);
            if (pad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) raw |= bit(cmd::Heal);
            if (pad.rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= bit(cmd::Dodge);
            if (pad.lt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= bit(cmd::Hook) | bit(cmd::Interact); // EM IA_Interact = LT
        }
        const int kx = (down('D') ? 1 : 0) - (down('A') ? 1 : 0);
        const int ky = (down('W') ? 1 : 0) - (down('S') ? 1 : 0);
        if (kx || ky) x = static_cast<float>(kx), y = static_cast<float>(ky);
        if (down(VK_SPACE) || down('Z')) raw |= bit(cmd::Jump);
        if (down(VK_LSHIFT) || down('C')) raw |= bit(cmd::Dodge);
        if (down('H') || down('X')) raw |= bit(cmd::Attack);
        if (down('J')) raw |= bit(cmd::Sub1);
        if (down('K')) raw |= bit(cmd::Sub2);
        if (down('L')) raw |= bit(cmd::SummonSwitch);
        if (down('Q')) raw |= bit(cmd::Heal);
        if (down('E')) raw |= bit(cmd::Hook) | bit(cmd::Interact); // EM IA_Interact = E
    }
    {
        std::scoped_lock lk(g_script.m);
        if (g_script.frames > 0) {
            if (std::abs(g_script.x) > 0.001f || std::abs(g_script.y) > 0.001f) x = g_script.x, y = g_script.y;
            raw |= g_script.held;
            taps |= g_script.taps;
            g_script.taps = 0;
            if (--g_script.frames == 0) g_script.held = 0, g_script.x = g_script.y = 0.0f;
        }
    }
    raw &= cmd::Mask;
    const std::uint32_t rising = (raw & ~prev_raw_) | taps;
    prev_raw_ = raw;
    out.x = x;
    out.y = y;
    out.raw = raw;
    for (int c = 1; c < cmd::Count; ++c) {
        if (rising & bit(c)) ++out.tap_seq[c];
    }
}

} // namespace lie::input
