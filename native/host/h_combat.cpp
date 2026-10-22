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
};

// FKnockbackData members [reflection, UE5: FVector = 3 doubles]
struct KnockLayout {
    ue::FProperty* impulse[3]{}; // KnockState order
    ue::FProperty* stagger = nullptr;
    ue::FProperty* auto_recovery = nullptr;
    bool ready = false;
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
    KnockLayout kb;
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

void resolve_knockback_layout() {
    if (g.kb.ready) return;
    auto* st = static_cast<ue::UStruct*>(ue::find(L"/Script/Zion.KnockbackData"));
    if (!st) return;
    const wchar_t* names[3] = {L"LocalSpaceImpulse_Grounded", L"LocalSpaceImpulse_Airborne", L"LocalSpaceImpulse_Swim"};
    for (int i = 0; i < 3; ++i) g.kb.impulse[i] = ue::prop(st, names[i]);
    g.kb.stagger = ue::prop(st, L"StaggerTime");
    g.kb.auto_recovery = ue::prop(st, L"bLaunchAutoRecovery");
    g.kb.ready = true;
}

bool intercepting() { return bridged() && !g.lethal_window; }

// EM settles one hit as DoDamage and StartKnockback back to back, in either order: both halves go into one Hurt so
// EL applies them together (a lone knockback first would start Lily's hit invincibility and swallow the damage).
Pending& pending_for(bool damage) {
    if (!g.pending.empty()) {
        Pending& last = g.pending.back();
        if (damage ? !(last.hurt.flags & kHurtDamage) : !last.hurt.knockback) return last;
    }
    return g.pending.emplace_back();
}

bool __fastcall on_do_damage(UObject* hp, UObject* source, const void* data) {
    if (hp && hp == g.lilac_hp.get() && intercepting()) {
        resolve_damage_layout();
        HurtPayload& h = pending_for(true).hurt;
        if (data && g.dd_damage >= 0) {
            const auto* b = static_cast<const std::uint8_t*>(data);
            h.damage_em = *reinterpret_cast<const std::int32_t*>(b + g.dd_damage);
            if (g.dd_element >= 0) h.element = b[g.dd_element];
            if (g.dd_type >= 0) h.type = b[g.dd_type];
        }
        h.max_hp_em = static_cast<std::int32_t>(ue::call_num(hp, L"GetMaxValue"));
        h.source_id = entity_id_of(source);
        if (!h.knockback) { // the knockback direction, when there is one, is EM's own
            const V3 sl = ue::actor_location(source), ll = ue::actor_location(lilac());
            h.dir_y = ll.y >= sl.y ? 1.0f : -1.0f; // pushed away from the source
        }
        h.flags |= kHurtDamage;
        ++counters().dmg_intercepted;
        return false; // EL settles it
    }
    return g_do_damage(hp, source, data);
}

// EM already decided that Lilac is knocked back (attack data, poise conditions). Record what EM would do to her:
// the world velocity per movement state and whether it is a launch or a stagger (core/combat_map.h em_push).
void __fastcall on_start_knockback(UObject* kb, const void* data, const void* dir) {
    if (kb && kb == g.lilac_kb.get() && intercepting()) {
        resolve_knockback_layout();
        HurtPayload& h = pending_for(false).hurt;
        h.knockback = 1;
        const V3 d = dir ? ue::read_vec(dir, 24) : V3{}; // UE5 FVector = 3 doubles
        const double factor = ue::num(kb, L"ImpulseForceFactor", 1.0), min_launch = ue::num(kb, L"MinImpulseForLaunch");
        const auto* b = static_cast<const std::uint8_t*>(data);
        for (int i = 0; b && i < 3; ++i) {
            ue::FProperty* p = g.kb.impulse[i];
            if (!p) continue;
            const V3 l = ue::read_vec(b + p->GetOffset_Internal(), p->GetElementSize());
            const combat::Push q = combat::em_push(l.x, l.y, l.z, d.x, d.y, d.z, factor, min_launch, static_cast<combat::Move>(i));
            h.push[i] = {static_cast<float>(q.y), static_cast<float>(q.z)};
            if (q.launch) h.flags |= static_cast<std::uint8_t>(kHurtLaunchGround << i);
        }
        if (b && g.kb.stagger) h.stagger = static_cast<float>(ue::read_num(g.kb.stagger, b + g.kb.stagger->GetOffset_Internal()));
        if (b && g.kb.auto_recovery && ue::read_num(g.kb.auto_recovery, b + g.kb.auto_recovery->GetOffset_Internal()) != 0.0) h.flags |= kHurtAutoRecovery;
        if (std::abs(d.y) > 1e-3) h.dir_y = d.y > 0.0 ? 1.0f : -1.0f;
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
        kind = EntityKind::Enemy; // faction is checked when publishing: at BeginPlay it is not assigned yet

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
        if (t.kind == EntityKind::Enemy) {
            UObject* fc = ue::obj(a, L"FactionComponent");
            if (!fc || static_cast<int>(ue::num(fc, L"Faction")) != facts::em::kFactionEnemy) continue;
        }
        V3 p = ue::actor_location(a);
        UObject* cap = ue::obj(a, L"CapsuleComponent");
        V3 half{0.0, cap ? ue::num(cap, L"CapsuleRadius", 40.0) : 60.0, cap ? ue::num(cap, L"CapsuleHalfHeight", 60.0) : 60.0};
        if (t.kind == EntityKind::Breakable) { // no capsule: the colliding components (its hurtbox) give centre and size
            Call bounds(a, L"GetActorBounds");
            ue::FProperty* origin = bounds.param(L"Origin");
            ue::FProperty* extent = bounds.param(L"BoxExtent");
            if (origin && extent && bounds.b(true).skip().skip().b(false).go()) {
                const V3 o = ue::read_vec(bounds.slot(origin), origin->GetElementSize());
                const V3 x = ue::read_vec(bounds.slot(extent), extent->GetElementSize());
                if (x.y > 1.0 && x.z > 1.0) {
                    p = o;
                    half = x;
                }
            }
        }
        if (std::abs(p.y - l.y) > kEntityRadius || std::abs(p.z - l.z) > kEntityRadius) continue;
        if (s.count >= kMaxEntities) break;
        Entity& e = s.items[s.count++];
        e.id = t.id;
        e.kind = t.kind;
        e.y = static_cast<float>(p.y);
        e.z = static_cast<float>(p.z);
        e.hy = static_cast<float>(half.y);
        e.hz = static_cast<float>(half.z);
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
    // Breakables need nothing more: DoDamage broadcasts the HP delegate bound to ABreakable::OnBreakableReceivedDamage
    // and HP reaching zero fires UDeathComponent::OnHPReachedZero [IDA 0x1446C0330, 0x1447024A0] -> break + drops.
    if (UObject* stop = ue::obj(target, L"HitStopComponent")) Call(stop, L"LaunchHitStop").f(kEnemyHitStop).go();
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
