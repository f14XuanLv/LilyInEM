// Guest combat (FR-CMB-1/2/3).
//
// Damage TO EM enemies / breakables: each one near Lilac has an invisible EL proxy of the matching EL kind, so EL's
// native hitboxes, damage formula, relics and spirit levels settle Lily's hit on it:
//   enemy     -> BP_e2010_Slime_C   ([IDA+CUE4P] BP_Character_Enemy_Base_C children carry the UCollisionComponent +
//                                    HurtCapsule that EL hitboxes accept); stripped down to the HurtCapsule
//   breakable -> BP_Breakable_Base_C ([CUE4P] Destructible-type Box that only overlaps, HPComponent, empty
//                                    OnPlayerOverlap; break FX, drops and the clear flag only run on death)
// Ability hits settle in AE_Damage, which writes the target's HP itself (not through UHPComponent::DoDamage): that
// detour measures the HP drop on a huge proxy HP pool and refills it, so the proxy never dies. HitEnemy goes to the
// Host, which settles it natively in EM.
//
// Damage TO Lily: the Host intercepted EM's settlement on Lilac and sent Hurt (damage and / or knockback exactly as
// EM would apply it to Lilac). It is replayed through EL's own entry points (HP + LaunchKnockback); dodge i-frames
// are respected because EL decides. A hit that would kill Lily is capped at 1 HP and reported as Lethal: EM owns
// death and respawn (FR-HDL-4).

#include "guest/guest.h"

#include "core/combat_map.h"
#include "rt/clock.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lie::guest {

