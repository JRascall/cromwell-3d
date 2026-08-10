#include "game/border/band/BandConnectivity.hpp"

#include "game/lattice/Constants.hpp"

#include <cmath>

namespace game {


int BandConnectivity::nearestInBand(int cell, Dir d, float tolerance) const
{
    const Lattice& lattice = world_.lattice();
    const Cell from = lattice.cellAt(cell);

    const int nx = from.x + dx(d);
    const int ny = from.y + dy(d);
    if (!lattice.inBounds(nx, ny)) return -1;

    /* sample both surfaces at the MIDPOINT OF THE SHARED FACE */
    const float midX = static_cast<float>(from.x) + 0.5f + static_cast<float>(dx(d)) * 0.5f;
    const float midY = static_cast<float>(from.y) + 0.5f + static_cast<float>(dy(d)) * 0.5f;
    const float myHeight = terrain_.surfaceHeightAt(from, midX, midY);

    int   best = -1;
    float bestDelta = tolerance;

    for (int z = 0; z < lattice.depth(); z++) {
        const int candidate = lattice.index(nx, ny, z);
        if (!band_->contains(candidate)) continue;

        const float delta =
            std::fabs(terrain_.surfaceHeightAt(nx, ny, z, midX, midY) - myHeight);
        if (delta <= bestDelta) { bestDelta = delta; best = candidate; }
    }
    return best;
}

int BandConnectivity::acrossSlit(int cell, Dir d) const
{
    return nearestInBand(cell, d, kRampMaxRise);
}

void BandConnectivity::rebuild(const Band& band)
{
    band_ = &band;

    const int cellCount = world_.lattice().cellCount();
    links_.assign(static_cast<std::size_t>(cellCount), { -1, -1, -1, -1 });

    for (int i = 0; i < cellCount; i++) {
        if (!band.contains(i)) continue;
        for (Dir d : kAllDirs)
            links_[static_cast<std::size_t>(i)][toIndex(d)] = nearestInBand(i, d, kWalkStep);
    }

    /* RECIPROCITY. The corner walk is a cycle only if its successor relation
     * is injective, and that needs this table symmetric: if T reaches A
     * northward, A must reach T southward. A column can hold several in-band
     * cells inside the tolerance, so "nearest" can pick a partner that does
     * not pick back — and one non-reciprocal link fragments an entire loop
     * into open strips. Ramps are where this bites, because their surface
     * height varies across the tile.
     *
     * Dropping the half-link is the conservative repair: that face then reads
     * as a boundary from both sides, which is never wrong — only occasionally
     * more line than strictly needed. */
    for (int i = 0; i < cellCount; i++) {
        for (Dir d : kAllDirs) {
            const int partner = links_[static_cast<std::size_t>(i)][toIndex(d)];
            if (partner < 0) continue;
            if (links_[static_cast<std::size_t>(partner)][toIndex(opposite(d))] != i)
                links_[static_cast<std::size_t>(i)][toIndex(d)] = -1;
        }
    }
}

int BandConnectivity::linkedNeighbour(int cell, Dir d) const
{
    return links_[static_cast<std::size_t>(cell)][toIndex(d)];
}

bool BandConnectivity::isBoundary(int cell, Dir d) const
{
    return band_->contains(cell) && linkedNeighbour(cell, d) < 0;
}

bool BandConnectivity::successor(int cell, Dir d, int& outCell, Dir& outDir) const
{
    const int forward = linkedNeighbour(cell, turnLeft(d));
    const int diagonal = (forward >= 0) ? linkedNeighbour(forward, d) : -1;

    if (diagonal >= 0 && isBoundary(diagonal, turnRight(d))) {
        outCell = diagonal; outDir = turnRight(d); return true;
    }
    if (forward >= 0 && isBoundary(forward, d)) {
        outCell = forward;  outDir = d;            return true;
    }
    if (isBoundary(cell, turnLeft(d))) {
        outCell = cell;     outDir = turnLeft(d);  return true;
    }

    /* fourth: the far side of a slit */
    const int slitPartner = acrossSlit(cell, d);
    if (slitPartner >= 0 && isBoundary(slitPartner, opposite(d))) {
        outCell = slitPartner; outDir = opposite(d); return true;
    }
    return false;
}

}  // namespace game
