// Host combat (FR-CMB-1/2/3, FR-ENG-7).
//
// Hits ON Lilac: EM still decides *whether* Lilac was hit (its hitboxes, contact damage, hazards hit the
// puppet's hurtbox natively). The settlement is intercepted with native detours ([IDA] these are direct C++
// calls, invisible to ProcessEvent hooks): UStatHPComponent::DoDamage and UKnockbackComponent::StartKnockback
// on Lilac return without effect and become one Hurt event per hit, settled by EL. Lilac's HP stays full.
//
// Hits BY Lily: HitEnemy events from EL are settled through EM's own UStatHPComponent::DoDamage on the enemy
// (flash, floating number, death, drops, EXP), plus the shake / hit-stop reactions that EM keeps outside
// DoDamage ([IDA] they belong to AEP_Player_Attacks_C effects).
//
// Lethal: EL reports it; Lilac's HP is driven to 0 inside an explicit window so EM runs its own death and
// respawn flow.

#include "host/host.h"

#include "core/combat_map.h"
#include "core/facts.h"
#include "rt/clock.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lie::host {

namespace {

using ue::Call;
using ue::V3;

// [POLICY] EM enemies within this distance of Lilac (UU, about two screens) get an EL proxy
constexpr double kEntityRadius = 3000.0;
// [POLICY] hit-stop applied to an enemy hit by Lily, matching EM's light-attack feel until the AEP route lands
constexpr double kEnemyHitStop = 0.05;

struct Pending {
    HurtPayload hurt;
    bool has_damage = false;
};

struct Tracked {
    ue::Weak actor;
    std::uint32_t id;
    EntityKind kind;
};

struct State {
    ue::Weak lilac_hp, lilac_kb;
    bool lethal_window = false;
    bool dying = false;
    std::vector<Pending> pending;
    std::vector<Tracked> tracked;
    std::uint32_t next_id = 1;
    std::uint32_t entity_revision = 0;
    std::unique_ptr<EntitySnapshot> snap = std::make_unique<EntitySnapshot>();
    clock::Every pin{1.0}, census{2.0};
    int dd_damage = -1, dd_element = -1, dd_type = -1, dd_rate = -1, dd_size = 0;
    bool census_done = false;
} g;

using DoDamageFn = bool(__fastcall*)(UObject* hp, UObject* source, const void* damage_data);
using StartKnockbackFn = void(__fastcall*)(UObject* kb, const void* data, const void* dir);
DoDamageFn g_do_damage = nullptr;
StartKnockbackFn g_start_knockback = nullptr;

void resolve_damage_layout() {
    if (g.dd_size) return;
    auto* st = static_cast<ue::UStruct*>(ue::find(L"/Script/Zion.DamageData"));
    if (!st) return;
    auto off = [&](const wchar_t* n) {
        ue::FProperty* p = ue::prop(st, n);
        return p ? p->GetOffset_Internal() : -1;
    };
    g.dd_damage = off(L"Damage");
    g.dd_element = off(L"Element");
    g.dd_type = off(L"Type");
    g.dd_rate = off(L"AppliedRate");
    g.dd_size = st->GetPropertiesSize();
}

bool intercepting() { return bridged() && !g.lethal_window; }

bool __fastcall on_do_damage(UObject* hp, UObject* source, const void* data) {
    if (hp && hp == g.lilac_hp.get() && intercepting()) {
        resolve_damage_layout();
        Pending p;
        if (data && g.dd_damage >= 0) {
            const auto* b = static_cast<const std::uint8_t*>(data);
            p.hurt.damage_em = *reinterpret_cast<const std::int32_t*>(b + g.dd_damage);
            if (g.dd_element >= 0) p.hurt.element = b[g.dd_element];
            if (g.dd_type >= 0) p.hurt.type = b[g.dd_type];
        }
        p.hurt.max_hp_em = static_cast<std::int32_t>(ue::call_num(hp, L"GetMaxValue"));
        p.hurt.source_id = entity_id_of(source);
        const V3 sl = ue::actor_location(source), ll = ue::actor_location(lilac());
        p.hurt.dir_y = ll.y >= sl.y ? 1.0f : -1.0f; // pushed away from the source
        p.has_damage = true;
        g.pending.push_back(p);
        ++counters().dmg_intercepted;
        return false; // EL settles it
    }
    return g_do_damage(hp, source, data);
}

void __fastcall on_start_knockback(UObject* kb, const void* data, const void* dir) {
    if (kb && kb == g.lilac_kb.get() && intercepting()) {
        const V3 d = dir ? ue::read_vec(dir, 24) : V3{}; // UE5 FVector = 3 doubles
        if (g.pending.empty() || g.pending.back().hurt.knockback) g.pending.push_back({});
        Pending& p = g.pending.back();
        p.hurt.knockback = 1;
        if (std::abs(d.y) > 1e-3) p.hurt.dir_y = d.y > 0.0 ? 1.0f : -1.0f;
        ++counters().kb_intercepted;
        return;
    }
    g_start_knockback(kb, data, dir);
}

UObject* stats_of(UObject* pawn) {
    Call c(pawn, L"GetComponentByClass");
    return c.o(ue::find_class(L"/Script/Zion.StatsComponent")).go() ? c.ro() : nullptr;
}

// ---- entity registry (enemies / breakables near Lilac) ----

void track(UObject* actor) {
    if (!ue::alive(actor) || actor == lilac()) return;
    EntityKind kind;
    if (ue::is_a(actor, ue::find_class(L"/Script/Zion.Breakable"))) {
        kind = EntityKind::Breakable;
    } else if (ue::is_a(actor, ue::find_class(L"/Script/Zion.CharacterZion"))) {
        UObject* fc = ue::obj(actor, L"FactionComponent");
        if (!fc || static_cast<int>(ue::num(fc, L"Faction")) != facts::em::kFactionEnemy) return;
        kind = EntityKind::Enemy;
    } else {
        return;
    }
    for (const Tracked& t : g.tracked) {
        if (t.actor.get() == actor) return;
    }
    g.tracked.push_back({ue::Weak(actor), g.next_id++, kind});
    ++g.entity_revision;
}

void census() {
    std::vector<UObject*> found;
    for (const wchar_t* cls : {STR("CharacterZion"), STR("Breakable")}) {
        found.clear();
        RC::Unreal::UObjectGlobals::FindAllOf(cls, found); // on activation / rebase only; BeginPlay keeps it current
        for (UObject* a : found) track(a);
    }
}

void publish_entities(UObject* pawn) {
    const V3 l = ue::actor_location(pawn);
    std::erase_if(g.tracked, [](const Tracked& t) { return !t.actor.get(); });
    EntitySnapshot& s = *g.snap;
    s.count = 0;
    for (const Tracked& t : g.tracked) {
        UObject* a = t.actor.get();
        UObject* hp = ue::obj(a, L"StatHPComponent");
        if (!hp || ue::call_num(hp, L"GetCurrValue") <= 0.0) continue;
        const V3 p = ue::actor_location(a);
        if (std::abs(p.y - l.y) > kEntityRadius || std::abs(p.z - l.z) > kEntityRadius) continue;
        if (s.count >= kMaxEntities) break;
        Entity& e = s.items[s.count++];
        e.id = t.id;
        e.kind = t.kind;
        UObject* cap = ue::obj(a, L"CapsuleComponent");
        e.y = static_cast<float>(p.y);
        e.z = static_cast<float>(p.z);
        e.hy = static_cast<float>(cap ? ue::num(cap, L"CapsuleRadius", 40.0) : 60.0);
        e.hz = static_cast<float>(cap ? ue::num(cap, L"CapsuleHalfHeight", 60.0) : 60.0);
    }
    s.revision = g.entity_revision;
    if (World* w = shm::world()) w->entities.store(s);
}

UObject* tracked_actor(std::uint32_t id) {
    for (const Tracked& t : g.tracked) {
        if (t.id == id) return t.actor.get();
    }
    return nullptr;
}

void apply_hit(UObject* pawn, const HitEnemyPayload& h) {
    UObject* target = tracked_actor(h.entity_id);
    UObject* hp = ue::obj(target, L"StatHPComponent");
    if (!hp || ue::call_num(hp, L"GetCurrValue") <= 0.0) return;
    resolve_damage_layout();
    UObject* stats = stats_of(pawn);
    const auto attack_em = static_cast<std::int32_t>(ue::call_num(stats, L"GetAttack"));
    const std::int32_t dmg = combat::hit_to_em(h.damage_el, attack_em, h.attack_el);
    Call dd(hp, L"DoDamage");
    ue::FProperty* data = dd.param(L"DamageData");
    if (!dd || !data || g.dd_damage < 0) return;
    auto* b = static_cast<std::uint8_t*>(dd.slot(data));
    *reinterpret_cast<std::int32_t*>(b + g.dd_damage) = dmg;
    if (g.dd_rate >= 0) *reinterpret_cast<float*>(b + g.dd_rate) = 1.0f;
    dd.set_o(L"Source", pawn).go();
    ++counters().enemy_damage_calls;
    if (UObject* stop = ue::obj(target, L"HitStopComponent")) Call(stop, L"LaunchHitStop").f(kEnemyHitStop).go();
    if (ue::class_contains(target, L"Breakable")) Call(target, L"OnBreakableReceivedDamage").o(pawn).i(dmg).go();
}

void kill_lilac(UObject* pawn) {
    UObject* hp = ue::obj(pawn, L"StatHPComponent");
    if (!hp || g.dying) return;
    g.lethal_window = true;
    const auto cur = static_cast<std::int64_t>(ue::call_num(hp, L"GetCurrValue"));
    if (cur > 0) Call(hp, L"Subtract").i(cur).go();
    g.lethal_window = false;
    g.dying = true;
    log::info(L"Lily lethal -> Lilac death (EM respawn flow)");
}

} // namespace

