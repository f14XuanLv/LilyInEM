#pragma once

// Engine / asset facts and the few product policies the mod relies on (FR-ENG-6), with provenance:
//   [SDK]    reconstructed class headers (D:\CodeT\EnderLilies, D:\CodeT\EnderMagnolia)
//   [UE]     Unreal Engine 4.26.2 / 5.4.4 source
//   [CUE4P]  asset data read with CUE4Parse
//   [IDA]    behaviour verified in the Shipping executables
//   [POLICY] product decision, not an engine fact
// Binary-version data (SHA-256, RVAs, prefixes, raw struct offsets) is NOT here: it lives in
// data/binary/{EL,EM}.json and is loaded at runtime (see core/binary_manifest.h).
// Everything else must be read from the running game or derived by a formula.

#include <cstdint>

namespace lie::facts {

// ---------------- Ender Lilies (Guest, UE 4.26.2) ----------------
namespace el {
inline constexpr const wchar_t* kExe = L"EnderLiliesSteam-Win64-Shipping.exe";

// [UE] EComponentMobility. [IDA] UAutoClimbComponent / UWallGrabComponent tick traces use
// FCollisionQueryParams::MobilityType = Static, so mirrored terrain must be Static to be climbable/grabbable.
inline constexpr int kMobilityStatic = 0;
inline constexpr int kMobilityMovable = 2;
// [UE] ECollisionEnabled::QueryAndPhysics
inline constexpr int kCollisionQueryAndPhysics = 3;
// [UE] ECollisionChannel::ECC_WorldStatic
inline constexpr int kChannelWorldStatic = 0;
// [CUE4P] UCA_FallThrough_C requires the floor's ObjectType == ECC_GameTraceChannel4 (17)
inline constexpr int kChannelOneWay = 17;

// [CUE4P] native blueprint classes / tables reused as-is
inline constexpr const wchar_t* kOneWayPlatformClass = L"/Game/_Zenith/Gameplay/LevelBlocks/BP_OneWayPlatform.BP_OneWayPlatform_C";
inline constexpr const wchar_t* kSpiritTable = L"/Game/_Zenith/Gameplay/Data/DT_Items_Spirits.DT_Items_Spirits";
inline constexpr const wchar_t* kPassiveTable = L"/Game/_Zenith/Gameplay/Data/DT_Items_Passives.DT_Items_Passives";

// [SDK] enums
inline constexpr int kSpineLayerEvent = 9;          // ESpineAnimationLayer::EVENT
inline constexpr int kSoundBgm = 1;                 // ESoundFamily::BGM
inline constexpr int kSoundAmbience = 3;            // ESoundFamily::Ambience
inline constexpr int kAptitudeSpecialAttack = 120;  // EAptitudeType::SpecialAttack
inline constexpr int kMoveWalking = 1;              // [UE] EMovementMode::MOVE_Walking
inline constexpr int kMoveFalling = 3;              // [UE] EMovementMode::MOVE_Falling
} // namespace el

// ---------------- Ender Magnolia (Host, UE 5.4.4) ----------------
namespace em {
inline constexpr const wchar_t* kExe = L"EnderMagnoliaSteam-Win64-Shipping.exe";
inline constexpr const wchar_t* kLilacClass = L"BP_p0000_Lilac_C";
// [CUE4P] parent M_Spine_Character: Unlit / Translucent / TwoSided, texture parameter SpriteTexture
inline constexpr const wchar_t* kSpineMaterial = L"/Game/_Zion/Materials/Common/MI_Spine_Character.MI_Spine_Character";
// [SDK] EFaction (Zion)
inline constexpr int kFactionEnemy = 3;
inline constexpr int kFactionBreakableBlock = 5;
} // namespace em

// ---------------- Cross-process presentation ----------------
// [POLICY] Unique render-target size so the CreateTexture2D hook identifies our world-layer RT unambiguously.
inline constexpr std::uint32_t kWorldRtWidth = 1282;
inline constexpr std::uint32_t kWorldRtHeight = 722;

// [POLICY] A peer whose heartbeat is older than this is treated as gone (auto-fallback to Lilac).
inline constexpr std::uint64_t kHeartbeatTimeoutMs = 2000;

// [POLICY] Terrain scanner column pitch (UU): narrower than the smallest EL capsule diameter
// (2 x 15 UU, read at runtime and asserted), so no gap the capsule could fall through is missed.
inline constexpr double kTerrainColumnPitch = 16.0;

} // namespace lie::facts
