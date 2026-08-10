/* ColumnScanner.hpp — what surfaces exist in one (x, y) column?
 *
 * SINGLE RESPONSIBILITY: enumerate a column's flat surfaces and its ramps,
 * with ABSOLUTE heights. Move generation asks this and then reasons purely in
 * world units, which is what lets connectivity come from height deltas rather
 * than from which z cell a surface happens to be stored in.
 */
#pragma once

#include "core/lattice/Direction.hpp"
#include "core/world/World.hpp"

#include <vector>

namespace xcom {

struct FlatSurface {
    int   z = 0;
    float height = 0.0f;
};

struct RampSurface {
    int   z = 0;
    Dir   uphill = Dir::North;
    float lowHeight  = 0.0f;
    float highHeight = 0.0f;
    float midHeight  = 0.0f;
};

class ColumnScanner {
public:
    explicit ColumnScanner(const World& world) : world_(world) {}

    /* Both append into caller-owned vectors, which are cleared first. Reusing
     * one vector across a search keeps this allocation-free after warm-up. */
    void flatSurfaces(int x, int y, std::vector<FlatSurface>& out) const;
    void ramps(int x, int y, std::vector<RampSurface>& out) const;

private:
    const World& world_;
};

}  // namespace xcom
