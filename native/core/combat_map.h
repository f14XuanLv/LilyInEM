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

} // namespace lie::combat
