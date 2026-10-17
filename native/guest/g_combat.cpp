// Guest combat (FR-CMB-1/2/3).
//
// Damage TO EM enemies: every EM enemy / breakable near Lilac has an invisible EL proxy built from a real EL
// enemy blueprint ([IDA+CUE4P] only BP_Character_Enemy_Base_C children carry the UCollisionComponent +
// HurtCapsule that EL hitboxes accept). EL's native hitboxes, damage formula, relics and spirit levels settle
// the hit on the proxy; the native UHPComponent::DoDamage detour captures the final number, refills the proxy
// and sends HitEnemy to the Host, which settles it natively on the EM enemy.
//
// Damage TO Lily: the Host intercepted EM's settlement on Lilac and sent Hurt. It is replayed here through EL's
// own entry points (HP + knockback); dodge i-frames are respected because EL decides. A hit that would kill
// Lily is capped at 1 HP and reported as Lethal: EM owns death and respawn (FR-HDL-4).

#include "guest/guest.h"

#include "core/combat_map.h"
#include "core/facts.h"
#include "rt/clock.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace lie::guest {

namespace {

using ue::Call;
using ue::V3;

// [CUE4P] smallest EL enemy blueprint deriving from BP_Character_Enemy_Base_C
constexpr const wchar_t* kProxyClass = L"/Game/_Zenith/Characters/e2010_Slime/BP_e2010_Slime.BP_e2010_Slime_C";
constexpr int kElFactionEnemies = 2; // [SDK] EFaction (Zenith)

struct Proxy {
    ue::Weak actor;
    UObject* hp = nullptr; // identity only (compared, never dereferenced without the actor alive)
    std::uint32_t entity = 0;
    bool active = false;
};

std::vector<Proxy> g_proxies;
std::unordered_map<const UObject*, std::size_t> g_by_hp;
std::unique_ptr<EntitySnapshot> g_entities = std::make_unique<EntitySnapshot>();
ue::Weak g_lily_hp;
bool g_lethal_pending = false;
bool g_proxy_class_missing = false;
clock::Every g_reassert{1.0};

using DoDamageFn = void(__fastcall*)(UObject* hp, UObject* from, std::int32_t damage, bool just_guarding, bool guarding);
DoDamageFn g_do_damage = nullptr;

void send(EventType type, const auto& payload) {
    if (World* w = shm::world()) w->to_host.push(Event::make(type, payload, clock::now_s()));
}

void __fastcall on_do_damage(UObject* hp, UObject* from, std::int32_t damage, bool just_guarding, bool guarding) {
    if (hp && hp == g_lily_hp.get()) {
        const auto curr = static_cast<std::int32_t>(ue::num(hp, L"CurrHP"));
        const combat::LethalSplit s = combat::split_lethal(damage, curr);
        if (s.lethal) {
            if (s.apply > 0) g_do_damage(hp, from, s.apply, just_guarding, guarding);
            if (!g_lethal_pending) {
                g_lethal_pending = true;
                send(EventType::Lethal, LethalPayload{damage});
                ++counters().lethal_sent;
                log::info(L"lethal hit (%d >= %d HP): EM runs death / respawn", damage, curr);
            }
            return;
        }
        g_do_damage(hp, from, damage, just_guarding, guarding);
        return;
    }
    if (auto it = g_by_hp.find(hp); it != g_by_hp.end() && it->second < g_proxies.size()) {
        Proxy& p = g_proxies[it->second];
        g_do_damage(hp, from, damage, just_guarding, guarding); // EL on-hit reactions (relics, MP gain) stay native
        ue::call(hp, L"FullyRestore");                          // proxies never die: EM decides
        ++counters().proxy_hits;
        if (p.active && damage > 0) {
            HitEnemyPayload h;
            h.entity_id = p.entity;
            h.damage_el = damage;
            if (UObject* pc = ue::first(L"ZenithPlayerController")) {
                Call atk(ue::obj(pc, L"ParameterPlayerComponent"), L"GetAttack");
                if (atk.b(false).go()) h.attack_el = static_cast<std::int32_t>(atk.ri());
            }
            const V3 pl = ue::actor_location(p.actor.get());
            const V3 ll = ue::actor_location(lily());
            h.dir_y = pl.y >= ll.y ? 1.0f : -1.0f;
            send(EventType::HitEnemy, h);
            ++counters().hit_events;
        }
        return;
    }
    g_do_damage(hp, from, damage, just_guarding, guarding);
}

void freeze_proxy(UObject* a) {
    Call(a, L"SetActorHiddenInGame").b(true).go();
    Call(a, L"SetActorTickEnabled").b(false).go();
    if (UObject* cmc = ue::obj(a, L"CharacterMovement")) Call(cmc, L"SetComponentTickEnabled").b(false).go();
    if (UObject* hit = ue::obj(a, L"HitCapsule")) Call(hit, L"SetCollisionEnabled").i(0).go(); // contact damage only via Hurt events
    if (UObject* fc = ue::obj(a, L"FactionComponent")) Call(fc, L"SetFaction").i(kElFactionEnemies).go();
}

UObject* spawn_proxy(UObject* world, const V3& at) {
    UClass* cls = ue::load_class(kProxyClass);
    if (!cls) {
        if (!g_proxy_class_missing) log::error(L"proxy class not loadable: %s (enemy damage disabled)", kProxyClass);
        g_proxy_class_missing = true;
        return nullptr;
    }
    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(cls).xf(at).i(1).o(nullptr);
    if (!begin.go() || !begin.ro()) return nullptr;
    UObject* a = begin.ro();
    stage_register_owned(a);
    ue::set_num(a, L"AutoPossessAI", 0); // EAutoPossessAI::Disabled: no AI controller, no behaviour
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(a).xf(at).go();
    freeze_proxy(a);
    return a;
}

void park(Proxy& p) {
    if (UObject* a = p.actor.get()) Call(a, L"SetActorEnableCollision").b(false).go();
    p.active = false;
    p.entity = 0;
}

void sync_proxies(UObject* world, const FrameMap& map, double plane_x) {
    World* w = shm::world();
    if (!w || !map.valid() || !w->entities.read(*g_entities)) return;
    std::vector<bool> used(g_proxies.size(), false);
    const bool reassert = g_reassert.due(clock::now_s());
    for (std::uint32_t i = 0; i < g_entities->count && i < kMaxEntities; ++i) {
        const Entity& e = g_entities->items[i];
        if (e.kind != EntityKind::Enemy && e.kind != EntityKind::Breakable) continue;
        std::size_t slot = g_proxies.size();
        for (std::size_t k = 0; k < g_proxies.size(); ++k) {
            if (g_proxies[k].active && g_proxies[k].entity == e.id) { slot = k; break; }
        }
        if (slot == g_proxies.size()) {
            for (std::size_t k = 0; k < g_proxies.size(); ++k) {
                if (!g_proxies[k].active && !used[k] && g_proxies[k].actor.get()) { slot = k; break; }
            }
        }
        const V3 at{plane_x, map.guest_y(e.y), map.guest_z(e.z)};
        if (slot == g_proxies.size()) {
            if (g_proxy_class_missing) return;
            UObject* a = spawn_proxy(world, at);
            if (!a) return;
            Proxy p;
            p.actor.set(a);
            p.hp = ue::obj(a, L"HPComponent");
            g_proxies.push_back(p);
            used.push_back(false);
            if (p.hp) g_by_hp[p.hp] = slot;
        }
        Proxy& p = g_proxies[slot];
        used[slot] = true;
        UObject* a = p.actor.get();
        if (!a) continue;
        if (!p.active || p.entity != e.id) {
            p.entity = e.id;
            p.active = true;
            Call(a, L"SetActorEnableCollision").b(true).go();
            if (UObject* hurt = ue::obj(a, L"HurtCapsule")) Call(hurt, L"SetCapsuleSize").f(std::max(e.hy, 1.0f)).f(std::max(e.hz, e.hy)).b(true).go();
        }
        if (reassert) freeze_proxy(a); // EL activation paths may re-enable tick / AI
        ue::set_actor_location(a, at);
    }
    for (std::size_t k = 0; k < g_proxies.size(); ++k) {
        if (!used[k] && g_proxies[k].active) park(g_proxies[k]);
    }
}

} // namespace

