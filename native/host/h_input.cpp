// Live key layout (game thread), docs/INPUT_MATRIX.md:
// - Lily's actions: the player's EL bindings (published by the Guest, Control::el_bindings), plus EM's interact key,
//   minus every key EM keeps in Lily mode (pause / game menu / minimap / camera offset / interact): EM wins, the
//   dropped Lily binding is logged and toasted. EM's keys: InputSettingsSubsystem::GetMappedKey.
// - Lily's menus: EM's UI keys. EM has no getter for them, so every known key is probed with
//   UMGBPFLibrary::IsKeyUIInputType; the Guest binds EL's UI to the same keys (Control::ui_layout).
// The result goes to the render thread (input::set_layout).

#include "host/host.h"

#include "core/input_map.h"
#include "rt/clock.h"
#include "rt/input.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace lie::host {

namespace {

using ue::Call;

struct KeyArg { // FKey: FName KeyName + TSharedPtr<FKeyDetails> (null: resolved by name)
    RC::Unreal::FName name;
    void* details[2]{};
};

struct State {
    clock::Every world{2.0}, ui{10.0}; // the UI probe is ~1300 reflected calls: EM's UI keys rarely change
    bool have_world = false, have_el = false, have_ui = false;
    double ui_ms = 0.0; // last UI probe duration (game thread)
    input::EmWorldKeys em = input::em_default_world_keys();
    ElBindings el{};
    input::Bindings b{};
    int conflicts = 0;
    bool toasted = false;
    input::UiKeys u = input::em_default_ui_keys();
    UiLayout layout{};
    std::string kb_names[menu::Count][2], pad_names[menu::Count];
} g;

std::string narrow(const std::wstring& w) { // FKey names are ASCII
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::string mapped_key(UObject* settings, const char* mapping) {
    const std::string m(mapping);
    const std::wstring wm(m.begin(), m.end());
    Call c(settings, L"GetMappedKey");
    ue::FProperty* rv = c.param(L"ReturnValue");
    if (!rv || !c.n(wm.c_str()).go()) return {};
    return narrow(ue::str(static_cast<const KeyArg*>(c.slot(rv))->name));
}

// a resolvable key replaces the default; an unknown name keeps it (never drops an EM key by accident)
void take(Input& slot, UObject* settings, const char* mapping) {
    const Input in = input::input_from_name(mapped_key(settings, mapping).c_str());
    if (in.valid()) slot = in;
}

bool read_world(input::EmWorldKeys& e) {
    UObject* s = ue::first(L"InputSettingsSubsystem");
    if (!s) return false;
    e = input::em_default_world_keys();
    take(e.interact_kb, s, input::kEmInteract[0]);
    take(e.interact_pad, s, input::kEmInteract[1]);
    for (int i = 0; i < input::kEmReservedCount; ++i) take(e.reserved[i], s, input::kEmReserved[i]);
    return true;
}

std::string key_label(const Input& in) { // names from for_each_key_name live in a scratch buffer: copy
    std::string found = in.valid() ? "?" : "-";
    input::for_each_key_name([&](const char* n) {
        if ((found == "?") && input::input_from_name(n) == in) found = n;
    });
    return found;
}

// [SDK] Zenith ECommandInputTypes, for the conflict report
constexpr const char* kCmdLabel[cmd::Count] = {"移动", "跳", "闪避", "主技能", "副技能1", "副技能2", "切换精灵组", "钩爪", "祈祷", "交互"};
constexpr const char* kReservedLabel[] = {"暂停", "暂停", "游戏菜单", "游戏菜单", "小地图", "小地图",
                                          "镜头上", "镜头下", "镜头左", "镜头右", "交互", "交互"};
static_assert(sizeof(kReservedLabel) / sizeof(kReservedLabel[0]) == input::kEmReservedCount);

constexpr const char* kMoveLabel[4] = {"上", "下", "左", "右"};

// Every time the composition changes (EL rebinding lands within a frame, EM settings within 2 s):
// - row masks for the Guest, which tints the conflicting rows of EL's key binding pages red
// - a persistent list for the Host's banner over those pages, and a toast when something new conflicts
void compose() {
    input::Conflict c[16];
    const int before = g.conflicts;
    g.conflicts = input::compose_bindings(g.el, g.em, g.b, c, 16);
    std::uint32_t kb = 0, pad = 0;
    std::string list;
    for (int i = 0; i < std::min(g.conflicts, 16); ++i) {
        const std::string key = key_label(c[i].in);
        log::warn(L"Lily binding dropped: %hs on %hs (EM %hs wins)", input::kElAction[c[i].cmd] ? input::kElAction[c[i].cmd] : "Move",
                  key.c_str(), input::kEmReserved[c[i].reserved]);
        (c[i].in.vk ? kb : pad) |= input::conflict_bit(c[i]);
        char line[160];
        std::snprintf(line, sizeof(line), "%s%s%s（%s）→ EM %s", list.empty() ? "" : "\n", c[i].cmd ? kCmdLabel[c[i].cmd] : "移动",
                      c[i].cmd ? "" : kMoveLabel[c[i].move], key.c_str(), kReservedLabel[c[i].reserved]);
        list += line;
    }
    if (Control* ctl = shm::control()) ctl->conflicts_kb = kb, ctl->conflicts_pad = pad;
    render::set_binding_conflicts(list.c_str());
    if (g.conflicts > before || (g.conflicts && !g.toasted)) {
        render::toast(("莉莉键位与 EM 撞键，已失效（EM 优先）：" + list + "\nF5 → 设置 → 控制器 / 键盘 里改成别的键").c_str(), 6.0);
        g.toasted = true;
    }
    if (!g.conflicts && before) render::toast("莉莉键位冲突已解除", 2.5);
}

bool read_ui(UObject* pc, input::UiKeys& u, std::string kb[menu::Count][2], std::string pad[menu::Count]) {
    UObject* lib = ue::find(L"/Script/Zion.Default__UMGBPFLibrary");
    if (!lib || !pc) return false;
    u = {};
    for (int i = 0; i < menu::Count; ++i) kb[i][0].clear(), kb[i][1].clear(), pad[i].clear();
    bool any = false;
    input::for_each_key_name([&](const char* name) {
        const input::Input in = input::input_from_name(name);
        const KeyArg key{ue::fname(std::wstring(name, name + std::strlen(name)).c_str())};
        for (int i = 0; i < menu::Count; ++i) {
            Call c(lib, L"IsKeyUIInputType");
            if (!c.o(pc).copy(&key).i(input::kEmUiType[i]).go() || !c.rb()) continue;
            any = true;
            if (in.vk) {
                for (int s = 0; s < 2; ++s) {
                    if (u.kb[i][s].valid()) continue;
                    u.kb[i][s] = in, kb[i][s] = name;
                    break;
                }
            } else if (!u.pad[i].valid()) {
                u.pad[i] = in, pad[i] = name;
            }
        }
    });
    if (!any) return false;
    const input::UiKeys def = input::em_default_ui_keys(); // an intent EM left unbound keeps EM's default
    for (int i = 0; i < menu::Count; ++i) {
        if (!u.kb[i][0].valid()) u.kb[i][0] = def.kb[i][0], u.kb[i][1] = def.kb[i][1];
        if (!u.pad[i].valid()) u.pad[i] = def.pad[i];
    }
    return true;
}

// Keyboard keys EL can be sent through WM_KEYDOWN (not mouse buttons)
bool postable(const input::Input& in) { return in.vk >= 0x08; }

UiLayout make_layout() {
    UiLayout l{};
    l.valid = 1;
    for (int i = 0; i < menu::Count; ++i) {
        if (input::kElUiType[i] == 0) { // navigation: EL keeps its arrows / d-pad
            l.vk[i] = static_cast<std::uint8_t>(input::kElNavVk[i]);
            continue;
        }
        const int s = postable(g.u.kb[i][0]) ? 0 : (postable(g.u.kb[i][1]) ? 1 : -1);
        if (s >= 0 && !g.kb_names[i][s].empty()) {
            l.vk[i] = static_cast<std::uint8_t>(g.u.kb[i][s].vk);
            std::snprintf(l.kb[i], sizeof(l.kb[i]), "%s", g.kb_names[i][s].c_str());
        }
        if (!g.pad_names[i].empty()) std::snprintf(l.pad[i], sizeof(l.pad[i]), "%s", g.pad_names[i].c_str());
    }
    return l;
}

} // namespace

void input_layout_tick(UObject* pc, double now) {
    bool changed = false, recompose = false;
    if (g.world.due(now)) {
        input::EmWorldKeys e;
        if (read_world(e) && (!g.have_world || !(e == g.em))) {
            g.em = e;
            g.have_world = recompose = true;
            log::info(L"EM world keys read (interact %hs / %hs)", key_label(g.em.interact_kb).c_str(), key_label(g.em.interact_pad).c_str());
        }
    }
    ElBindings el{};
    if (Control* c = shm::control(); c && c->el_bindings.read(el) && el.valid && (!g.have_el || !(el == g.el))) {
        g.el = el;
        g.have_el = recompose = true;
        std::string list;
        for (int k = 1; k < cmd::Count; ++k) {
            if (input::kElAction[k]) list += std::string(" ") + input::kElAction[k] + "=" + key_label(el.kb[k][0]) + "/" + key_label(el.pad[k][0]);
        }
        log::info(L"EL bindings received:%hs", list.c_str());
    }
    if (recompose && g.have_el) {
        compose();
        changed = true;
    }
    if (g.ui.due(now)) {
        input::UiKeys u;
        std::string kb[menu::Count][2], pad[menu::Count];
        const double t0 = clock::now_s();
        const bool ok = read_ui(pc, u, kb, pad);
        g.ui_ms = (clock::now_s() - t0) * 1000.0;
        if (ok && (!g.have_ui || !(u == g.u))) {
            g.u = u;
            for (int i = 0; i < menu::Count; ++i) g.kb_names[i][0] = kb[i][0], g.kb_names[i][1] = kb[i][1], g.pad_names[i] = pad[i];
            g.have_ui = changed = true;
            g.layout = make_layout();
            log::info(L"EM UI layout read (confirm %hs/%hs, cancel %hs/%hs, next %hs/%hs)", g.layout.kb[menu::Confirm],
                      g.layout.pad[menu::Confirm], g.layout.kb[menu::Cancel], g.layout.pad[menu::Cancel], g.layout.kb[menu::Next],
                      g.layout.pad[menu::Next]);
        }
    }
    if (changed) input::set_layout(g.b, g.u);
    if (g.have_ui) {
        if (Control* c = shm::control()) c->ui_layout.store(g.layout);
    }
}

std::string input_layout_status() {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "layout el=%d em=%d ui=%d conflicts=%d uiProbeMs=%.2f", g.have_el ? 1 : 0, g.have_world ? 1 : 0,
                  g.have_ui ? 1 : 0, g.conflicts, g.ui_ms);
    return buf;
}

} // namespace lie::host
