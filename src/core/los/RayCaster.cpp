#include "core/los/RayCaster.hpp"

#include "core/lattice/Constants.hpp"
#include "core/units/UnitRoster.hpp"

#include <cmath>
#include <limits>

namespace xcom {
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

    /* A window belongs to the BUILDING FLOOR, so the crossing height is
     * measured from the storey base — not from this 64uu cell, of which a
     * full wall occupies three. */
    /* See RayRules: half cover stops light but not sight. */
    const auto edgeIsOpaque = [&](const Edge& edge) {
        if (edge.cover == Cover::Full) return true;
        return rules_ == RayRules::Sunlight && edge.cover == Cover::Half;
    };

    const auto passesThroughGlass = [&](const Edge& edge, float t, int z) {
        if (!edge.window) return false;
        const float relative = heightAt(t) - Lattice::storeyBaseHeight(Lattice::storeyOfZ(z));
        return relative >= kWindowSill * kStoreyHeight
            && relative <= kWindowHead * kStoreyHeight;
    };

    for (int step = 0; step < kMaxSteps; step++) {
        if (cx == tx && cy == ty && cz == tz) return true;

        float t;
        if (nextX <= nextY && nextX <= nextZ) {
            t = nextX;
            const Dir  d    = stepX > 0 ? Dir::East : Dir::West;
            const Edge edge = world_.effectiveEdge(cx, cy, cz, d);
            if (edgeIsOpaque(edge) && !passesThroughGlass(edge, t, cz))
                return blockedAt(t);
            cx += stepX; nextX += deltaX;
        } else if (nextY <= nextZ) {
            t = nextY;
            const Dir  d    = stepY > 0 ? Dir::North : Dir::South;
            const Edge edge = world_.effectiveEdge(cx, cy, cz, d);
            if (edgeIsOpaque(edge) && !passesThroughGlass(edge, t, cz))
                return blockedAt(t);
            cy += stepY; nextY += deltaY;
        } else {
            t = nextZ;
            /* cell-boundary planes: only slabs sitting EXACTLY at the boundary
             * block here; offset slabs are caught by the straddle test below
             * at their REAL height. Canopies live at cell tops and always block. */
            if (stepZ > 0) {
                const Tile* above   = world_.tryAt(cx, cy, cz + 1);
                const Tile* current = world_.tryAt(cx, cy, cz);
                if ((above && above->hasFloor && !above->isRamp() &&
                     std::fabs(above->floorOffset) < 1e-6f) ||
                    (current && current->canopy))
                    return blockedAt(t);
            } else {
                const Tile* current = world_.tryAt(cx, cy, cz);
                const Tile* below   = world_.tryAt(cx, cy, cz - 1);
                if ((current && current->hasFloor && !current->isRamp() &&
                     std::fabs(current->floorOffset) < 1e-6f) ||
                    (below && below->canopy))
                    return blockedAt(t);
            }
            cz += stepZ; nextZ += deltaZ;
        }

        const Tile* cell = world_.tryAt(cx, cy, cz);
        const bool  atDestination = (cx == tx && cy == ty && cz == tz);

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
         * times out of three. */
        if (roster_ && !atDestination) {
            const Unit* blocker = roster_->lineOfSightBlockerAt({ cx, cy, cz });
            if (blocker && blocker != shooter_) {
                const float hullTop = blocker->baseHeight(world_) + blocker->hullHeight();
                if (enterH < hullTop) return blockedAt(t);
            }
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

}  // namespace xcom
