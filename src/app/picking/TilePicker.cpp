#include "app/picking/TilePicker.hpp"

#include <cmath>

namespace xcom {

std::optional<int> TilePicker::pick(const Ray& ray, int maxStorey) const
{
    const Lattice& lattice = world_.lattice();

    float previousX = ray.position.x;
    float previousH = ray.position.y;
    float previousY = ray.position.z;

    for (float t = kStep; t < kMaxDistance; t += kStep) {
        const float px = ray.position.x + ray.direction.x * t;
        const float ph = ray.position.y + ray.direction.y * t;
        const float py = ray.position.z + ray.direction.z * t;

        const int x = static_cast<int>(std::floor(px));
        const int y = static_cast<int>(std::floor(py));

        if (lattice.inBounds(x, y)) {
            /* top-down so the highest surface wins where a column has several */
            for (int z = lattice.depth() - 1; z >= 0; z--) {
                if (Lattice::storeyOfZ(z) > maxStorey) continue;
                const Tile& tile = world_.at(lattice.index(x, y, z));

                if (tile.blocked) {
                    const std::optional<float> top = mass_.topHeight(x, y, z);
                    if (top && ph <= *top && ph >= Lattice::cellBaseHeight(z) - 0.05f)
                        return lattice.index(x, y, z);
                    continue;
                }
                if (!tile.hasFloor && !tile.isRamp()) continue;

                /* CROSSING test, not "below": standing outside a building the
                 * ray is under the upper floor for most of its length, and a
                 * plain ph <= surface would pick that floor through the wall. */
                const float surface         = terrain_.surfaceHeightAt(x, y, z, px, py);
                const float previousSurface = terrain_.surfaceHeightAt(x, y, z, previousX, previousY);
                if (previousH > previousSurface && ph <= surface) return lattice.index(x, y, z);
            }
        }

        if (ph < -3.0f) break;
        previousX = px;
        previousH = ph;
        previousY = py;
    }
    return std::nullopt;
}

}  // namespace xcom
