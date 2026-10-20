#pragma once

// The single registry of EM collision mechanisms (FR-MOV-4). Everything the mod knows about "how does this EM
// thing block Lilac, and what stands in for it in EL" lives here; Host classification and Guest mirroring read
// only this table. Engine-independent so it is unit-tested offline.
//
// Geometry ("where is it solid") is NOT classified: any component that mutually BLOCKs Lilac's capsule is
// terrain, its shape comes from the scan and is verified per component (volume vs open shell). Only behaviour
// that a static snapshot cannot express needs a rule: pass-through from below, movement, breaking.
// Not terrain at all (no Block against Lilac's "Player" profile): water volumes, hook points, triggers,
// design / fog volumes; those travel on the entity channel when they matter.

#include "core/protocol.h"

#include <cstdint>
#include <string_view>

namespace lie::terrain {

enum class Mechanism : std::uint8_t {
    Static,        // plain blocking geometry (the default for anything without a rule)
    OneWay,        // [SDK] AOneWayPlatform: BlockerComponent stays Block, pass-through is per actor
    MovingOneWay,  // [SDK] AMovingPlatform_Elevator / _Spline derive from AOneWayPlatform and move
    MovingBlock,   // [SDK] AMovingBlock_Elevator
    Breakable,     // [SDK] ABreakable: blocks until EM disables its collision
};

// How the Guest stands it in
enum class Mirror : std::uint8_t {
    Box,     // hidden Static TriggerBox (BlockAll, WorldStatic)
    OneWay,  // EL's own BP_OneWayPlatform_C: jump-through and UCA_FallThrough are EL's native rules
};

struct MechanismRule {
    Mechanism mechanism;
    const wchar_t* em_class; // native class path; a component's owner IsA this class
    std::uint32_t flags;     // TerrainFlags published to the Guest
    const char* note;        // current coverage, shown in status / docs
};

// Ordered most specific first: the first rule whose class the owner IsA wins.
inline constexpr MechanismRule kMechanisms[] = {
    {Mechanism::MovingOneWay, L"/Script/Zion.MovingPlatform_Elevator", kTerrainSolid | kTerrainOneWay,
     "EL one-way, re-placed on rescan; does not carry Lily yet (S4 kinematic)"},
    {Mechanism::MovingOneWay, L"/Script/Zion.MovingPlatform_Spline", kTerrainSolid | kTerrainOneWay,
     "EL one-way, re-placed on rescan; does not carry Lily yet (S4 kinematic)"},
    {Mechanism::OneWay, L"/Script/Zion.OneWayPlatform", kTerrainSolid | kTerrainOneWay, "EL BP_OneWayPlatform_C"},
    {Mechanism::MovingBlock, L"/Script/Zion.MovingBlock_Elevator", kTerrainSolid,
     "box, re-placed on rescan; does not carry Lily yet (S4 kinematic)"},
    {Mechanism::Breakable, L"/Script/Zion.Breakable", kTerrainSolid, "box until EM disables its collision (rescan)"},
};

inline constexpr std::uint32_t kStaticFlags = kTerrainSolid;

inline constexpr Mirror mirror_of(std::uint32_t flags) { return (flags & kTerrainOneWay) ? Mirror::OneWay : Mirror::Box; }

inline constexpr std::string_view mechanism_name(Mechanism m) {
    switch (m) {
    case Mechanism::Static: return "static";
    case Mechanism::OneWay: return "one-way";
    case Mechanism::MovingOneWay: return "moving-one-way";
    case Mechanism::MovingBlock: return "moving-block";
    case Mechanism::Breakable: return "breakable";
    }
    return "?";
}

} // namespace lie::terrain
