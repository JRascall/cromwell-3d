/* Palette.hpp — every colour the world is drawn in.
 *
 * SINGLE RESPONSIBILITY: name colours. Pulling them out of the mesh builders
 * means a re-skin touches this file and nothing else.
 */
#pragma once

#include "raylib.h"

#include "game/lattice/Constants.hpp"

#include <array>

namespace game::palette {

/* one floor tint per storey, so height reads at a glance */
inline const std::array<Color, kDefaultStoreyCount> kFloor = {
    Color{ 0x3b, 0x41, 0x4a, 255 },
    Color{ 0x45, 0x4c, 0x57, 255 },
    Color{ 0x4e, 0x56, 0x62, 255 },
};

inline constexpr Color kRoad   = { 0x30, 0x34, 0x3b, 255 };
inline constexpr Color kGrass  = { 0x4a, 0x5a, 0x44, 255 };
inline constexpr Color kWall   = { 0x88, 0x91, 0xa0, 255 };
inline constexpr Color kWindow = { 0x7f, 0xc4, 0xe8, 255 };
inline constexpr Color kHalf   = { 0x7a, 0x6f, 0x57, 255 };
inline constexpr Color kRamp   = { 0x5d, 0x65, 0x70, 255 };
inline constexpr Color kBlock  = { 0x26, 0x2b, 0x33, 255 };
inline constexpr Color kCanopy = { 0x77, 0x70, 0x5f, 255 };
inline constexpr Color kPortal = { 0xc0, 0x7d, 0xff, 255 };
inline constexpr Color kLadder = { 0xd7, 0xde, 0xe8, 255 };

/* units */
inline constexpr Color kEnemy         = { 0xff, 0x5f, 0x5f, 255 };
inline constexpr Color kPlayerSoldier = { 0x4d, 0xa3, 0xff, 255 };
inline constexpr Color kPlayerVehicle = { 0x5f, 0x7d, 0x52, 255 };
inline constexpr Color kVehicleTurret = { 0x71, 0x92, 0x6a, 255 };
inline constexpr Color kVehicleBarrel = { 0x4c, 0x5f, 0x45, 255 };

/* overlays */
inline constexpr Color kVisibleDirect = { 64, 224, 122, 70 };
inline constexpr Color kVisiblePeek   = { 55, 208, 196, 76 };
inline constexpr Color kVisibleNone   = { 179, 55, 74, 26 };
inline constexpr Color kCoverFull     = { 207, 232, 255, 240 };
inline constexpr Color kCoverHalf     = { 255, 217, 122, 240 };
inline constexpr Color kHoverValid    = { 255, 255, 255, 70 };
inline constexpr Color kHoverInvalid  = { 255, 90, 90, 60 };
inline constexpr Color kHoverGrenade  = { 255, 160, 80, 110 };
inline constexpr Color kBlastFlash    = { 255, 195, 107, 220 };

/* XCOM's authored ring colours, straight out of DefaultGameCore.ini:
 *   NonDashingBorderColor (0.177, 0.666, 0.666)
 *   DashingBorderColor    (1.0,   0.694, 0.198)  */
inline constexpr Color kRingMove   = { 45, 170, 170, 235 };
inline constexpr Color kRingSprint = { 255, 177, 50, 235 };

inline constexpr Color kBackground = { 0x17, 0x1a, 0x20, 255 };

}  // namespace game::palette
