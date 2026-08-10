#include "game/los/RayCaster.hpp"

#include "game/lattice/Constants.hpp"
#include "game/units/roster/UnitRoster.hpp"

#include <cmath>
#include <limits>

namespace game {

namespace {

constexpr int   kMaxSteps = 500;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

}  // namespace

RayCaster::RayCaster(const World& world, RayRules rules)
    : world_(world), terrain_(world), mass_(world), roster_(nullptr), shooter_(nullptr),
      rules_(rules)
{
}

RayCaster::RayCaster(const World& world, const UnitRoster& roster, const Unit* shooter)
    : world_(world), terrain_(world), mass_(world), roster_(&roster), shooter_(shooter),
      rules_(RayRules::Sight)
{
}

bool RayCaster::cast(float ax, float ay, float aHeight,
                     float bx, float by, float bHeight,
                     Hit* hit) const
{
    /* tiny nudge avoids exact corner/plane degeneracies (endpoints sit at .5s) */
    ax += 1e-4f;   ay += 2e-4f;   aHeight += 3e-4f;
    bx += 1.3e-4f; by += 1.7e-4f; bHeight += 2.9e-4f;

    int cx = static_cast<int>(std::floor(ax));
    int cy = static_cast<int>(std::floor(ay));
    int cz = static_cast<int>(std::floor(aHeight / kCellHeight));

    const int tx = static_cast<int>(std::floor(bx));
    const int ty = static_cast<int>(std::floor(by));
    const int tz = static_cast<int>(std::floor(bHeight / kCellHeight));

    const float dirX = bx - ax;
    const float dirY = by - ay;
    const float dirH = bHeight - aHeight;

    const int stepX = dirX > 0 ? 1 : (dirX < 0 ? -1 : 0);
    const int stepY = dirY > 0 ? 1 : (dirY < 0 ? -1 : 0);
    const int stepZ = dirH > 0 ? 1 : (dirH < 0 ? -1 : 0);

    const float deltaX = dirX != 0.0f ? std::fabs(1.0f / dirX) : kInfinity;
    const float deltaY = dirY != 0.0f ? std::fabs(1.0f / dirY) : kInfinity;
    const float deltaZ = dirH != 0.0f ? std::fabs(kCellHeight / dirH) : kInfinity;

    float nextX = dirX != 0.0f
        ? ((stepX > 0 ? static_cast<float>(cx) + 1.0f - ax : ax - static_cast<float>(cx)) * deltaX)
        : kInfinity;
    float nextY = dirY != 0.0f
        ? ((stepY > 0 ? static_cast<float>(cy) + 1.0f - ay : ay - static_cast<float>(cy)) * deltaY)
        : kInfinity;
    float nextZ = dirH != 0.0f
        ? ((stepZ > 0 ? Lattice::cellBaseHeight(cz + 1) - aHeight
                      : aHeight - Lattice::cellBaseHeight(cz)) / std::fabs(dirH))
        : kInfinity;

    const auto heightAt = [&](float t) { return aHeight + dirH * t; };
    const auto blockedAt = [&](float t) {
        if (hit) {
            hit->x      = ax + dirX * t;
            hit->y      = ay + dirY * t;
            hit->height = aHeight + dirH * t;
        }
        return false;
    };

    /* See RayRules: half cover stops light but not sight. Takes the cover
     * GRADE rather than an Edge, because that is what the occlusion grid
     * stores — deliberately, so a new ray rule stays a change to this one
     * predicate. See OcclusionGrid.hpp. */
    const auto coverIsOpaque = [&](int cover) {
        if (cover == static_cast<int>(Cover::Full)) return true;
        return rules_ == RayRules::Sunlight && cover == static_cast<int>(Cover::Half);
    };

    /* A window belongs to the BUILDING FLOOR, so the crossing height is
     * measured from the storey base — not from this 64uu cell, of which a
     * full wall occupies three. */
    const auto inGlassBand = [&](float t, int z) {
        const float relative = heightAt(t) - Lattice::storeyBaseHeight(Lattice::storeyOfZ(z));
        return relative >= kWindowSill * kStoreyHeight
            && relative <= kWindowHead * kStoreyHeight;
    };

    /* THE SUMMARY THE WALK READS, instead of the tiles. Fetched once for the
     * whole ray; it is rebuilt only when the geometry changes. */
    const OcclusionGrid& occ = world_.occlusion();
    const int strideY = occ.strideY();
    const int strideZ = occ.strideZ();

    /* Kept in step with cx/cy/cz so a step is an add rather than a multiply.
     * Only ever read through occAt(), which bounds-checks first. */
    int cellIndex = occ.lattice().isValid(cx, cy, cz) ? occ.lattice().index(cx, cy, cz) : 0;

    const auto occAt = [&](int x, int y, int z, int index) -> std::uint16_t {
        return occ.lattice().isValid(x, y, z) ? occ.at(index) : std::uint16_t(0);
    };

    for (int step = 0; step < kMaxSteps; step++) {
        if (cx == tx && cy == ty && cz == tz) return true;

        const std::uint16_t here = occAt(cx, cy, cz, cellIndex);

        float t;
        if (nextX <= nextY && nextX <= nextZ) {
            t = nextX;
            const int d = toIndex(stepX > 0 ? Dir::East : Dir::West);
            if (coverIsOpaque(occ::coverOf(here, d)) &&
                !(occ::hasWindow(here, d) && inGlassBand(t, cz)))
                return blockedAt(t);
            cx += stepX; cellIndex += stepX; nextX += deltaX;
        } else if (nextY <= nextZ) {
            t = nextY;
            const int d = toIndex(stepY > 0 ? Dir::North : Dir::South);
            if (coverIsOpaque(occ::coverOf(here, d)) &&
                !(occ::hasWindow(here, d) && inGlassBand(t, cz)))
                return blockedAt(t);
            cy += stepY; cellIndex += stepY * strideY; nextY += deltaY;
        } else {
            t = nextZ;
            /* cell-boundary planes: only slabs sitting EXACTLY at the boundary
             * block here; offset slabs are caught by the straddle test below
             * at their REAL height. Canopies live at cell tops and always block. */
            if (stepZ > 0) {
                if ((occAt(cx, cy, cz + 1, cellIndex + strideZ) & occ::kSlab) ||
                    (here & occ::kCanopy))
                    return blockedAt(t);
            } else {
                if ((here & occ::kSlab) ||
                    (occAt(cx, cy, cz - 1, cellIndex - strideZ) & occ::kCanopy))
                    return blockedAt(t);
            }
            cz += stepZ; cellIndex += stepZ * strideZ; nextZ += deltaZ;
        }

        const bool atDestination = (cx == tx && cy == ty && cz == tz);

        /* THE ESCAPE HATCH. Everything below this point needs real arithmetic
         * against real heights — ramp surfaces, offset slabs, the top of a
         * solid mass, a hull — and none of it is expressible in the summary.
         * Cells that need it say so; the rest of the walk skips straight on.
         *
         * The cell ABOVE is included because the slab straddle test at the
         * bottom reads cz and cz+1. That makes this a conservative superset:
         * it can only ever send the ray into work that turns out to be
         * unnecessary, never skip work that mattered. */
        const std::uint16_t entered = occAt(cx, cy, cz, cellIndex);
        const std::uint16_t above   = occAt(cx, cy, cz + 1, cellIndex + strideZ);

        /* Which hull might block here, resolved BEFORE the skip so that a cell
         * with no body in it and no awkward geometry can be stepped over
         * whole. Hoisting this only became worth doing once the roster kept an
         * occupancy index — as a walk over every unit it was the expensive
         * thing, and asking it earlier would have made this slower, not
         * faster. See UnitRoster::bindLattice. */
        const Unit* blocker = nullptr;
        if (roster_ && !atDestination) {
            const Unit* occupant = roster_->lineOfSightBlockerAt({ cx, cy, cz });
            if (occupant && occupant != shooter_) blocker = occupant;
        }

        if (!((entered | above) & occ::kNeedsTile) && !blocker) continue;

        const Tile* cell = world_.tryAt(cx, cy, cz);

        float exitT = nextX;
        if (nextY < exitT) exitT = nextY;
        if (nextZ < exitT) exitT = nextZ;
        if (exitT > 1.0f) exitT = 1.0f;

        const float enterH = heightAt(t);
        const float exitH  = heightAt(exitT);
        const float enterX = ax + dirX * t,     enterY = ay + dirY * t;
        const float exitX  = ax + dirX * exitT, exitY  = ay + dirY * exitT;

        /* solid mass blocks by PHYSICAL height: a full container fills its
         * cell, but a plinth (blocked base with a low platform above) only
         * blocks below its real top — you can shoot OVER half-height geometry */
        if (cell && cell->blocked && !atDestination) {
            if (const std::optional<float> top = mass_.topHeight(cx, cy, cz))
                if (enterH < *top - 0.02f || exitH < *top - 0.02f) return blockedAt(t);
        }

        /* a big unit's hull is terrain to sight: rays crossing its cells below
         * hull height are blocked. Never for the shooter's own hull, and never
         * for the destination cell — you can still shoot AT the tank. Height
         * comes from the unit's OWN footprint base, not the cell base: cells
         * are a third of a storey, so a cell-relative hull would sit wrong two
         * times out of three. Resolved above the skip. */
        if (blocker) {
            const float hullTop = blocker->baseHeight(world_) + blocker->hullHeight();
            if (enterH < hullTop) return blockedAt(t);
        }

        /* a staircase's solid mass: inside a ramp cell the volume below the
         * walk surface is solid. Shots over the top or up the flight pass;
         * shots THROUGH the steps do not. */
        if (cell && cell->isRamp()) {
            const float surfaceAtEnter = terrain_.surfaceHeightAt(cx, cy, cz, enterX, enterY);
            const float surfaceAtExit  = terrain_.surfaceHeightAt(cx, cy, cz, exitX, exitY);
            if (enterH < surfaceAtEnter - 0.02f || exitH < surfaceAtExit - 0.02f)
                return blockedAt(t);
        }

        /* FLOOR SLABS AT REAL HEIGHTS: a slab blocks where the ray STRADDLES
         * its actual plane inside this cell — a plinth top, a raised floor, a
         * sunken road all block exactly where they physically are. */
        for (int k = 0; k < 2; k++) {
            const int   z    = cz + k;
            const Tile* slab = world_.tryAt(cx, cy, z);
            if (!slab || !slab->hasFloor || slab->isRamp()) continue;

            const float offset = slab->floorOffset;
            if (std::fabs(offset) < 1e-6f) continue;   /* boundary slab: handled above */

            const float slabH = Lattice::cellBaseHeight(z) + offset;
            if (slabH <= Lattice::cellBaseHeight(cz) + 1e-7f) continue;
            if (slabH >= Lattice::cellBaseHeight(cz + 1) - 1e-7f) continue;
            if ((enterH - slabH) * (exitH - slabH) < -1e-7f) return blockedAt(t);
        }
    }
    return false;
}

}  // namespace game
