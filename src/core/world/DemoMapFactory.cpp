#include "core/world/DemoMapFactory.hpp"

#include "core/world/MapAuthor.hpp"

namespace xcom {
namespace {

/* building footprint */
constexpr int kBX0 = 4, kBX1 = 11, kBY0 = 12, kBY1 = 19;

}  // namespace

void DemoMapFactory::build(World& world, std::ostream* diagnostics)
{
    world.clear();
    MapAuthor author(world, diagnostics);

    /* order is load-bearing — see the header */
    buildGround(author, world);
    buildBuildingShell(author, world);
    buildStaircaseA(author);
    buildRoofAndStaircaseB(author);
    buildStaircaseC(author);
    buildLaddersAndPortals(author, world);
    buildRoofParapet(author);
    buildMicroRelief(author, world);
    buildScatteredCover(author);
    buildPlinth(author, world);

    /* MUST RUN LAST: anything authored after it would silently come out
     * indestructible. */
    makeStructureDestructible(world);
}

void DemoMapFactory::setCrate(MapAuthor& author, int x, int y, int storey)
{
    for (Dir d : kAllDirs) author.setWall(x, y, storey, d, Cover::Half, false, false);
}

/* --- ground level: open field ------------------------------------------ */
void DemoMapFactory::buildGround(MapAuthor& author, const World& world)
{
    const Lattice& lattice = world.lattice();
    for (int y = 0; y < lattice.height(); y++)
        for (int x = 0; x < lattice.width(); x++)
            author.setFloorAt(x, y, 0.0f);
}

/* --- 2-storey building -------------------------------------------------- */
void DemoMapFactory::buildBuildingShell(MapAuthor& author, World& world)
{
    /* ground-floor walls (full cover = blocks movement + LOS, all 3 cells) */
    for (int x = kBX0; x <= kBX1; x++) {
        author.setWall(x, kBY0, 0, Dir::South, Cover::Full, false, false);
        author.setWall(x, kBY1, 0, Dir::North, Cover::Full, false, false);
    }
    for (int y = kBY0; y <= kBY1; y++) {
        author.setWall(kBX0, y, 0, Dir::West, Cover::Full, false, false);
        author.setWall(kBX1, y, 0, Dir::East, Cover::Full, false, false);
    }
    author.clearWall(7, kBY0, 0, Dir::South);                             /* doorway */
    author.setWall(10, kBY0, 0, Dir::South, Cover::Full, false, true);    /* window  */
    author.setWall(11, 15,   0, Dir::East,  Cover::Full, false, true);    /* window  */

    /* upper-storey floor + south balcony overhang (nothing beneath it) */
    for (int y = kBY0; y <= kBY1; y++)
        for (int x = kBX0; x <= kBX1; x++) author.setFloorAt(x, y, kStoreyHeight);
    for (int y = 10; y <= 11; y++)
        for (int x = 4; x <= 9; x++) author.setFloorAt(x, y, kStoreyHeight);

    /* upper-storey walls */
    for (int x = kBX0; x <= kBX1; x++) {
        author.setWall(x, kBY0, 1, Dir::South, Cover::Full, false, false);
        author.setWall(x, kBY1, 1, Dir::North, Cover::Full, false, false);
    }
    for (int y = kBY0; y <= kBY1; y++) {
        author.setWall(kBX0, y, 1, Dir::West, Cover::Full, false, false);
        author.setWall(kBX1, y, 1, Dir::East, Cover::Full, false, false);
    }
    author.clearWall(6, kBY0, 1, Dir::South);                             /* balcony door */
    author.setWall(9, kBY0, 1, Dir::South, Cover::Full, false, true);     /* window       */

    /* balcony railing = half cover (climb over it, then drop to the ground) */
    for (int x = 4; x <= 9; x++)
        author.setWall(x, 10, 1, Dir::South, Cover::Half, false, false);
    for (int y = 10; y <= 11; y++) {
        author.setWall(4, y, 1, Dir::West, Cover::Half, false, false);
        author.setWall(9, y, 1, Dir::East, Cover::Half, false, false);
    }

    /* awning over the balcony — a ROOF that is NOT a floor: blocks vertical
     * LOS and can't be walked on or fallen through. Top cell of storey 1. */
    const int awningZ = Lattice::storeyBaseZ(1) + kCellsPerStorey - 1;
    for (int y = 10; y <= 11; y++)
        for (int x = 4; x <= 9; x++)
            world.at(world.lattice().index(x, y, awningZ)).canopy = true;
}

/* --- staircase A: ground -> upper storey --------------------------------
 * Each tile climbs kTileSize (the 45 deg cap), so a 2.0-high storey needs two
 * tiles; flights chain by absolute height 0 -> 1 -> 2. Tiles above the run
 * lose their floor. */
void DemoMapFactory::buildStaircaseA(MapAuthor& author)
{
    for (int k = 0; k < 2; k++) {
        const int sx = 5 + k;
        author.setRamp(sx, 13, Dir::North, 0.0f, 1.0f);
        author.setRamp(sx, 14, Dir::North, 1.0f, 1.0f);
        author.clearFloorAt(sx, 13, kStoreyHeight);      /* stairwell openings */
        author.clearFloorAt(sx, 14, kStoreyHeight);
    }
}

/* --- rooftop over the north half + staircase B up to it ----------------- */
void DemoMapFactory::buildRoofAndStaircaseB(MapAuthor& author)
{
    for (int y = 16; y <= kBY1; y++)
        for (int x = kBX0; x <= kBX1; x++) author.setFloorAt(x, y, 2.0f * kStoreyHeight);
    author.setRamp(10, 14, Dir::North, 2.0f, 1.0f);
    author.setRamp(10, 15, Dir::North, 3.0f, 1.0f);
}

/* --- staircase C: a long EXTERNAL 4-tile run from the ground to the roof.
 * The upper flights pass OVER walkable ground (multi-surface columns). */
void DemoMapFactory::buildStaircaseC(MapAuthor& author)
{
    author.setRamp(15, 17, Dir::West, 0.0f, 1.0f);
    author.setRamp(14, 17, Dir::West, 1.0f, 1.0f);
    author.setRamp(13, 17, Dir::West, 2.0f, 1.0f);
    author.setRamp(12, 17, Dir::West, 3.0f, 1.0f);
}

/* --- ladders (edge data; the landing level is DERIVED) + portals -------- */
void DemoMapFactory::buildLaddersAndPortals(MapAuthor& author, World& world)
{
    author.setLadderWall(12, 18, 0, Dir::West);   /* east facade -> roof (2 storeys) */
    author.setLadderWall(5,   9, 0, Dir::North);  /* balcony rim  -> 1 storey        */

    /* portals: two tiles sharing an id are linked */
    world.at(world.lattice().index(16, 3,  0)).portal = 'A';
    world.at(world.lattice().index(1,  22, 0)).portal = 'A';
}

/* --- roof parapet (half cover) on N + W; E and S left open for drop-downs */
void DemoMapFactory::buildRoofParapet(MapAuthor& author)
{
    for (int x = kBX0; x <= kBX1; x++)
        author.setWall(x, kBY1, 2, Dir::North, Cover::Half, false, false);
    for (int y = 16; y <= kBY1; y++)
        author.setWall(kBX0, y, 2, Dir::West, Cover::Half, false, false);
}

/* --- micro-relief: road / kerb / lawn -----------------------------------
 * These ARE real walk heights, not decoration. XCOM kerbs line up with the
 * tile grid, so the height change lands exactly on a tile boundary -- and the
 * border ribbon, which runs along that same boundary, caps the kerb instead
 * of floating behind it. artTag only picks the material.
 *
 * The blocked test skips nothing on the demo map: this runs BEFORE the solid
 * containers are placed, which is deliberate — see the header. */
void DemoMapFactory::buildMicroRelief(MapAuthor& author, World& world)
{
    const Lattice& lattice = world.lattice();

    for (int x = 0; x < lattice.width(); x++)
        for (int k = 0; k < 2; k++) {
            const int y = 6 + k;
            Tile& tile = world.at(lattice.index(x, y, 0));
            if (tile.blocked) continue;
            author.setFloorAt(x, y, -0.15f);
            tile.artTag = Art::Road;
        }

    for (int y = 1; y <= 4; y++)
        for (int x = 2; x <= 7; x++) {
            Tile& tile = world.at(lattice.index(x, y, 0));
            if (tile.blocked) continue;
            author.setFloorAt(x, y, 0.05f);
            tile.artTag = Art::Grass;
        }
}

/* --- scattered low cover, then fully blocked tiles ---------------------- */
void DemoMapFactory::buildScatteredCover(MapAuthor& author)
{
    setCrate(author, 15,  8, 0);
    setCrate(author, 17,  9, 0);
    setCrate(author, 20, 12, 0);
    for (int x = 14; x <= 18; x++)
        author.setWall(x, 5, 0, Dir::North, Cover::Half, false, false);

    /* solid containers / rock */
    author.setSolid(18, 14, 0); author.setSolid(19, 14, 0);
    author.setSolid(18, 15, 0); author.setSolid(19, 15, 0);
    author.setSolid(3,   6, 0); author.setSolid(3,   7, 0);
}

/* --- PLINTH: half-height climbable geometry -----------------------------
 * Solid mass from the ground with a walkable top exactly ONE Z CELL up.
 * 64uu is XCOM's Cover_LowCoverHeight, so this is a medium obstacle by
 * construction rather than by taste: roughly three quarters of a soldier's
 * height, chest-high, vaultable.
 *
 * Everything else follows from that one number: units MANTLE up (rise 0.667
 * <= kMantleMax), drop back off, its faces grant derived HALF cover at its
 * base (0.667 >= kLedgeHalf, < kLedgeFull), and LOS passes over it but not
 * through.
 *
 * It was authored at 1.0, carried over from the JS build where it read as
 * "half a storey". That is 1.11x the soldier's height — the thing meant to be
 * cover stood taller than the people hiding behind it. */
void DemoMapFactory::buildPlinth(MapAuthor& author, World& world)
{
    for (int y = 2; y <= 4; y++)
        for (int x = 20; x <= 22; x++) {
            world.at(world.lattice().index(x, y, 0)).blocked = true;
            author.setFloorAt(x, y, kCellHeight);
        }
}

/* --- XCOM default: STRUCTURE IS DESTRUCTIBLE ---------------------------
 * One pass makes every authored cover edge destructible and every elevated
 * floor blowable. Solid blocked tiles stay indestructible. */
void DemoMapFactory::makeStructureDestructible(World& world)
{
    const Lattice& lattice = world.lattice();

    for (int z = 0; z < lattice.depth(); z++)
    for (int y = 0; y < lattice.height(); y++)
    for (int x = 0; x < lattice.width(); x++) {
        Tile& tile = world.at(lattice.index(x, y, z));
        for (Dir d : kAllDirs)
            if (tile.edge(d).cover != Cover::None) tile.edge(d).destructible = true;
        if (z > 0 && tile.hasFloor) tile.floorDestructible = true;
    }

    /* exception AFTER the pass (safe direction — removing destructibility):
     * the plinth top is solid concrete over solid mass, not a blowable slab */
    for (int y = 2; y <= 4; y++)
        for (int x = 20; x <= 22; x++)
            if (Tile* tile = world.tryAt(x, y, 1)) tile->floorDestructible = false;
}

}  // namespace xcom
