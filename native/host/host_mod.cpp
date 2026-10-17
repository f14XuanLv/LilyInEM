// MagnoliaHost: UE4SS C++ mod for ENDER MAGNOLIA (world owner, parent process).
// - fail-closed binary check (data/binary.json: module + SHA-256 + function prefixes)
// - launches the headless EL Guest from LilyInEM.ini and ties its lifetime to EM with a Job Object (FR-ENG-5)
// - UEngine::Tick PRE drives h_world (before EM's world / camera tick); Present drives h_render
// - Lua API for debugging / AI only

#include "host/host.h"

#include "rt/binary.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>

#include "rt/clock.h" // Windows.h before TlHelp32.h
#include <TlHelp32.h>

#include <filesystem>
#include <optional>

using RC::LuaMadeSimple::Lua;

namespace {

std::optional<lie::binary::Bound> g_bound;
HANDLE g_job = nullptr;

template <class F>
void guarded(const wchar_t* where, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        static int reports = 0;
        if (reports++ < 20) lie::log::error(L"%s: %hs", where, e.what());
    }
}

std::wstring ini(const std::filesystem::path& file, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[2048]{};
    GetPrivateProfileStringW(L"Guest", key, fallback, buf, 2048, file.c_str());
    return buf;
}

bool running(const std::wstring& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    const std::wstring name = std::filesystem::path(exe).filename().wstring();
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t path[MAX_PATH]{};
            DWORD len = MAX_PATH;
            found = QueryFullProcessImageNameW(h, 0, path, &len) && _wcsicmp(path, exe.c_str()) == 0;
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return found;
}

void launch_guest() {
    const auto file = lie::binary::mod_dir() / L"LilyInEM.ini";
    if (!std::filesystem::exists(file) || ini(file, L"AutoLaunch", L"1") != L"1") return;
    const std::wstring exe = ini(file, L"Exe", L"");
    if (exe.empty() || !std::filesystem::exists(exe)) {
        lie::log::warn(L"guest exe not found (%s): auto-launch skipped", exe.c_str());
        return;
    }
    if (running(exe)) {
        lie::log::info(L"guest already running");
        return;
    }
    std::wstring cmd = L"\"" + exe + L"\" -dx11 -windowed -ResX=1280 -ResY=720 -MultiprocessOSS -SaveToUserDir";
    const std::wstring user_dir = ini(file, L"UserDir", L"");
    if (!user_dir.empty()) cmd += L" -UserDir=\"" + user_dir + L"\"";
    cmd += L" " + ini(file, L"ExtraArgs", L"");
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // EM exits -> EL exits
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE; // never minimised: a minimised UE4 window stops presenting
    PROCESS_INFORMATION pi{};
    const std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        lie::log::error(L"guest launch failed (%lu)", GetLastError());
        return;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    lie::log::info(L"launched guest: %s", cmd.c_str());
}

int lua_status(const Lua& lua) {
    lua.set_string(lie::host::status());
    return 1;
}

// LIE_HostAct(x, y, held_bits, tap_bits, frames): scripted input through the normal Host input path
int lua_act(const Lua& lua) {
    const float x = lua.is_number() ? static_cast<float>(lua.get_number()) : 0.0f;
    const float y = lua.is_number() ? static_cast<float>(lua.get_number()) : 0.0f;
    const auto held = lua.is_integer() ? static_cast<std::uint32_t>(lua.get_integer()) : 0u;
    const auto taps = lua.is_integer() ? static_cast<std::uint32_t>(lua.get_integer()) : 0u;
    const int frames = lua.is_integer() ? static_cast<int>(lua.get_integer()) : 10;
    lie::host::scripted_input(x, y, held, taps, frames);
    lua.set_bool(true);
    return 1;
}

// LIE_HostDebug("rebase" | "menu" | "menu2" | "menu3" | "menu4" | "restore" | "kill" | "terrain" | "lilac" | "bridge")
int lua_debug(const Lua& lua) {
    if (lua.is_string()) {
        const std::string cmd(lua.get_string());
        lie::host::debug(std::wstring(cmd.begin(), cmd.end()));
    }
    lua.set_bool(true);
    return 1;
}

} // namespace

class MagnoliaHostMod final : public RC::CppUserModBase {
public:
    MagnoliaHostMod() {
        ModName = STR("MagnoliaHost");
        ModVersion = STR("0.3.0");
        ModDescription = STR("LilyInEM Host: Ender Magnolia world owner");
        ModAuthors = STR("LilyInEM");
        ModIntendedSDKVersion = STR("3.0");
        lie::log::tag() = L"MagnoliaHost";
        lie::shm::open();
    }

    ~MagnoliaHostMod() override {
        for (auto id : callbacks_) RC::Unreal::Hook::UnregisterCallback(id);
        lie::hook::detach_all();
        lie::render::shutdown();
        lie::shm::close();
        if (g_job) CloseHandle(g_job);
    }

    void on_unreal_init() override {
        std::string why;
        g_bound = lie::binary::load_for_current_process(&why);
        if (!g_bound) {
            lie::log::error(L"binary check failed, mod idle: %hs", why.c_str());
            return;
        }
        guarded(L"launch_guest", [] { launch_guest(); });
        lie::render::install();
        lie::host::combat_arm(g_bound->manifest);

        using namespace RC::Unreal;
        const Hook::FCallbackOptions opts{false, false, STR("MagnoliaHost"), STR("LIE")};
        callbacks_.push_back(Hook::RegisterEngineTickPreCallback(
            [](auto&, UEngine*, float dt, bool) { guarded(L"tick", [&] { lie::host::tick_pre(dt); }); }, opts));
        callbacks_.push_back(Hook::RegisterBeginPlayPostCallback(
            [](auto&, AActor* actor) { guarded(L"BeginPlay", [&] { lie::host::on_begin_play(actor); }); }, opts));
        callbacks_.push_back(Hook::RegisterProcessEventPreCallback(
            [](auto& data, UObject* ctx, UFunction* fn, void* parms) {
                guarded(L"ProcessEvent.pre", [&] {
                    if (lie::host::on_process_event_pre(ctx, fn, parms)) data.PreventOriginalFunctionCall();
                });
            },
            opts));
        callbacks_.push_back(Hook::RegisterProcessEventPostCallback(
            [](auto&, UObject* ctx, UFunction* fn, void* parms) { guarded(L"ProcessEvent.post", [&] { lie::host::on_process_event_post(ctx, fn, parms); }); },
            opts));
        lie::log::info(L"armed (EXE %hs verified)", g_bound->exe_sha256.c_str());
    }

    void on_lua_start(StringViewType mod_name, Lua& lua, Lua& main, Lua& async, Lua* hook) override {
        const std::wstring name(mod_name);
        if (name != STR("MagnoliaHost") && name != STR("UEBridge")) return;
        for (Lua* st : {&lua, &main, &async, hook}) {
            if (!st) continue;
            st->register_function("LIE_HostStatus", lua_status);
            st->register_function("LIE_HostAct", lua_act);
            st->register_function("LIE_HostDebug", lua_debug);
        }
    }

private:
    std::vector<RC::Unreal::Hook::GlobalCallbackId> callbacks_;
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod() { return new MagnoliaHostMod(); }
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
