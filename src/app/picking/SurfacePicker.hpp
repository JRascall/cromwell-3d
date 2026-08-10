/* SurfacePicker.hpp — WHERE on a surface did the ray land, and which way does
 * it face?
 *
 * SINGLE RESPONSIBILITY: march the mouse ray through the lattice and report the
 * first surface it meets as a POINT and a NORMAL.
 *
 * NOT A REPLACEMENT FOR TilePicker, and the two answer different questions.
 * TilePicker answers "which tile is the player pointing at", which is a
 * gameplay question: it deliberately reports only standable floors and solid
 * masses, ignores walls entirely, and returns a cell index because a cell is
 * what movement, cover and line of sight are defined over. This answers "what
 * is the geometry under the cursor", which is an authoring question — and for
 * that a wall is a perfectly good answer, the exact height matters, and a cell
 * index throws away most of what the caller needs.
 *
 * WHAT IT IS FOR. Placing something ON a surface: a decal, and later whatever
 * else wants to be stuck to the world by pointing at it. A projector needs an
 * orientation, and the normal is that orientation — pick it here, where the
 * crossing that produced the hit is still in hand, rather than trying to
 * reconstruct it afterwards from a cell index that no longer remembers which
 * face was crossed.
 *
 * WALLS ARE FOUND BY THE CROSSING, not by intersecting a box. The march already
 * knows the previous sample's tile and the current one; when those differ it
 * has crossed a tile boundary, and the boundary IS the wall plane. The face
 * that gets the hit is the one on the side the ray arrived from, which is the
 * side that can be seen — so the normal always points back toward the camera
 * and a decal placed here is never stuck to the far side of the wall.
 */
#pragma once

#include "raylib.h"

#include "core/world/World.hpp"

#include <optional>

namespace xcom {

struct SurfaceHit {
    Vector3 point{};    /* on the visible surface                 */
    Vector3 normal{};   /* out of it, toward where the ray came from */

    /* Which cell the surface belongs to, for callers that still want one. */
    Cell cell{};

    /* True when the hit is a vertical face rather than a floor or a mass top.
     * Callers size things differently on the two — a mark on a wall wants a
     * shallow projection, one on the ground wants a deep one. */
    bool vertical = false;
};

class SurfacePicker {
public:
    explicit SurfacePicker(const World& world) : world_(world) {}

    /* Nothing when the ray leaves the grid without meeting anything. */
    std::optional<SurfaceHit> pick(const Ray& ray, int maxStorey) const;

private:
    /* Finer than TilePicker's 0.03: that one only has to land in the right
     * TILE, and this has to land on the right FACE — at 0.03 a grazing ray can
     * step past a 0.09-thick wall entirely and hit the floor behind it. */
    static constexpr float kStep = 0.01f;
    static constexpr float kMaxDistance = 140.0f;

    /* Half a full wall's thickness, so the point lands on the visible surface
     * rather than inside the geometry. Matches StoreyGeometryEmitter. */
    static constexpr float kWallHalfThickness = 0.045f;

    const World& world_;
};

}  // namespace xcom
