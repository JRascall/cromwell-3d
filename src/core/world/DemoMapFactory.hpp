/* DemoMapFactory.hpp — the prototype's one authored map.
 *
 * SINGLE RESPONSIBILITY: describe THIS map. It owns no rules, only choices;
 * every write goes through MapAuthor.
 *
 * Storeys: 0 ground, 1 upper storey ("level 2"), 2 rooftop. Each storey is
 * THREE 64uu z cells, so authoring goes through the storey helpers rather
 * than touching cells directly. Heights are absolute: storey s sits at
 * s * kStoreyHeight.
 *
 * THE CALL ORDER IN build() IS LOAD-BEARING. Two dependencies in particular:
 *   - a ramp clears the floor of the cell it lands in, so flights must be
 *     authored AFTER the slab they cut through;
 *   - micro-relief skips already-blocked tiles, and it runs BEFORE the solid
 *     containers are placed — so the road is painted onto (3,6) and (3,7)
 *     and only then are they made solid.
 */
#pragma once

#include "core/world/World.hpp"

#include <iosfwd>

namespace xcom {

class MapAuthor;

class DemoMapFactory {
public:
    /* Clears `world` and rebuilds the demo map into it. */
    static void build(World& world, std::ostream* diagnostics = nullptr);

private:
    static void buildGround(MapAuthor& author, const World& world);
    static void buildBuildingShell(MapAuthor& author, World& world);
    static void buildStaircaseA(MapAuthor& author);
    static void buildRoofAndStaircaseB(MapAuthor& author);
    static void buildStaircaseC(MapAuthor& author);
    static void buildLaddersAndPortals(MapAuthor& author, World& world);
    static void buildRoofParapet(MapAuthor& author);
    static void buildMicroRelief(MapAuthor& author, World& world);
    static void buildScatteredCover(MapAuthor& author);
    static void buildPlinth(MapAuthor& author, World& world);
    static void makeStructureDestructible(World& world);

    static void setCrate(MapAuthor& author, int x, int y, int storey);
};

}  // namespace xcom
