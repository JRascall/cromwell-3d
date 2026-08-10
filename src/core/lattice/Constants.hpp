/* Constants.hpp — the physical constants of the tile lattice.
 *
 * SINGLE RESPONSIBILITY: state XCOM 2's recovered numbers. Nothing here
 * computes, stores or queries; it only names magnitudes.
 *
 * THE VERTICAL LATTICE — XCOM 2's actual numbers, recovered from the SDK's
 * XComWorldData.uc. XCOM quantises the world into 96 x 96 x 64 unreal-unit
 * tiles:
 *     WORLD_StepSize             96   tile X/Y          -> kTileSize = 1
 *     WORLD_FloorHeight          64   ONE z index       -> kCellHeight = 2/3
 *     WORLD_FloorHeightsPerLevel  3   z indices/storey  -> kCellsPerStorey
 *     WORLD_TotalLevels           3   storeys per map   -> Lattice's default
 * So a z index is a THIRD of a storey, not a storey. A building floor spans
 * three z cells; a full-height wall is three stacked edge records. This is
 * why XCOM's cover constants land on clean multiples: low cover 64 = exactly
 * one cell, high cover 96 = 1.5 cells, climb-over clearance 160 = 2.5 cells.
 *
 * Sub-cell detail still exists — XCOM's own WORLD_BaseHeight is 24uu, not a
 * multiple of 64, and its floor position per tile is a float. That is what
 * Tile::floorOffset is for: micro-relief (roads, curbs, lawns) WITHIN a cell
 * that movement ignores. Anything taller than a step deserves a real ramp.
 */
#pragma once

namespace xcom {

/* ------------------------------------------------------------- lattice */
inline constexpr float kTileSize        = 1.0f;                  /* 96uu, ~1.5m */
inline constexpr int   kCellsPerStorey  = 3;                     /* WORLD_FloorHeightsPerLevel */
inline constexpr float kCellHeight      = kTileSize * 2.0f / 3.0f;   /* 64uu    */
inline constexpr float kStoreyHeight    = kCellHeight * kCellsPerStorey; /* 192uu = 2 */

/* Default map extents. Runtime values live on Lattice — these are only the
 * dimensions the demo map and the prototype's tests are authored against. */
inline constexpr int kDefaultGridWidth   = 24;
inline constexpr int kDefaultGridHeight  = 24;
inline constexpr int kDefaultStoreyCount = 3;                    /* WORLD_TotalLevels */

/* ------------------------------------------------------------- movement */
inline constexpr float kDiagonalCost = 1.4f;
/* XCOM-style free verticality: hopping cover, dropping and climbing ladders
 * cost no EXTRA movement, and stair tiles are free to traverse.            */
inline constexpr float kClimbCost  = 0.0f;
inline constexpr float kDropCost   = 0.0f;   /* per STOREY fallen, not per cell */
inline constexpr float kLadderCost = 0.0f;   /* per STOREY climbed              */
inline constexpr float kPortalCost = 1.0f;

/* MOVEMENT DELTAS & DERIVED LEDGE COVER — absolute physical heights, because
 * they describe what a body can step or climb, not what a storey measures.
 * XCOM equivalents at 96uu per tile:
 *     WORLD_BaseHeight       24uu = 0.25  (std. curb)  ~ kWalkStep
 *     Cover_LowCoverHeight   64uu = 0.667 (low cover)  ~ kLedgeHalf
 *     Cover_HighCoverHeight  96uu = 1.0   (high cover) ~ kLedgeFull      */
inline constexpr float kWalkStep  = 0.30f;
inline constexpr float kMantleMax = 1.10f;
inline constexpr float kLedgeHalf = 0.35f;
inline constexpr float kLedgeFull = 1.10f;

/* ------------------------------------------------------------------ LOS */
/* kEyeHeight matches XCOM's Cover_PeekTestHeight (96uu) and equals its high
 * cover height — which is what makes "you cannot see over high cover" true
 * by construction rather than by tuning.                                   */
inline constexpr float kEyeHeight      = 1.00f;
inline constexpr float kVehicleEyeHeight = 0.95f;
inline constexpr float kVehicleLosHeight = 0.85f;
/* Window glass band, as a fraction of the STOREY (a window belongs to a
 * building floor, and a full wall spans three cells' worth of edges).      */
inline constexpr float kWindowSill = 0.30f;
inline constexpr float kWindowHead = 0.92f;

/* ----------------------------------------------------------------- ramps */
/* XCOM classifies a floor tile as a ramp purely by its surface NORMAL:
 * WORLD_RampDotMinThreshold 0.7 (~45 deg) to WORLD_RampDotMaxThreshold
 * 0.9848 (~10 deg). Steeper than 45 is not a ramp there at all — it becomes
 * a ClimbOver / ClimbOnto / Ladder traversal. Shallower than 10 is not a
 * staircase but micro-relief: use floorOffset.
 *
 * Ramps are authored by ABSOLUTE rise: a flight climbs `rise` world units
 * across one tile. At the 45 deg cap that is 1.0 = 1.5 z cells, so a ramp
 * legitimately overspills the cell that owns it — just as in XCOM, where a
 * 96uu rise lives in a 64uu cell. A full storey always needs a 2-tile run.
 *
 * kRampArtSteps is ART ONLY. The tile DATA models a ramp as one flat inclined
 * plane; the visible treads are decoration the grid knows nothing about. */
inline constexpr float kRampMaxRise = kTileSize * 1.0f;       /* tan(45 deg) */
inline constexpr float kRampMinRise = kTileSize * 0.17633f;   /* tan(10 deg) */
inline constexpr int   kRampArtSteps = 6;

}  // namespace xcom