namespace {

using ue::Call;
using ue::V3;

struct ProxyClass {
    EntityKind kind;
    const wchar_t* path;
    bool missing = false;
};
ProxyClass g_classes[] = {
    {EntityKind::Enemy, L"/Game/_Zenith/Characters/e2010_Slime/BP_e2010_Slime.BP_e2010_Slime_C"},
    {EntityKind::Breakable, L"/Game/_Zenith/Gameplay/LevelDesign/Breakables/BP_Breakable_Base.BP_Breakable_Base_C"},
};
// [POLICY] spawns per tick, so a burst of new entities (or a proxy EL keeps destroying) cannot stall a frame
constexpr int kSpawnsPerTick = 4;
constexpr int kElFactionEnemies = 2; // [SDK] EFaction (Zenith)
// [CUE4P] BP_Breakable_Base Box depth along X (the 2D plane's normal); Y / Z come from the EM entity
constexpr double kBreakableDepth = 300.0;

struct Proxy {
    ue::Weak actor;
    EntityKind kind = EntityKind::Enemy;
    UObject* hp = nullptr; // identity only (compared, never dereferenced without the actor alive)
    std::uint32_t entity = 0;
    bool active = false;
};

std::vector<Proxy> g_proxies;
std::unordered_map<const UObject*, std::size_t> g_by_hp;
std::unique_ptr<EntitySnapshot> g_entities = std::make_unique<EntitySnapshot>();
ue::Weak g_lily_hp;
bool g_lethal_pending = false;
clock::Every g_reassert{1.0};

using DoDamageFn = void(__fastcall*)(UObject* hp, UObject* from, std::int32_t damage, bool just_guarding, bool guarding);
DoDamageFn g_do_damage = nullptr;
using AEDamageFn = void(__fastcall*)(UObject* effect, UObject* instigator, UObject* target, void* entry);
AEDamageFn g_ae_damage = nullptr;

// [POLICY] proxy HP pool: far above any single EL hit, so a proxy never reaches 0 (no death FX, never destroyed)
constexpr double kProxyHP = 100000000.0;

void send(EventType type, const auto& payload) {
    if (World* w = shm::world()) w->to_host.push(Event::make(type, payload, clock::now_s()));
}

void report_hit(const Proxy& p, std::int32_t damage) {
    ++counters().proxy_hits;
    if (!p.active || damage <= 0) return;
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

Proxy* proxy_of_actor(const UObject* actor) {
    if (!actor) return nullptr;
    for (Proxy& p : g_proxies) {
        if (p.actor.get() == actor) return &p;
    }
    return nullptr;
}

void refill(UObject* hp) {
    ue::set_num(hp, L"MaxHP", kProxyHP);
    ue::set_num(hp, L"CurrHP", kProxyHP);
}

// Lily's ability hits: AE_Damage computes the damage (relics, spirit level, stance) and writes the target's CurrHP
// itself [IDA 0x140CB07E0] -- UHPComponent::DoDamage is never called. On a proxy the original runs against a huge HP
// pool (attacker-side reactions such as MP gain and relics stay native), the drop is the hit's damage, then refill.
void __fastcall on_ae_damage(UObject* effect, UObject* instigator, UObject* target, void* entry) {
    Proxy* p = proxy_of_actor(target);
    UObject* hp = p && p->actor.get() ? ue::obj(p->actor.get(), L"HPComponent") : nullptr;
    if (!hp) {
        g_ae_damage(effect, instigator, target, entry);
        return;
    }
    if (ue::num(hp, L"CurrHP") < kProxyHP) refill(hp);
    g_ae_damage(effect, instigator, target, entry);
    const auto dealt = static_cast<std::int32_t>(std::llround(kProxyHP - ue::num(hp, L"CurrHP")));
    refill(hp);
    report_hit(*p, dealt);
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
        // direct DoDamage callers (not ability hits, see on_ae_damage): take the number, never settle it on the proxy
        report_hit(g_proxies[it->second], damage);
        return;
    }
    g_do_damage(hp, from, damage, just_guarding, guarding);
}

void collision_off(UObject* comp) {
    if (!comp) return;
    Call(comp, L"SetCollisionEnabled").i(0).go();
    Call(comp, L"SetGenerateOverlapEvents").b(false).go();
}

// A proxy is a hurtbox and nothing else. Enemy proxies keep only the HurtCapsule: no contact damage (HitCapsule), no
// body to bump into (capsule), no AI wake-up (TickTrigger). The slime's OnFactionChange handler [live EL probe] both
// gives HurtCapsule its Enemy-Hurtbox profile (without it EL hitboxes ignore the proxy) and turns HitCapsule on
// (contact damage: EM breakables used to knock Lily back). So: SetFaction only while not yet Enemies, then strip.
void freeze_proxy(const Proxy& p, UObject* a) {
    Call(a, L"SetActorHiddenInGame").b(true).go();
    Call(a, L"SetActorTickEnabled").b(false).go();
    if (p.kind != EntityKind::Enemy) return;
    if (UObject* cmc = ue::obj(a, L"CharacterMovement")) Call(cmc, L"SetComponentTickEnabled").b(false).go();
    if (UObject* fc = ue::obj(a, L"FactionComponent"); fc && static_cast<int>(ue::num(fc, L"Faction")) != kElFactionEnemies) {
        Call(fc, L"SetFaction").i(kElFactionEnemies).go();
    }
    for (const wchar_t* c : {L"HitCapsule", L"CapsuleComponent", L"TickTrigger"}) collision_off(ue::obj(a, c));
}

void fit_hurtbox(const Proxy& p, UObject* a, const Entity& e) {
    const double hy = std::max(e.hy, 1.0f), hz = std::max(e.hz, 1.0f);
    if (p.kind == EntityKind::Enemy) {
        if (UObject* hurt = ue::obj(a, L"HurtCapsule")) Call(hurt, L"SetCapsuleSize").f(hy).f(std::max(hz, hy)).b(true).go();
        return;
    }
    if (UObject* box = ue::obj(a, L"Box")) {
        Call(box, L"K2_SetRelativeLocation").v({}).b(false).skip().b(true).go(); // centred on the actor = the EM hurtbox centre
        Call(box, L"SetBoxExtent").v({kBreakableDepth, hy, hz}).b(true).go();
    }
}

ProxyClass* class_for(EntityKind kind) {
    for (ProxyClass& c : g_classes) {
        if (c.kind == kind) return &c;
    }
    return nullptr;
}

UObject* spawn_proxy(UObject* world, const V3& at, ProxyClass& pc) {
    UClass* cls = ue::load_class(pc.path);
    if (!cls) {
        if (!pc.missing) log::error(L"proxy class not loadable: %s (damage on that kind disabled)", pc.path);
        pc.missing = true;
        return nullptr;
    }
    Call begin(ue::gameplay_statics(), L"BeginDeferredActorSpawnFromClass");
    begin.o(world).o(cls).xf(at).i(1).o(nullptr);
    if (!begin.go() || !begin.ro()) return nullptr;
    UObject* a = begin.ro();
    stage_register_owned(a);
    if (pc.kind == EntityKind::Enemy) ue::set_num(a, L"AutoPossessAI", 0); // EAutoPossessAI::Disabled: no AI controller
    Call(ue::gameplay_statics(), L"FinishSpawningActor").o(a).xf(at).go();
    return a;
}

void park(Proxy& p) {
    if (UObject* a = p.actor.get()) Call(a, L"SetActorEnableCollision").b(false).go();
    p.active = false;
    p.entity = 0;
}

// EL may still destroy a proxy (level streaming, kill volumes): drop the slot so the entity gets a new one
void purge_dead() {
    const auto dead = [](const Proxy& p) { return !p.actor.get(); };
    if (std::none_of(g_proxies.begin(), g_proxies.end(), dead)) return;
    std::erase_if(g_proxies, dead);
    g_by_hp.clear();
    for (std::size_t k = 0; k < g_proxies.size(); ++k) {
        if (g_proxies[k].hp) g_by_hp[g_proxies[k].hp] = k;
    }
}

void sync_proxies(UObject* world, const FrameMap& map, double plane_x) {
    World* w = shm::world();
    if (!w || !map.valid() || !w->entities.read(*g_entities)) return;
    purge_dead();
    std::vector<bool> used(g_proxies.size(), false);
    const bool reassert = g_reassert.due(clock::now_s());
    int spawns = 0;
    for (std::uint32_t i = 0; i < g_entities->count && i < kMaxEntities; ++i) {
        const Entity& e = g_entities->items[i];
        ProxyClass* pc = class_for(e.kind);
        if (!pc) continue;
        std::size_t slot = g_proxies.size();
        for (std::size_t k = 0; k < g_proxies.size(); ++k) {
            if (g_proxies[k].active && g_proxies[k].entity == e.id && g_proxies[k].kind == e.kind) { slot = k; break; }
        }
        if (slot == g_proxies.size()) {
            for (std::size_t k = 0; k < g_proxies.size(); ++k) {
                if (!g_proxies[k].active && !used[k] && g_proxies[k].kind == e.kind) { slot = k; break; }
            }
        }
        const V3 at{plane_x, map.guest_y(e.y), map.guest_z(e.z)};
        if (slot == g_proxies.size()) {
            if (pc->missing || spawns >= kSpawnsPerTick) continue;
            ++spawns;
            UObject* a = spawn_proxy(world, at, *pc);
            if (!a) continue;
            Proxy p;
            p.actor.set(a);
            p.kind = e.kind;
            p.hp = ue::obj(a, L"HPComponent");
            if (p.hp) refill(p.hp);
            freeze_proxy(p, a);
            log::info(L"proxy #%zu spawned (%s) for entity %u", slot, p.kind == EntityKind::Enemy ? L"enemy" : L"breakable", e.id);
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
            fit_hurtbox(p, a, e);
        }
        if (reassert) freeze_proxy(p, a); // EL activation paths may re-enable tick / AI / collision
        ue::set_actor_location(a, at);
    }
    for (std::size_t k = 0; k < g_proxies.size(); ++k) {
        if (!used[k] && g_proxies[k].active) park(g_proxies[k]);
    }
}

UObject* knockback_component(UObject* pawn) {
    // the property is "Knockback" on BP_Character_Base_C; ask by class so the name does not matter
    Call c(pawn, L"GetComponentByClass");
    return c.o(ue::find_class(L"/Script/Zenith.KnockbackComponent")).go() ? c.ro() : nullptr;
}

// EM's knockback, as EM would apply it to Lilac, replayed through EL's own LaunchKnockback. Lily's own record
// (AirborneLaunchKnockback) supplies what EM's data has no word for (states, movement curve, move time); EM supplies
// the launch velocity per movement state (EL's LaunchKnockbackFactor / MovementFactor undone, core/combat_map.h
// el_launch), launch vs stagger (a zero vector makes EL stagger) and the stagger time.
void replay_knockback(UObject* lily_pawn, const HurtPayload& hurt) {
    UObject* kb = knockback_component(lily_pawn);
    if (!kb) return;
    ue::FProperty* own = ue::prop(kb->GetClassPrivate(), L"AirborneLaunchKnockback");
    Call launch(kb, L"LaunchKnockback");
    ue::FProperty* data = launch.param(L"KnockbackData");
    if (!own || !data) return;
    launch.copy(reinterpret_cast<std::uint8_t*>(kb) + own->GetOffset_Internal());
    auto* d = static_cast<std::uint8_t*>(launch.slot(data));
    const auto member = [&](const wchar_t* n) -> std::pair<ue::FProperty*, void*> {
        ue::FProperty* m = ue::member_of(data, n);
        return {m, m ? d + m->GetOffset_Internal() : nullptr};
    };
    const auto set = [&](const wchar_t* n, double v) {
        if (auto [m, at] = member(n); m) ue::write_num(m, at, v);
    };
    const double movement = ue::num(kb, L"MovementFactor", 1.0);
    const V3 axis = ue::vec(kb, L"LaunchKnockbackFactor");
    const wchar_t* vectors[3] = {L"LaunchVectorGrounded", L"LaunchVectorAir", L"LaunchVectorSwim"};
    double dir = hurt.dir_y >= 0.0f ? 1.0 : -1.0;
    bool launches = false;
    for (int i = 0; i < 3; ++i) {
        const combat::Push q{hurt.push[i].y, hurt.push[i].z, (hurt.flags & (kHurtLaunchGround << i)) != 0};
        const combat::LocalLaunch l = combat::el_launch(q, movement, axis.x, axis.z);
        if (auto [m, at] = member(vectors[i]); m) ue::write_vec(at, m->GetElementSize(), {l.x, 0.0, l.z});
        if (q.launch && std::abs(q.y) > 1e-3) dir = q.y > 0.0 ? 1.0 : -1.0;
        launches |= q.launch;
    }
    set(L"bAbsoluteLaunchVector", 0.0);
    set(L"bForceRestartDamageAnimation", 1.0);
    // A launch stays a knockback until Lily lands, like EM's WaitForLanding phase. Ending it on a timer (EM's
    // auto recovery) left EL flying with launch velocity in its plain fall / jump animation.
    set(L"bKnockbackUntilGrounded", launches ? 1.0 : 0.0);
    if (hurt.stagger > 0.0f) set(L"StunTime", hurt.stagger);
    const bool ok = launch.v({0.0, dir, 0.0}).go();
    if (ok) ++counters().knockback_replayed;
    log::info(L"knockback: launch g/a/s=%d%d%d push g=(%.0f,%.0f) a=(%.0f,%.0f) stagger=%.2f auto=%d dir=%.0f factors m=%.2f (%.2f,%.2f) -> id %lld",
              (hurt.flags & kHurtLaunchGround) != 0, (hurt.flags & kHurtLaunchAir) != 0, (hurt.flags & kHurtLaunchSwim) != 0,
              hurt.push[0].y, hurt.push[0].z, hurt.push[1].y, hurt.push[1].z, hurt.stagger, (hurt.flags & kHurtAutoRecovery) != 0, dir,
              movement, axis.x, axis.z, ok ? static_cast<long long>(launch.ri()) : -1LL);
}

} // namespace

void combat_arm(const binary::Manifest& m) {
    hook::attach(m, "HPComponentDoDamage", reinterpret_cast<void*>(&on_do_damage), reinterpret_cast<void**>(&g_do_damage));
    hook::attach(m, "AEDamageApply", reinterpret_cast<void*>(&on_ae_damage), reinterpret_cast<void**>(&g_ae_damage));
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
    Call(lily_pawn, L"SetLastAttackerDirection").v({0.0, hurt.dir_y >= 0.0f ? 1.0 : -1.0, 0.0}).go();
    if (hurt.flags & kHurtDamage) {
        const auto dmg = combat::hurt_to_el(hurt.damage_em, static_cast<std::int32_t>(ue::num(hp, L"MaxHP")), hurt.max_hp_em);
        Call(hp, L"DoDamage").o(nullptr).i(dmg).b(false).b(false).go(); // native body -> our detour applies the lethal cap
        ++counters().hurt_replayed;
    }
    if (hurt.knockback) replay_knockback(lily_pawn, hurt); // a push without damage (EM knockback-only) still pushes
}

void combat_reset() {
    for (Proxy& p : g_proxies) park(p);
    g_lethal_pending = false;
}

} // namespace lie::guest
