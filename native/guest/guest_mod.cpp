// LiliesGuest: UE4SS C++ mod for ENDER LILIES (headless character engine).
// Startup is fail-closed: unless data/binary.json matches the running EXE (module + SHA-256), no native
// detour is armed and the mod stays idle. Shipping logic is C++; the Lua API is for debugging / AI only.

#include "guest/guest.h"

#include "core/facts.h"
#include "rt/binary.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>

#include <optional>

using RC::LuaMadeSimple::Lua;

namespace lie::guest {
extern const binary::Manifest* g_manifest;
void shutdown_frame();
} // namespace lie::guest

namespace {

std::optional<lie::binary::Bound> g_bound;

template <class F>
void guarded(const wchar_t* where, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        static int reports = 0;
        if (reports++ < 20) lie::log::error(L"%s: %hs", where, e.what());
    }
}

int lua_status(const Lua& lua) {
    lua.set_string(lie::guest::status());
    return 1;
}

// LIE_GuestAct(x, y, held_bits, tap_bits, frames): local scripted input (bypasses the Host)
int lua_act(const Lua& lua) {
    const float x = lua.is_number() ? static_cast<float>(lua.get_number()) : 0.0f;
    const float y = lua.is_number() ? static_cast<float>(lua.get_number()) : 0.0f;
    const auto held = lua.is_integer() ? static_cast<std::uint32_t>(lua.get_integer()) : 0u;
    const auto taps = lua.is_integer() ? static_cast<std::uint32_t>(lua.get_integer()) : 0u;
    const int frames = lua.is_integer() ? static_cast<int>(lua.get_integer()) : 10;
    lie::guest::local_input(x, y, held, taps, frames);
    lua.set_bool(true);
    return 1;
}

// LIE_GuestDebug("unlock" | "restore" | "reanchor" | "isolate" | "menu" | "terrain")
int lua_debug(const Lua& lua) {
    if (lua.is_string()) {
        const std::string cmd(lua.get_string());
        lie::guest::debug(std::wstring(cmd.begin(), cmd.end()));
    }
    lua.set_bool(true);
    return 1;
}

} // namespace

class LiliesGuestMod final : public RC::CppUserModBase {
public:
    LiliesGuestMod() {
        ModName = STR("LiliesGuest");
        ModVersion = STR("0.3.0");
        ModDescription = STR("LilyInEM Guest: Ender Lilies as a headless character engine");
        ModAuthors = STR("LilyInEM");
        ModIntendedSDKVersion = STR("3.0");
        lie::log::tag() = L"LiliesGuest";
        lie::shm::open();
    }

    ~LiliesGuestMod() override {
        for (auto id : callbacks_) RC::Unreal::Hook::UnregisterCallback(id);
        lie::hook::detach_all();
        lie::guest::shutdown_frame();
        lie::shm::close();
    }

    void on_unreal_init() override {
        std::string why;
        g_bound = lie::binary::load_for_current_process(&why);
        if (!g_bound) {
            lie::log::error(L"binary check failed, mod idle: %hs", why.c_str());
            return;
        }
        lie::guest::g_manifest = &g_bound->manifest;
        lie::guest::arm_present();
        lie::guest::arm_xinput();
        lie::guest::combat_arm(g_bound->manifest);

        using namespace RC::Unreal;
        const Hook::FCallbackOptions opts{false, false, STR("LiliesGuest"), STR("LIE")};
        callbacks_.push_back(Hook::RegisterEngineTickPostCallback(
            [](auto&, UEngine*, float, bool) {
                lie::guest::arm_xinput(); // xinput DLL may load late
                guarded(L"tick", [] { lie::guest::tick(); });
            },
            opts));
        callbacks_.push_back(Hook::RegisterBeginPlayPostCallback(
            [](auto&, AActor* actor) { guarded(L"BeginPlay", [&] { lie::guest::stage_on_begin_play(actor); }); }, opts));
        callbacks_.push_back(Hook::RegisterProcessEventPreCallback(
            [](auto& data, UObject* ctx, UFunction* fn, void*) {
                guarded(L"ProcessEvent.pre", [&] {
                    if (lie::guest::on_process_event_pre(ctx, fn)) data.PreventOriginalFunctionCall();
                });
            },
            opts));
        callbacks_.push_back(Hook::RegisterProcessEventPostCallback(
            [](auto&, UObject* ctx, UFunction* fn, void*) { guarded(L"ProcessEvent.post", [&] { lie::guest::on_process_event_post(ctx, fn); }); },
            opts));
        lie::log::info(L"armed (EXE %hs verified)", g_bound->exe_sha256.c_str());
    }

    void on_lua_start(StringViewType mod_name, Lua& lua, Lua& main, Lua& async, Lua* hook) override {
        const std::wstring name(mod_name);
        if (name != STR("LiliesGuest") && name != STR("UEBridge")) return;
        for (Lua* st : {&lua, &main, &async, hook}) {
            if (!st) continue;
            st->register_function("LIE_GuestStatus", lua_status);
            st->register_function("LIE_GuestAct", lua_act);
            st->register_function("LIE_GuestDebug", lua_debug);
        }
    }

private:
    std::vector<RC::Unreal::Hook::GlobalCallbackId> callbacks_;
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod() { return new LiliesGuestMod(); }
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
