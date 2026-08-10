/* Terrain.hpp — THE height authority.
 *
 * SINGLE RESPONSIBILITY: say how high the walkable surface is at a point.
 * Movement, LOS, cover and the border ribbon all read this one class, so they
 * can never disagree about where the ground is.
 *
 * A ramp is a PLANE, so a chord across it IS the surface — no drape, no
 * per-tread sampling, no stair-foot fudge.
 */
#pragma once

#include "game/world/World.hpp"

namespace game {


class Terrain {
public:
    explicit Terrain(const World& world) : world_(world) {}

    /* How far along a ramp's uphill axis a world point sits, clamped to
     * [0, 1]. Pure geometry — no world access, hence static. */
    static float rampFraction(const Tile& tile, int x, int y, float worldX, float worldY);

    /* Absolute height of the walk surface at a world point inside a cell. */
    float surfaceHeightAt(int x, int y, int z, float worldX, float worldY) const;
    float surfaceHeightAt(const Cell& c, float worldX, float worldY) const
    {
        return surfaceHeightAt(c.x, c.y, c.z, worldX, worldY);
    }

    /* The surface height at the tile's centre — the usual sample point. */
    float centerHeight(int x, int y, int z) const;
    float centerHeight(const Cell& c) const { return centerHeight(c.x, c.y, c.z); }

private:
    const World& world_;
};

}  // namespace game