std::uint32_t entity_id_of(UObject* actor) {
    UObject* cur = actor;
    for (int depth = 0; depth < 4 && cur; ++depth) {
        for (const Tracked& t : g.tracked) {
            if (t.actor.get() == cur) return t.id;
        }
        cur = ue::obj(cur, L"Owner");
    }
    return 0;
}

void combat_arm(const binary::Manifest& m) {
    hook::attach(m, "StatHPComponentDoDamage", reinterpret_cast<void*>(&on_do_damage), reinterpret_cast<void**>(&g_do_damage));
    hook::attach(m, "KnockbackComponentStartKnockback", reinterpret_cast<void*>(&on_start_knockback), reinterpret_cast<void**>(&g_start_knockback));
}

void combat_tick(UObject* pc, UObject* pawn, bool is_bridged) {
    (void)pc;
    g.lilac_hp.set(ue::obj(pawn, L"StatHPComponent"));
    g.lilac_kb.set(ue::obj(pawn, L"KnockbackComponent"));
    for (Pending& p : g.pending) {
        send_to_guest(Event::make(EventType::Hurt, p.hurt, clock::now_s()));
    }
    g.pending.clear();
    if (!is_bridged) {
        g.census_done = false;
        return;
    }
    if (!g.census_done) {
        census();
        g.census_done = true;
    }
    publish_entities(pawn);
    // Lilac's HP is not a gameplay quantity while bridged: keep it full (counted, should stay 0)
    UObject* hp = g.lilac_hp.get();
    if (hp && g.dying && ue::call_num(hp, L"GetCurrValue") > 0.0) g.dying = false; // EM restored / respawned
    if (hp && !g.dying && g.pin.due(clock::now_s())) {
        const double cur = ue::call_num(hp, L"GetCurrValue"), max = ue::call_num(hp, L"GetMaxValue");
        if (cur < max) {
            Call(hp, L"Add").i(static_cast<std::int64_t>(max - cur)).go();
            ++counters().lilac_hp_corrections;
        }
    }
}

void combat_on_begin_play(UObject* actor) {
    if (g.census_done) track(actor);
}

void combat_on_event(UObject* pc, UObject* pawn, const Event& e) {
    (void)pc;
    switch (e.type) {
    case EventType::HitEnemy:
        apply_hit(pawn, e.as<HitEnemyPayload>());
        ++counters().hit_events_applied;
        break;
    case EventType::Lethal: kill_lilac(pawn); break;
    default: break;
    }
}

void combat_reset() {
    g.pending.clear();
    g.tracked.clear();
    g.census_done = false;
    g.dying = false;
    ++g.entity_revision;
}

} // namespace lie::host
