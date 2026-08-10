#include "app/picking/SurfacePicker.hpp"

#include "core/query/BlockedMass.hpp"
#include "core/query/Terrain.hpp"

#include <cmath>

namespace xcom {
namespace {

/* Whether a face is something a decal can be stuck to.
 *
 * WINDOWS ARE EXCLUDED, and not as an afterthought: pbr.fs.glsl refuses decals
 * on blended surfaces (a mark inside a 6%-opaque pane is a smear hanging in the
 * opening, not a mark on the glass), so a placement that lands on one would
 * simply produce nothing. Failing to pick is a far better answer than picking
 * somewhere the decal cannot appear. */
bool facePaintable(const Edge& edge)
{
    return edge.cover != Cover::None && !edge.window;
}

}  // namespace

std::optional<SurfaceHit> SurfacePicker::pick(const Ray& ray, int maxStorey) const
{
    const Lattice& lattice = world_.lattice();
    const Terrain terrain(world_);
    const BlockedMass mass(world_);

    float previousX = ray.position.x;
    float previousH = ray.position.y;
    float previousY = ray.position.z;

    int previousCellX = static_cast<int>(std::floor(previousX));
    int previousCellY = static_cast<int>(std::floor(previousY));

    for (float t = kStep; t < kMaxDistance; t += kStep) {
        const float px = ray.position.x + ray.direction.x * t;
        const float ph = ray.position.y + ray.direction.y * t;
        const float py = ray.position.z + ray.direction.z * t;

        const int x = static_cast<int>(std::floor(px));
        const int y = static_cast<int>(std::floor(py));

        /* The lattice's y is the world's z; height is its own axis. */
        const int z = lattice.cellOfHeight(ph);

        if (lattice.isValid(x, y, z) && Lattice::storeyOfZ(z) <= maxStorey) {

            /* ---- 1. a wall, found by the boundary the step just crossed -----
             * Checked BEFORE the floor. A ray coming down at a shallow angle
             * crosses the wall plane and the floor plane within a step or two
             * of each other, and the wall is the nearer of the two — testing
             * the floor first would put a poster meant for the wall on the
             * pavement in front of it. */
            if (x != previousCellX && lattice.inBounds(previousCellX, previousCellY)) {
                const bool eastward = (x > previousCellX);
                const Dir  d = eastward ? Dir::East : Dir::West;

                if (facePaintable(world_.effectiveEdge(previousCellX, previousCellY, z, d))) {
                    SurfaceHit hit;
                    /* The plane is the boundary between the two tiles, which is
                     * the larger of the two x indices either way. */
                    const float plane = static_cast<float>(eastward ? x : previousCellX);
                    hit.normal   = Vector3{ eastward ? -1.0f : 1.0f, 0.0f, 0.0f };
                    hit.point    = Vector3{ plane + hit.normal.x * kWallHalfThickness, ph, py };
                    hit.cell     = Cell{ previousCellX, previousCellY, z };
                    hit.vertical = true;
                    return hit;
                }
            }
            if (y != previousCellY && lattice.inBounds(previousCellX, previousCellY)) {
                const bool northward = (y > previousCellY);
                const Dir  d = northward ? Dir::North : Dir::South;

                if (facePaintable(world_.effectiveEdge(previousCellX, previousCellY, z, d))) {
                    SurfaceHit hit;
                    const float plane = static_cast<float>(northward ? y : previousCellY);
                    hit.normal   = Vector3{ 0.0f, 0.0f, northward ? -1.0f : 1.0f };
                    hit.point    = Vector3{ px, ph, plane + hit.normal.z * kWallHalfThickness };
                    hit.cell     = Cell{ previousCellX, previousCellY, z };
                    hit.vertical = true;
                    return hit;
                }
            }

            /* ---- 2. a floor or the top of a solid mass ---------------------
             * Top-down so the highest surface in the column wins, which is what
             * makes a roof pick as a roof rather than as the floor under it. */
            for (int probe = lattice.depth() - 1; probe >= 0; probe--) {
                if (Lattice::storeyOfZ(probe) > maxStorey) continue;
                const Tile& tile = world_.at(lattice.index(x, y, probe));

                if (tile.blocked) {
                    const std::optional<float> top = mass.topHeight(x, y, probe);
                    if (top && ph <= *top && ph >= Lattice::cellBaseHeight(probe) - 0.05f) {
                        SurfaceHit hit;
                        hit.normal = Vector3{ 0.0f, 1.0f, 0.0f };
                        hit.point  = Vector3{ px, *top, py };
                        hit.cell   = Cell{ x, y, probe };
                        return hit;
                    }
                    continue;
                }
                if (!tile.hasFloor && !tile.isRamp()) continue;

                /* CROSSING, not "below" — the same test TilePicker documents.
                 * Standing outside a building the ray spends most of its length
                 * beneath the upper floor, and a plain `ph <= surface` would
                 * pick that floor straight through the wall. */
                const float surface = terrain.surfaceHeightAt(x, y, probe, px, py);
                const float previousSurface =
                    terrain.surfaceHeightAt(x, y, probe, previousX, previousY);

                if (previousH > previousSurface && ph <= surface) {
                    SurfaceHit hit;
                    hit.normal = Vector3{ 0.0f, 1.0f, 0.0f };
                    hit.point  = Vector3{ px, surface, py };
                    hit.cell   = Cell{ x, y, probe };
                    return hit;
                }
            }
        }

        if (ph < -3.0f) break;

        previousX = px;
        previousH = ph;
        previousY = py;
        previousCellX = x;
        previousCellY = y;
    }
    return std::nullopt;
}

}  // namespace xcom
