// Native input / camera hooks:
// - UZenithInputComponent::InputConsume: after EL consumed its own snapshot, overwrite InputSnapshot with the
//   Host's frame (only in Gameplay), so Lily is driven exactly like by a local pad.
// - AGameplayCamera::GameplayCameraUpdate: force bLag = false so Lily keeps a fixed screen point.
// - XInputGetState: while the Host is alive EL never reads the physical pad (the Host owns input).

#include "guest/guest.h"

#include "rt/hook.h"
#include "rt/input.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include "rt/clock.h"
#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>

namespace lie::guest {

namespace {

struct InputSnapshot { // [IDA] 40 bytes, verified against binary.json before arming
    float input_x, input_y;
    float right_x, right_y;
    float move_x, move_y, move_z;
    std::uint8_t direction, pad[3];
    std::uint32_t flags; // rising edges
    std::uint32_t raw;   // held
};
static_assert(sizeof(InputSnapshot) == 40);

int g_snapshot_offset = -1;
ue::Weak g_input_comp;
std::uint32_t g_seen_taps[16]{};
std::uint32_t g_prev_raw = 0;

std::mutex g_local_mutex;
struct Local {
    float x = 0.0f, y = 0.0f;
    std::uint32_t held = 0, taps = 0;
    int frames = 0;
} g_local;

std::uint8_t direction_of(float x, float y) {
    const bool r = x > 0.5f, l = x < -0.5f, u = y > 0.5f, d = y < -0.5f;
    if (u && r) return 5;
    if (u && l) return 6;
    if (d && r) return 7;
    if (d && l) return 8;
    if (u) return 1;
    if (d) return 2;
    if (l) return 3;
    return r ? 4 : 0;
}

using ConsumeFn = std::uintptr_t(__fastcall*)(UObject*, void*);
ConsumeFn g_consume = nullptr;

std::uintptr_t __fastcall on_consume(UObject* comp, void* param) {
    const std::uintptr_t res = g_consume(comp, param);
    if (!comp || g_snapshot_offset < 0 || g_input_comp.get() != comp || !RC::Unreal::IsInGameThreadRaw()) return res;

    Control* c = shm::control();
    const Ctx ctx = host_ctx();
    float x = 0.0f, y = 0.0f;
    std::uint32_t raw = 0, taps = 0;
    InputFrame in{};
    if (c && c->input.read(in)) {
        if (ctx == Ctx::Gameplay) {
            x = std::clamp(in.x, -1.0f, 1.0f);
            y = std::clamp(in.y, -1.0f, 1.0f);
            raw = in.raw & cmd::Mask;
            for (int k = 1; k < cmd::Count; ++k) {
                if (in.tap_seq[k] != g_seen_taps[k]) taps |= 1u << k;
            }
        }
        std::memcpy(g_seen_taps, in.tap_seq, sizeof(g_seen_taps)); // presses outside Gameplay are dropped, not replayed later
    }
    {
        std::scoped_lock lk(g_local_mutex);
        if (g_local.frames > 0) {
            if (std::abs(g_local.x) > 0.001f || std::abs(g_local.y) > 0.001f) x = g_local.x, y = g_local.y;
            raw |= g_local.held;
            taps |= g_local.taps;
            g_local.taps = 0;
            if (--g_local.frames == 0) g_local = {};
        }
    }
    const bool drive = ctx != Ctx::Inactive || raw || taps || std::abs(x) > 0.001f || std::abs(y) > 0.001f;
    if (!drive) {
        g_prev_raw = 0;
        return res;
    }
    InputSnapshot s{};
    s.input_x = s.right_x = s.move_y = x;
    s.input_y = s.right_y = s.move_z = y;
    s.direction = direction_of(x, y);
    s.raw = raw;
    s.flags = taps | (raw & ~g_prev_raw);
    g_prev_raw = raw;
    std::memcpy(reinterpret_cast<std::uint8_t*>(comp) + g_snapshot_offset, &s, sizeof(s));
    return res;
}

using CameraFn = std::uint64_t(__fastcall*)(UObject*, float, bool);
CameraFn g_camera = nullptr;
std::uint64_t __fastcall on_camera(UObject* cam, float dt, bool) { return g_camera(cam, dt, false); }

using XInputFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputFn g_xinput = nullptr;
// While the Host is alive the physical pad belongs to it. In Lily's menus it hands the pad back as is (EL's pad UI
// layout already equals EM's), so EL shows pad prompts and navigates natively; elsewhere EL sees an idle pad.
DWORD WINAPI on_xinput(DWORD index, XINPUT_STATE* st) {
    if (host_alive() && st) {
        static DWORD packet = 0;
        static PadState last{};
        std::memset(st, 0, sizeof(*st));
        Control* c = shm::control();
        InputFrame in{};
        if (index == 0 && c && host_ctx() == Ctx::LilyMenu && c->input.read(in) && in.pad.connected) {
            const PadState& p = in.pad;
            if (std::memcmp(&p, &last, sizeof(p)) != 0) last = p, ++packet;
            st->Gamepad = {p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry};
        }
        st->dwPacketNumber = packet;
        return index == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    }
    return g_xinput(index, st);
}

bool offset_matches(UObject* comp, const wchar_t* prop_name, std::optional<std::int64_t> expected) {
    ue::FProperty* p = ue::prop(comp->GetClassPrivate(), prop_name);
    return p && expected && p->GetOffset_Internal() == *expected;
}

} // namespace

bool arm_input(const binary::Manifest& m, UObject* pawn) {
    UObject* comp = ue::obj(pawn, L"ZenithInputComponent");
    if (!comp) return false;
    if (!offset_matches(comp, L"PawnOwner", m.offset("ZenithInputComponent.PawnOwner")) ||
        !offset_matches(comp, L"NextInputSnapshot", m.offset("ZenithInputComponent.NextInputSnapshot")) ||
        !offset_matches(comp, L"InputSnapshot", m.offset("ZenithInputComponent.InputSnapshot")) ||
        m.offset("InputSnapshot.size").value_or(0) != static_cast<std::int64_t>(sizeof(InputSnapshot))) {
        log::error(L"UZenithInputComponent layout differs from binary.json: input injection disabled (fail-closed)");
        return false;
    }
    g_snapshot_offset = static_cast<int>(*m.offset("ZenithInputComponent.InputSnapshot"));
    const bool ok = hook::attach(m, "InputConsume", reinterpret_cast<void*>(&on_consume), reinterpret_cast<void**>(&g_consume)) &&
                    hook::attach(m, "GameplayCameraUpdate", reinterpret_cast<void*>(&on_camera), reinterpret_cast<void**>(&g_camera));
    g_input_comp.set(comp);
    return ok;
}

void arm_xinput() {
    if (hook::attached("XInputGetState")) return;
    for (const wchar_t* dll : {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
        HMODULE mod = GetModuleHandleW(dll);
        void* fn = mod ? reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState")) : nullptr;
        if (!fn) continue;
        if (hook::attach_at("XInputGetState", fn, reinterpret_cast<void*>(&on_xinput), reinterpret_cast<void**>(&g_xinput))) {
            input::set_xinput(reinterpret_cast<input::XInputGetStateFn>(g_xinput));
        }
        return;
    }
}

void local_input(float x, float y, std::uint32_t held, std::uint32_t taps, int frames) {
    std::scoped_lock lk(g_local_mutex);
    g_local.x = std::clamp(x, -1.0f, 1.0f);
    g_local.y = std::clamp(y, -1.0f, 1.0f);
    g_local.held = held & cmd::Mask;
    g_local.taps |= taps & cmd::Mask;
    g_local.frames = std::max(frames, 1);
}

} // namespace lie::guest
