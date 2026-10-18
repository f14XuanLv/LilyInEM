#pragma once

// Engine-independent input routing tables: one physical layout (EM's own) mapped per context.
// Matrix and rationale: docs/INPUT_MATRIX.md. Unit-tested offline (tests/test_core.cpp).
//   Lily actions  = EM IMC_Default / IMC_Interact slot for slot (Lilac's Attack_A..D -> main / sub1 / sub2 / set switch)
//   Lily menus    = EM UIInputSettingsData (Confirm / Cancel / Context / Next / Previous / navigation)

#include "core/protocol.h"

#include <cstdint>

namespace lie::input {

using lie::Input;

namespace vk { // Win32 virtual-key codes (no <Windows.h> in core)
inline constexpr int Tab = 0x09, Return = 0x0D, Escape = 0x1B, Space = 0x20;
inline constexpr int Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
inline constexpr int LShift = 0xA0;
} // namespace vk

namespace xb { // XINPUT_GAMEPAD_* / thresholds
inline constexpr std::uint16_t DUp = 0x0001, DDown = 0x0002, DLeft = 0x0004, DRight = 0x0008;
inline constexpr std::uint16_t Start = 0x0010, Back = 0x0020, LThumb = 0x0040, RThumb = 0x0080;
inline constexpr std::uint16_t LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
inline constexpr std::uint8_t TriggerThreshold = 30;
inline constexpr int LeftDeadZone = 7849, RightDeadZone = 8689;
} // namespace xb

struct Pad {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t lt = 0, rt = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Keys { // virtual-key bitset
    std::uint64_t bits[4]{};
    bool down(int k) const { return k > 0 && k < 256 && ((bits[k >> 6] >> (k & 63)) & 1u) != 0; }
    void set(int k) {
        if (k > 0 && k < 256) bits[k >> 6] |= 1ull << (k & 63);
    }
    bool any() const { return (bits[0] | bits[1] | bits[2] | bits[3]) != 0; }
};

struct Devices {
    Keys keys;
    Pad pad;
};

inline bool trigger_down(std::uint8_t v) { return v > xb::TriggerThreshold; }
inline bool stick_moved(std::int16_t x, std::int16_t y, int dead) {
    return static_cast<long long>(x) * x + static_cast<long long>(y) * y > static_cast<long long>(dead) * dead;
}

// Inputs held when a context switch happens belong to the context that is ending: armed at the switch, they read
// as released for the new owner until physically let go (a menu-closing Esc / B / A must not also pause EM,
// cancel Lily's next menu or make her jump).
class Latch {
public:
    void arm(const Devices& d) {
        keys_ = d.keys;
        buttons_ = d.pad.buttons;
        lt_ = trigger_down(d.pad.lt);
        rt_ = trigger_down(d.pad.rt);
        lstick_ = stick_moved(d.pad.lx, d.pad.ly, xb::LeftDeadZone);
        rstick_ = stick_moved(d.pad.rx, d.pad.ry, xb::RightDeadZone);
    }
    void apply(Devices& d) {
        apply_keys(d.keys);
        apply_pad(d.pad);
    }
    void apply_keys(Keys& k) {
        for (int i = 0; i < 4; ++i) {
            keys_.bits[i] &= k.bits[i];
            k.bits[i] &= ~keys_.bits[i];
        }
    }
    void apply_pad(Pad& p) {
        buttons_ &= p.buttons;
        p.buttons = static_cast<std::uint16_t>(p.buttons & ~buttons_);
        lt_ = lt_ && trigger_down(p.lt);
        rt_ = rt_ && trigger_down(p.rt);
        lstick_ = lstick_ && stick_moved(p.lx, p.ly, xb::LeftDeadZone);
        rstick_ = rstick_ && stick_moved(p.rx, p.ry, xb::RightDeadZone);
        if (lt_) p.lt = 0;
        if (rt_) p.rt = 0;
        if (lstick_) p.lx = p.ly = 0;
        if (rstick_) p.rx = p.ry = 0;
    }
    bool blocks(int k) const { return keys_.down(k); }
    bool any() const { return keys_.any() || buttons_ || lt_ || rt_ || lstick_ || rstick_; }

private:
    Keys keys_;
    std::uint16_t buttons_ = 0;
    bool lt_ = false, rt_ = false, lstick_ = false, rstick_ = false;
};

// ---- physical inputs named by UE FKey names (what EM's / EL's settings store); lie::Input in protocol.h ----

inline bool down(const Devices& d, const Input& in) {
    if (in.vk) return d.keys.down(in.vk);
    if (!d.pad.connected) return false;
    if (in.button) return (d.pad.buttons & in.button) != 0;
    if (in.trigger == 1) return trigger_down(d.pad.lt);
    if (in.trigger == 2) return trigger_down(d.pad.rt);
    return false;
}

struct KeyName {
    const char* name;
    Input in;
};
inline constexpr KeyName kNamedKeys[] = {
    {"SpaceBar", {vk::Space}}, {"Enter", {vk::Return}}, {"Escape", {vk::Escape}}, {"Tab", {vk::Tab}}, {"BackSpace", {0x08}},
    {"LeftShift", {vk::LShift}}, {"RightShift", {0xA1}}, {"LeftControl", {0xA2}}, {"RightControl", {0xA3}}, {"LeftAlt", {0xA4}},
    {"RightAlt", {0xA5}}, {"CapsLock", {0x14}}, {"Up", {vk::Up}}, {"Down", {vk::Down}}, {"Left", {vk::Left}}, {"Right", {vk::Right}},
    {"PageUp", {0x21}}, {"PageDown", {0x22}}, {"End", {0x23}}, {"Home", {0x24}}, {"Insert", {0x2D}}, {"Delete", {0x2E}},
    {"Zero", {'0'}}, {"One", {'1'}}, {"Two", {'2'}}, {"Three", {'3'}}, {"Four", {'4'}}, {"Five", {'5'}}, {"Six", {'6'}},
    {"Seven", {'7'}}, {"Eight", {'8'}}, {"Nine", {'9'}}, {"NumPadZero", {0x60}}, {"NumPadOne", {0x61}}, {"NumPadTwo", {0x62}},
    {"NumPadThree", {0x63}}, {"NumPadFour", {0x64}}, {"NumPadFive", {0x65}}, {"NumPadSix", {0x66}}, {"NumPadSeven", {0x67}},
    {"NumPadEight", {0x68}}, {"NumPadNine", {0x69}}, {"Semicolon", {0xBA}}, {"Equals", {0xBB}}, {"Comma", {0xBC}},
    {"Hyphen", {0xBD}}, {"Period", {0xBE}}, {"Slash", {0xBF}}, {"Tilde", {0xC0}}, {"LeftBracket", {0xDB}},
    {"Backslash", {0xDC}}, {"RightBracket", {0xDD}}, {"Apostrophe", {0xDE}},
    {"LeftMouseButton", {0x01}}, {"RightMouseButton", {0x02}}, {"MiddleMouseButton", {0x04}}, {"ThumbMouseButton", {0x05}},
    {"ThumbMouseButton2", {0x06}},
    {"Gamepad_FaceButton_Bottom", {0, xb::A}}, {"Gamepad_FaceButton_Right", {0, xb::B}}, {"Gamepad_FaceButton_Left", {0, xb::X}},
    {"Gamepad_FaceButton_Top", {0, xb::Y}}, {"Gamepad_LeftShoulder", {0, xb::LB}}, {"Gamepad_RightShoulder", {0, xb::RB}},
    {"Gamepad_LeftThumbstick", {0, xb::LThumb}}, {"Gamepad_RightThumbstick", {0, xb::RThumb}},
    {"Gamepad_Special_Left", {0, xb::Back}}, {"Gamepad_Special_Right", {0, xb::Start}}, {"Gamepad_DPad_Up", {0, xb::DUp}},
    {"Gamepad_DPad_Down", {0, xb::DDown}}, {"Gamepad_DPad_Left", {0, xb::DLeft}}, {"Gamepad_DPad_Right", {0, xb::DRight}},
    {"Gamepad_LeftTrigger", {0, 0, 1}}, {"Gamepad_RightTrigger", {0, 0, 2}},
};

// Every name input_from_name understands (EM's UI keys can only be probed key by key)
template <class F>
void for_each_key_name(F&& f) {
    char buf[4] = {};
    for (char c = 'A'; c <= 'Z'; ++c) buf[0] = c, buf[1] = 0, f(static_cast<const char*>(buf));
    for (int i = 1; i <= 12; ++i) {
        buf[0] = 'F', buf[1] = static_cast<char>(i < 10 ? '0' + i : '1'), buf[2] = static_cast<char>(i < 10 ? 0 : '0' + i - 10);
        f(static_cast<const char*>(buf));
    }
    for (const KeyName& k : kNamedKeys) f(k.name);
}

inline Input input_from_name(const char* n) {
    if (!n || !n[0]) return {};
    if (n[1] == 0 && n[0] >= 'A' && n[0] <= 'Z') return {n[0]};
    if (n[0] == 'F' && n[1] >= '1' && n[1] <= '9') {
        const int i = n[2] == 0 ? n[1] - '0' : (n[1] == '1' && n[2] >= '0' && n[2] <= '2' && n[3] == 0 ? 10 + n[2] - '0' : 0);
        if (i >= 1 && i <= 12) return {0x6F + i};
    }
    for (const KeyName& k : kNamedKeys) {
        const char* a = k.name;
        const char* b = n;
        while (*a && *a == *b) ++a, ++b;
        if (*a == 0 && *b == 0) return k.in;
    }
    return {};
}

// ---- Lily in the world ----
// Lily's abilities are EL's, not Lilac's (no set switch on Lilac, no chorded hook on Lily, Lilac's four attacks
// are not Lily's main / sub / sub / switch), so Lily's keys are the player's EL bindings (EL's key binding pages).
// EM keeps its world services: interaction (EM's prompt shows EM's key) and the system keys it still owns in
// Lily mode. A Lily binding on one of those keys is dropped (EM wins) and reported.

// [SDK] Zenith ECommandInputTypes / EL action names (DefaultInput.ini, InputSettingsSubsystem) per Lily command.
// Interact is EM's; EL's MainMenu / Pause / Minimap / CameraReset are EL world business and never read.
inline constexpr const char* kElAction[cmd::Count] = {nullptr, "Jump", "Dodge", "Attack", "Summon_1", "Summon_2",
                                                      "SummonSetSwitch", "Hook", "Heal", nullptr};
// EL movement axes (+1 / -1 scale): up, down, left, right
inline constexpr struct {
    const char* axis;
    float scale;
} kElMoveAxis[4] = {{"Vertical", 1.0f}, {"Vertical", -1.0f}, {"Horizontal", -1.0f}, {"Horizontal", 1.0f}};

// EM mappings (InputSettingsSubsystem::GetMappedKey) that stay EM's in Lily mode
inline constexpr const char* kEmInteract[2] = {"Keyboard_Interact", "Gamepad_Interact"};
inline constexpr const char* kEmReserved[] = {
    "Keyboard_Pause", "Gamepad_Pause", "Keyboard_Menu", "Gamepad_Menu", "Keyboard_Minimap", "Gamepad_Minimap",
    "Keyboard_CameraOffset_Up", "Keyboard_CameraOffset_Down", "Keyboard_CameraOffset_Left", "Keyboard_CameraOffset_Right",
    "Keyboard_Interact", "Gamepad_Interact",
};
inline constexpr int kEmReservedCount = static_cast<int>(sizeof(kEmReserved) / sizeof(kEmReserved[0]));

struct EmWorldKeys {
    Input interact_kb{}, interact_pad{};
    Input reserved[kEmReservedCount]{};
    bool operator==(const EmWorldKeys&) const = default;
};

// EM's defaults (IMC_Default / IMC_Interact [CUE4P]) until EM's live settings have been read
inline EmWorldKeys em_default_world_keys() {
    EmWorldKeys e;
    e.interact_kb = {'E'}, e.interact_pad = {0, 0, 1};
    const Input r[kEmReservedCount] = {{vk::Escape}, {0, xb::Start}, {vk::Tab}, {0, xb::Back}, {'R'}, {0, xb::RThumb},
                                       {vk::Up}, {vk::Down}, {vk::Left}, {vk::Right}, {'E'}, {0, 0, 1}};
    for (int i = 0; i < kEmReservedCount; ++i) e.reserved[i] = r[i];
    return e;
}

struct Bindings {
    Input kb[cmd::Count][2]{}, pad[cmd::Count][2]{};
    Input kb_move[4][2]{}; // up, down, left, right (the pad moves with the left stick and the d-pad)
    bool operator==(const Bindings&) const = default;
};

struct Conflict {
    int cmd = 0;      // cmd:: (0 = movement)
    int move = -1;    // movement direction when cmd == 0 (up, down, left, right)
    int reserved = 0; // index into kEmReserved
    Input in{};
};

// Conflict bit per EL key binding row (ConflictRows): bits 1..9 = cmd::, bits 16..19 = movement up / down / left / right
inline std::uint32_t conflict_bit(const Conflict& c) { return c.cmd ? (1u << c.cmd) : (1u << (16 + c.move)); }

// EL's bindings + EM's interaction, minus everything EM keeps. Returns the number of conflicts written to `out`.
inline int compose_bindings(const ElBindings& el, const EmWorldKeys& em, Bindings& b, Conflict* out = nullptr, int cap = 0) {
    b = {};
    int n = 0;
    auto keep = [&](const Input& in, int c, int m = -1) {
        if (!in.valid()) return false;
        for (int r = 0; r < kEmReservedCount; ++r) {
            if (!(em.reserved[r] == in)) continue;
            if (out && n < cap) out[n] = {c, m, r, in};
            ++n;
            return false;
        }
        return true;
    };
    for (int c = 1; c < cmd::Count; ++c) {
        if (c == cmd::Interact) continue;
        for (int s = 0; s < 2; ++s) {
            if (keep(el.kb[c][s], c)) b.kb[c][s] = el.kb[c][s];
            if (keep(el.pad[c][s], c)) b.pad[c][s] = el.pad[c][s];
        }
    }
    for (int m = 0; m < 4; ++m) {
        for (int s = 0; s < 2; ++s) {
            if (keep(el.kb_move[m][s], 0, m)) b.kb_move[m][s] = el.kb_move[m][s];
        }
    }
    b.kb[cmd::Interact][0] = em.interact_kb;
    b.pad[cmd::Interact][0] = em.interact_pad;
    return n;
}

struct Actions {
    float x = 0.0f, y = 0.0f;
    std::uint32_t raw = 0; // cmd:: bits
};

inline Actions lily_actions(const Devices& d, const Bindings& b) {
    Actions a;
    const Pad& p = d.pad;
    if (p.connected) {
        if (stick_moved(p.lx, p.ly, xb::LeftDeadZone)) a.x = p.lx / 32767.0f, a.y = p.ly / 32767.0f;
        const int dx = ((p.buttons & xb::DRight) ? 1 : 0) - ((p.buttons & xb::DLeft) ? 1 : 0);
        const int dy = ((p.buttons & xb::DUp) ? 1 : 0) - ((p.buttons & xb::DDown) ? 1 : 0);
        if (dx || dy) a.x = static_cast<float>(dx), a.y = static_cast<float>(dy);
    }
    auto any = [&](const Input (&in)[2]) { return down(d, in[0]) || down(d, in[1]); };
    const int kx = (any(b.kb_move[3]) ? 1 : 0) - (any(b.kb_move[2]) ? 1 : 0);
    const int ky = (any(b.kb_move[0]) ? 1 : 0) - (any(b.kb_move[1]) ? 1 : 0);
    if (kx || ky) a.x = static_cast<float>(kx), a.y = static_cast<float>(ky);
    for (int c = 1; c < cmd::Count; ++c) {
        if (any(b.kb[c]) || any(b.pad[c])) a.raw |= 1u << c;
    }
    return a;
}

// ---- Lily's menus: EM's UI keys ----

// [SDK] Zion EUIInputType per menu:: intent (EM UMGBPFLibrary::IsKeyUIInputType)
inline constexpr std::uint8_t kEmUiType[menu::Count] = {9, 10, 11, 12, 1, 2, 3, 5, 6};
// [SDK] Zenith EUIInputTypes per menu:: intent (EL UMGSubsystem::OverrideUIInput); 0 = left to EL (navigation)
inline constexpr std::uint8_t kElUiType[menu::Count] = {0, 0, 0, 0, 1, 2, 3, 8, 9};
// EL's own navigation keys, pressed for the four navigation intents
inline constexpr int kElNavVk[4] = {vk::Up, vk::Down, vk::Left, vk::Right};
// Keys the Guest presses until the Host has read EM's UI layout (= EM's defaults)
inline constexpr int kElMenuVkDefault[menu::Count] = {vk::Up, vk::Down, vk::Left, vk::Right, vk::Return, vk::Escape, vk::Tab, 'E', 'Q'};
inline constexpr const char* kElMenuKbDefault[menu::Count] = {"", "", "", "", "Enter", "Escape", "Tab", "E", "Q"};

struct UiKeys {
    Input kb[menu::Count][2]{};
    Input pad[menu::Count]{};
    bool operator==(const UiKeys&) const = default;
};

// EM's defaults, probed live with IsKeyUIInputType (2026-10-19)
inline UiKeys em_default_ui_keys() {
    UiKeys u;
    auto set = [&](int m, Input k1, Input k2, Input p) { u.kb[m][0] = k1, u.kb[m][1] = k2, u.pad[m] = p; };
    set(menu::Up, {'W'}, {vk::Up}, {0, xb::DUp});
    set(menu::Down, {'S'}, {vk::Down}, {0, xb::DDown});
    set(menu::Left, {'A'}, {vk::Left}, {0, xb::DLeft});
    set(menu::Right, {'D'}, {vk::Right}, {0, xb::DRight});
    set(menu::Confirm, {vk::Return}, {}, {0, xb::A});
    set(menu::Cancel, {vk::Escape}, {}, {0, xb::B});
    set(menu::Context, {vk::Tab}, {}, {0, xb::Y});
    set(menu::Next, {'E'}, {}, {0, xb::RB});
    set(menu::Prev, {'Q'}, {}, {0, xb::LB});
    return u;
}

// Keyboard -> menu intents. The pad is handed to EL as is (menu_pad) after EL's pad UI bindings were set to EM's.
inline std::uint32_t menu_bits(const Keys& k, const UiKeys& u) {
    Devices d;
    d.keys = k;
    std::uint32_t m = 0;
    for (int i = 0; i < menu::Count; ++i) {
        if ((u.kb[i][0].vk && down(d, u.kb[i][0])) || (u.kb[i][1].vk && down(d, u.kb[i][1]))) m |= 1u << i;
    }
    return m;
}

// The pad for EL's own pad UI, minus Start / Back (EM's pause / game menu; EL's would open its own world menus)
inline PadState menu_pad(const Pad& p) {
    PadState s;
    if (!p.connected) return s;
    s.connected = 1;
    s.buttons = static_cast<std::uint16_t>(p.buttons & ~(xb::Start | xb::Back));
    s.lt = p.lt, s.rt = p.rt;
    s.lx = p.lx, s.ly = p.ly, s.rx = p.rx, s.ry = p.ry;
    return s;
}

} // namespace lie::input
