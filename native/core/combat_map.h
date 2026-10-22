#pragma once

// Damage unit mapping between the two games (FR-CMB). Both operands always come from the running
// games, never from constants:
//   hurt (EM -> Lily): dmg_EL = round(dmg_EM * MaxHP_Lily / MaxHP_Lilac)   (same share of the bar)
//   hit  (Lily -> EM): dmg_EM = round(dmg_EL * Attack_Lilac / Attack_Lily) (same share of the attack stat)
// [CUE4P] both games use base HP 100 and Lv1 Attack 50, so at equal progression both ratios are ~1.

#include <cmath>
#include <cstdint>

namespace lie::combat {

inline std::int32_t ratio_map(std::int32_t value, double num, double den) {
    if (value <= 0) return 0;
    if (num <= 0.0 || den <= 0.0) return value; // missing reading: pass through unchanged
    const double mapped = std::round(static_cast<double>(value) * num / den);
    return mapped < 1.0 ? 1 : static_cast<std::int32_t>(mapped);
}

inline std::int32_t hurt_to_el(std::int32_t dmg_em, std::int32_t max_hp_lily, std::int32_t max_hp_lilac) {
    return ratio_map(dmg_em, max_hp_lily, max_hp_lilac);
}

inline std::int32_t hit_to_em(std::int32_t dmg_el, std::int32_t attack_lilac, std::int32_t attack_lily) {
    return ratio_map(dmg_el, attack_lilac, attack_lily);
}

// EL never runs its own death flow (EM owns death / respawn): a hit that would kill Lily is
// settled down to 1 HP and reported as lethal instead.
struct LethalSplit {
    std::int32_t apply = 0;
    bool lethal = false;
};
inline LethalSplit split_lethal(std::int32_t dmg_el, std::int32_t hp_now) {
    if (dmg_el < hp_now) return {dmg_el, false};
    return {hp_now > 1 ? hp_now - 1 : 0, true};
}

// Knockback "as Lilac would get it" (FR-CMB-2). EM UKnockbackComponent::StartKnockback [IDA 0x144756150]:
// the impulse variant for the movement state (local X = along the push, Z = up) is rotated by the push direction's
// orientation, world X is dropped (2D), and the result is scaled by Lilac's ImpulseForceFactor. It is a launch when
// |v| >= MinImpulseForLaunch [IDA 0x144737340]; walking additionally needs a vertical component and swimming never
// launches. Otherwise EM only staggers.
enum class Move { Ground, Air, Swim };
struct Push {
    double y = 0.0, z = 0.0;
    bool launch = false;
};
inline Push em_push(double lx, double ly, double lz, double dx, double dy, double dz, double factor, double min_launch, Move move) {
    const double h = std::sqrt(dx * dx + dy * dy);
    const double len = std::sqrt(h * h + dz * dz);
    double cy = 1.0, sy = 0.0, cp = 1.0, sp = 0.0; // FVector::ToOrientationQuat: yaw then pitch, no roll
    if (len > 1e-8) {
        if (h > 1e-8) {
            cy = dx / h;
            sy = dy / h;
        }
        cp = h / len;
        sp = dz / len;
    }
    Push p;
    p.y = (lx * cp * sy + ly * cy - lz * sp * sy) * factor; // forward.y, right.y, up.y
    p.z = (lx * sp + lz * cp) * factor;                     // forward.z, up.z
    const double m2 = p.y * p.y + p.z * p.z, t2 = min_launch * min_launch;
    switch (move) {
    case Move::Ground: p.launch = std::abs(p.z) > 0.0 && m2 >= t2; break;
    case Move::Air: p.launch = m2 >= t2; break;
    case Move::Swim: p.launch = false; break;
    }
    return p;
}

// EL UKnockbackComponent::LaunchKnockback [IDA 0x140C9EA90]: velocity = LaunchVector x LaunchKnockbackFactor (per axis)
// x MovementFactor, rotated onto the push direction (0, sign, 0). Returns the local LaunchVector (X along the push, Z up)
// that reproduces EM's world velocity on Lily; zero = EL staggers instead of launching.
struct LocalLaunch {
    double x = 0.0, z = 0.0;
};
inline LocalLaunch el_launch(const Push& p, double movement_factor, double fx, double fz) {
    if (!p.launch) return {};
    const auto inv = [](double v, double f) { return std::abs(f) > 1e-6 ? v / f : v; }; // missing reading: pass through
    const double m = std::abs(movement_factor) > 1e-6 ? movement_factor : 1.0;
    return {inv(std::abs(p.y), fx) / m, inv(p.z, fz) / m};
}

} // namespace lie::combat
