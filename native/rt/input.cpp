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
    std::uint32_t menu = 0;
    int menu_frames = 0;
} g_script;

struct Layout {
    std::mutex m;
    Bindings b{}; // until the Guest has published EL's bindings, Lily only moves with the pad
    UiKeys u = em_default_ui_keys();
} g_layout;

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
    p.buttons = st.Gamepad.wButtons;
    p.lt = st.Gamepad.bLeftTrigger;
    p.rt = st.Gamepad.bRightTrigger;
    p.lx = st.Gamepad.sThumbLX;
    p.ly = st.Gamepad.sThumbLY;
    p.rx = st.Gamepad.sThumbRX;
    p.ry = st.Gamepad.sThumbRY;
    return p;
}

Devices read_devices() {
    Devices d;
    for (int k = 0x01; k < 0xFF; ++k) { // incl. mouse buttons: EM may bind actions to them
        if (k == 0x03) continue;        // VK_CANCEL
        if (GetAsyncKeyState(k) & 0x8000) d.keys.set(k);
    }
    d.pad = read_pad();
    return d;
}

void set_layout(const Bindings& b, const UiKeys& u) {
    std::scoped_lock lk(g_layout.m);
    g_layout.b = b;
    g_layout.u = u;
}
Bindings bindings() {
    std::scoped_lock lk(g_layout.m);
    return g_layout.b;
}
UiKeys ui_keys() {
    std::scoped_lock lk(g_layout.m);
    return g_layout.u;
}

void script(float x, float y, std::uint32_t held, std::uint32_t taps, int frames) {
    std::scoped_lock lk(g_script.m);
    g_script.x = std::clamp(x, -1.0f, 1.0f);
    g_script.y = std::clamp(y, -1.0f, 1.0f);
    g_script.held = held & cmd::Mask;
    g_script.taps |= taps & cmd::Mask;
    g_script.frames = std::max(frames, 1);
}

void script_menu(std::uint32_t held, int frames) {
    std::scoped_lock lk(g_script.m);
    g_script.menu = held & ((1u << menu::Count) - 1);
    g_script.menu_frames = std::max(frames, 1);
}

void FrameBuilder::build(InputFrame& out, const Devices* d) {
    Actions a;
    std::uint32_t taps = 0;
    if (d) a = lily_actions(*d, bindings());
    {
        std::scoped_lock lk(g_script.m);
        if (g_script.frames > 0) {
            if (std::abs(g_script.x) > 0.001f || std::abs(g_script.y) > 0.001f) a.x = g_script.x, a.y = g_script.y;
            a.raw |= g_script.held;
            taps |= g_script.taps;
            g_script.taps = 0;
            if (--g_script.frames == 0) g_script.held = 0, g_script.x = g_script.y = 0.0f;
        }
    }
    a.raw &= cmd::Mask;
    const std::uint32_t rising = (a.raw & ~prev_raw_) | taps;
    prev_raw_ = a.raw;
    out.x = std::clamp(a.x, -1.0f, 1.0f);
    out.y = std::clamp(a.y, -1.0f, 1.0f);
    out.raw = a.raw;
    for (int c = 1; c < cmd::Count; ++c) {
        if (rising & bit(c)) ++out.tap_seq[c];
    }
}

std::uint32_t build_menu(const Devices* d) {
    std::uint32_t m = d ? menu_bits(d->keys, ui_keys()) : 0u;
    std::scoped_lock lk(g_script.m);
    if (g_script.menu_frames > 0) {
        m |= g_script.menu;
        if (--g_script.menu_frames == 0) g_script.menu = 0;
    }
    return m;
}

} // namespace lie::input
