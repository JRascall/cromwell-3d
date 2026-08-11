/* DrawLayers.hpp — THIS GAME'S categories of drawable thing.
 *
 * SINGLE RESPONSIBILITY: declare what this project divides its rendering into,
 * name those categories, and offer the presets that follow from knowing what
 * they mean.
 *
 * ===================== THE FILE A NEW PROJECT REPLACES =====================
 *
 * NOTHING BELOW IS ENGINE. `units`, `props`, `movementRings` — these are what
 * an XCOM-shaped tactics game happens to draw, and a shooter or a puzzle game
 * would delete the lot and write its own. That is the point: cromwell owns
 * thirty-two anonymous slots and the machinery for masking them
 * (cromwell/math/Mask.hpp), and a project says what they are.
 *
 * These used to be fields on the engine's ViewLayers, which quietly asserted
 * that every game embedding cromwell has units and movement rings. It does not.
 * The collision system already had this right — game/picking/WorldTrace.hpp
 * declares kFloor, kWall, kUnit and the engine declares none — and this is the
 * same arrangement for rendering.
 *
 * ====================== WHY THE NAMES ARE REGISTERED =======================
 *
 * So the dev panel can ENUMERATE them. A checkbox per category, built from the
 * registry, means a project that adds a layer gets a switch for it without
 * anybody editing a panel — and a project that deletes one does not leave a
 * dead checkbox behind. `defaultDrawLayerNames()` is the one place the list
 * exists; the ids below and the names there are read together.
 *
 * ADDING A LAYER is three lines: an id here, a name there, and the `drawing()`
 * test at the pass that should honour it.
 */
#pragma once

#include "cromwell/overlay/ViewLayers.hpp"

namespace game {

/* THIS GAME'S DRAW LAYERS. Contiguous from zero, because the registry walks up
 * to the highest declared index and a gap would be a nameless checkbox. */
namespace drawLayer {

using cromwell::DrawLayerId;

/* the world's geometry */
inline constexpr DrawLayerId kStatics{ 0 };   /* the lattice: floors, walls, ramps */
inline constexpr DrawLayerId kProps{ 1 };
inline constexpr DrawLayerId kUnits{ 2 };

/* WHAT THE GAME PAINTS OVER THE WORLD, in 3D but not part of it. A camera that
 * is not the player's should think hard before showing any of these: a security
 * feed displaying the movement rings of the unit YOU have selected is showing
 * your cursor, from across the map, to nobody. */
inline constexpr DrawLayerId kOverlays{ 3 };       /* LOS tint, cover, hover, blasts */
inline constexpr DrawLayerId kMovementRings{ 4 };  /* the reach ribbons              */
inline constexpr DrawLayerId kRingGlow{ 5 };       /* their bloom, a separate pass   */

/* Every category the game paints over the world rather than in it — the group
 * that comes off for any camera that is not the player's. */
inline constexpr cromwell::DrawLayerMask kAnnotations =
    cromwell::DrawLayerMask::of(kOverlays) | cromwell::DrawLayerMask::of(kMovementRings)
    | cromwell::DrawLayerMask::of(kRingGlow);

}  // namespace drawLayer

/* The names, for the dev panel and any diagnostic. Built once and held. */
cromwell::DrawLayerNames defaultDrawLayerNames();

/* ---- presets ------------------------------------------------------------
 *
 * HERE RATHER THAN ON ViewLayers, because every one of them needs to know what
 * this game's layers MEAN. "The world without the interface" is not a sentence
 * the engine can complete.
 *
 * WHY PRESETS AT ALL. Configuring a camera otherwise means a dozen field pokes
 * at the call site, and the next camera means a dozen more — subtly different,
 * because nobody copies a dozen lines correctly. Worse, the set of layers
 * grows: one added here is drawn by every hand-built configuration already
 * written, which is how a security feed quietly acquires movement rings a year
 * later. A preset is one place to update instead of N call sites. */

/* Everything, engine features included. The player's view. */
cromwell::ViewLayers allLayers();

/* THE WORLD, WITHOUT THE INTERFACE ON TOP OF IT. The right starting point for
 * almost any second camera. */
cromwell::ViewLayers worldOnly();

/* A PLAN. worldOnly, minus the sky — from directly overhead it is not visible
 * anyway, so drawing it is a fullscreen pass contributing nothing. Shadows
 * stay: world-space, and they read well on a map. */
cromwell::ViewLayers planView();

}  // namespace game
