/* RayCaster.hpp — a 3D DDA (Amanatides-Woo) over the tile lattice.
 *
 * SINGLE RESPONSIBILITY: trace one segment and say whether it arrives.
 *
 * Stopped by blocked cells, full-cover edges (windows pass only within the
 * glass band), floor slabs and canopies, and stair mass. Half cover never
 * blocks LOS. The walk steps in 64uu CELLS, matching the tile grid, so it
 * visits every cell a wall or slab could live in.
 *
 * Big units' hulls also block: XCOM treats them as mobile high cover, while
 * 1x1 units are transparent — you shoot past squadmates.
 *
 * HULL CONTEXT IS CONSTRUCTOR STATE, NOT GLOBAL STATE. The C original kept a
 * module-scope `g_units`/`g_ignore` pair that callers had to set before a
 * batch of casts and clear afterwards; forgetting either half silently
 * changed what every later cast could see. Here a caster either has a roster
 * or does not, for its whole life.
 */
#pragma once

#include "core/query/BlockedMass.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

namespace xcom {

class Unit;
class UnitRoster;

/* WHAT COUNTS AS BLOCKING, and it is not the same question twice.
 *
 *   Sight     XCOM's rule: half cover never blocks line of sight. You shoot
 *             over a sandbag wall; that is the entire point of low cover.
 *   Sunlight  a sandbag wall is opaque. Light does not care that a soldier
 *             could shoot over it.
 *
 * The traversal is identical either way, so this selects a predicate rather
 * than a second copy of the DDA — one of these walks is quite enough to keep
 * correct. Windows pass in both: glass transmits sight and light alike. */
enum class RayRules {
    Sight,
    Sunlight,
};

class RayCaster {
public:
    struct Hit {
        float x = 0.0f;
        float y = 0.0f;
        float height = 0.0f;
    };

    /* Pure terrain — no hull blocking. */
    explicit RayCaster(const World& world, RayRules rules = RayRules::Sight);

    /* Hull-aware. `shooter` (may be nullptr) is the unit whose own hull never
     * blocks its own view. */
    RayCaster(const World& world, const UnitRoster& roster, const Unit* shooter);

    /* True if the ray reaches the far end. `hit` (optional) receives the
     * impact point when blocked. */
    bool cast(float ax, float ay, float aHeight,
              float bx, float by, float bHeight,
              Hit* hit = nullptr) const;

private:
    const World&      world_;
    Terrain           terrain_;
    BlockedMass       mass_;
    const UnitRoster* roster_;   /* nullptr disables hull blocking */
    const Unit*       shooter_;
    RayRules          rules_;
};

}  // namespace xcom
