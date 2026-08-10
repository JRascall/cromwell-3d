#include "core/query/Terrain.hpp"

namespace xcom {

float Terrain::rampFraction(const Tile& tile, int x, int y, float worldX, float worldY)
{
    float u;
    switch (tile.rampDir) {
        case Dir::North: u = worldY - static_cast<float>(y);          break;
        case Dir::South: u = 1.0f - (worldY - static_cast<float>(y)); break;
        case Dir::East:  u = worldX - static_cast<float>(x);          break;
        default:         u = 1.0f - (worldX - static_cast<float>(x)); break;   /* West */
    }
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    return u;
}

float Terrain::surfaceHeightAt(int x, int y, int z, float worldX, float worldY) const
{
    const Tile* tile = world_.tryAt(x, y, z);
    if (!tile) return Lattice::cellBaseHeight(z);
    if (!tile->isRamp()) return Lattice::cellBaseHeight(z) + tile->floorOffset;
    return tile->rampBaseHeight + rampFraction(*tile, x, y, worldX, worldY) * tile->rampRise;
}

float Terrain::centerHeight(int x, int y, int z) const
{
    return surfaceHeightAt(x, y, z, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
}

}  // namespace xcom