void combat_arm(const binary::Manifest& m) {
    hook::attach(m, "HPComponentDoDamage", reinterpret_cast<void*>(&on_do_damage), reinterpret_cast<void**>(&g_do_damage));
}

void combat_tick(UObject* world, UObject* pc, UObject* lily_pawn, const FrameMap& map, double plane_x) {
    UObject* hp = ue::obj(lily_pawn, L"HPComponent");
    if (g_lily_hp.get() != hp) g_lily_hp.set(hp);
    if (g_lethal_pending && hp && ue::num(hp, L"CurrHP") >= ue::num(hp, L"MaxHP")) g_lethal_pending = false; // restored after EM respawn
    if (host_alive()) sync_proxies(world, map, plane_x);
    (void)pc;
}

void combat_on_hurt(UObject* lily_pawn, const HurtPayload& hurt) {
    ++counters().hurt_received;
    UObject* hp = ue::obj(lily_pawn, L"HPComponent");
    if (!hp || !ue::call_bool(hp, L"IsAlive")) return;
    if (ue::call_bool(hp, L"IsNullifyingDamage")) return; // EL dodge / post-hit invincibility decides (FR-CMB-3)
    const auto dmg = combat::hurt_to_el(hurt.damage_em, static_cast<std::int32_t>(ue::num(hp, L"MaxHP")), hurt.max_hp_em);
    const V3 dir{0.0, hurt.dir_y >= 0.0f ? 1.0 : -1.0, 0.0};
    Call(lily_pawn, L"SetLastAttackerDirection").v(dir).go();
    Call(hp, L"DoDamage").o(nullptr).i(dmg).b(false).b(false).go(); // native body -> our detour applies the lethal cap
    ++counters().hurt_replayed;
    if (hurt.knockback) {
        UObject* kb = ue::obj(lily_pawn, L"KnockbackComponent");
        ue::FProperty* data = kb ? ue::prop(kb->GetClassPrivate(), L"AirborneLaunchKnockback") : nullptr;
        if (data) {
            Call launch(kb, L"LaunchKnockback");
            launch.copy(reinterpret_cast<std::uint8_t*>(kb) + data->GetOffset_Internal()).v(dir).go();
            ++counters().knockback_replayed;
        }
    }
}

void combat_reset() {
    for (Proxy& p : g_proxies) park(p);
    g_lethal_pending = false;
}

} // namespace lie::guest
