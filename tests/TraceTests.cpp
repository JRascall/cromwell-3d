/* TraceTests.cpp — the trace against a real tile world.
 *
 * WHAT THIS ADDS OVER xcom_collision_tests. That binary checks the engine's
 * geometry in isolation: given a box and a cell, when do they touch. This checks
 * the half that only exists once there are tiles — that a floor is where the
 * floor is, that a wall between two cells is reported once rather than twice or
 * not at all, that a window is a different layer from a wall and that the layer
 * table's rules therefore actually take effect, and that a swept box stops
 * further from a surface than a ray does by exactly its own half-extent.
 *
 * IT IS HEADLESS BECAUSE WorldTrace IS. Nothing in the trace names a raylib
 * type — the pickers that wrap it do, and they stay with the app — so a world
 * can be built in six lines here and swept without opening a window. That is the
 * whole reason the trace was put in game_core rather than beside the pickers.
 *
 * THE COORDINATE CONVENTION, since every number below depends on it: the
 * lattice's x is the world's x, the lattice's y is the world's Z, and the
 * lattice's z is a height index of kCellHeight (two thirds of a unit) each. So
 * cell (2, 3, 1) spans world x in [2,3], world z in [3,4] and world height in
 * [0.667, 1.333].
 */
#include "game/picking/WorldTrace.hpp"

#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

#include <cmath>
#include <cstdio>

using namespace game;
using cromwell::TraceFilter;
using cromwell::TraceHit;
using cromwell::TraceHitBuffer;
using cromwell::TraceShape;
using cromwell::Vec3;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

bool nearly(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

/* A flat floor across the whole ground storey, which is the world every test
 * below starts from. */
World groundWorld()
{
    World world;
    const Lattice& lattice = world.lattice();

    for (int y = 0; y < lattice.height(); ++y) {
        for (int x = 0; x < lattice.width(); ++x) {
            world.at(lattice.index(x, y, 0)).hasFloor = true;
        }
    }
    return world;
}

TraceFilter cursorFilter()
{
    return defaultLayerMatrix().filterFor(layer::kCursor);
}

/* ---- the ground -------------------------------------------------------- */

void testRayFindsTheFloorAtTheRightHeight()
{
    const World world = groundWorld();
    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 4.5f, 3.0f, 4.5f };
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();

    const auto hit = trace.single(params);
    CHECK(hit.has_value(), "a ray dropped onto the ground finds it");
    if (!hit) return;

    /* Cell z = 0's base is height 0, and a plain floor has no offset, so the
     * walk surface is exactly 0. Three metres up means three metres of fall. */
    CHECK(nearly(hit->distance, 3.0f), "at exactly the drop height");
    CHECK(nearly(hit->point.y, 0.0f), "with the contact on the surface");
    CHECK(nearly(hit->normal.y, 1.0f), "and the normal pointing up");
    CHECK(hit->layer == layer::kFloor, "reported as the floor layer");
    CHECK(hit->cellX == 4 && hit->cellY == 4 && hit->cellZ == 0, "in the cell under the ray");
}

void testBoxStopsHigherThanARayByItsHalfExtent()
{
    const World world = groundWorld();
    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 4.5f, 3.0f, 4.5f };
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();
    params.shape = TraceShape::box(Vec3{ 0.3f, 0.4f, 0.3f });

    const auto hit = trace.single(params);
    CHECK(hit.has_value(), "the box reaches the ground");
    if (!hit) return;

    /* THE TEST THAT CATCHES THE WHOLE ANISOTROPIC-SCALE MISTAKE. The walk runs
     * in lattice space, where height is divided by kCellHeight; if the shape's
     * extents were scaled on the way out as well as on the way in, this would be
     * 0.4 / kCellHeight = 0.6 metres short instead of 0.4. */
    CHECK(nearly(hit->distance, 2.6f), "it stops its half-height above the floor");
    CHECK(nearly(hit->end.y, 0.4f), "which puts its centre at 0.4");
    CHECK(nearly(hit->point.y, 0.0f), "while the contact is still on the surface");
}

void testTraceRespectsTheStoreyCeiling()
{
    World world = groundWorld();
    const Lattice& lattice = world.lattice();

    /* A floor on the storey above, directly under the ray. */
    world.at(lattice.index(4, 4, kCellsPerStorey)).hasFloor = true;

    const WorldTrace trace(world);
    WorldTrace::Params params;
    params.start = Vec3{ 4.5f, 8.0f, 4.5f };
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();

    const auto upper = trace.single(params);
    CHECK(upper.has_value() && upper->cellZ == kCellsPerStorey,
          "unrestricted, the ray stops on the upper floor");

    params.maxStorey = 0;
    const auto lower = trace.single(params);
    CHECK(lower.has_value() && lower->cellZ == 0,
          "cut to the ground storey, it passes through and lands below");
}

/* ---- walls ------------------------------------------------------------- */

/* Puts a full-cover face on the boundary between (x,y,z) and its neighbour in
 * `side`. Authored on one tile only, which is the case that proves the trace
 * reads the merged edge rather than the tile it happens to visit first. */
void buildWall(World& world, int x, int y, int z, Dir side, bool window = false)
{
    Edge& edge = world.at(world.lattice().index(x, y, z)).edge(side);
    edge.cover = Cover::Full;
    edge.window = window;
}

void testWallIsFoundOnceFromEitherSide()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East);   /* the face at x = 5 */

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.direction = Vec3{ 1.0f, 0.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();
    params.start = Vec3{ 1.5f, 0.3f, 4.5f };

    const auto eastward = trace.single(params);
    CHECK(eastward.has_value(), "a ray from the west meets the wall");
    if (eastward) {
        /* The slab is 4.5 cm either side of the plane at x = 5, so the near face
         * is at 4.955 and the ray starts at 1.5. */
        CHECK(nearly(eastward->distance, 3.455f), "on the near face of the slab");
        CHECK(nearly(eastward->normal.x, -1.0f), "with the normal facing the ray");
        CHECK(eastward->layer == layer::kWall, "as a wall");
    }

    /* From the other side. The face is authored on the west tile only, so this
     * only works if effectiveEdge is merging the two records — which is exactly
     * the thing a per-cell trace could get wrong by testing the tile it is
     * standing in rather than the boundary. */
    params.start = Vec3{ 8.5f, 0.3f, 4.5f };
    params.direction = Vec3{ -1.0f, 0.0f, 0.0f };

    const auto westward = trace.single(params);
    CHECK(westward.has_value(), "and a ray from the east meets the same wall");
    if (westward) {
        CHECK(nearly(westward->distance, 3.455f), "at the mirrored distance");
        CHECK(nearly(westward->normal.x, 1.0f), "with the normal facing back");
    }
}

void testMultiReportsAWallOnlyOnce()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East);

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 1.5f, 0.3f, 4.5f };
    params.direction = Vec3{ 1.0f, 0.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = TraceFilter::overlapAll();  /* report everything, stop at nothing */

    TraceHitBuffer<16> storage;
    cromwell::TraceHits hits = storage.view();
    trace.multi(params, hits);

    int walls = 0;
    for (const TraceHit& hit : hits) {
        if (hit.layer == layer::kWall) ++walls;
    }
    /* ONE FACE, ONE OWNER. A wall is stored on both adjoining tiles, and the
     * walk visits both — so a trace that tested all four faces of every cell
     * would return this twice. */
    CHECK(walls == 1, "the shared face is reported exactly once (saw %d)", walls);
}

void testWallIsFoundByARayThatLeavesTheCellInsideTheSlab()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East);   /* the face at x = 5 */

    const WorldTrace trace(world);

    /* THE CASE THAT BROKE THE DECAL TOOL, and it is about the WALK rather than
     * about walls.
     *
     * A wall slab STRADDLES the boundary it sits on — 4.5 cm either side of
     * x = 5 — so half of it lies in the cell to the west. The walk visits the
     * cells the ray crosses, and each face has ONE owner, so that western half
     * is only ever tested if the ray also reaches the OWNING cell.
     *
     * Usually it does. This ray does not: it is aimed so that it enters the
     * slab at x = 4.955 and crosses into the next cell in Z four hundredths of
     * a unit later, still short of x = 5. Every cell it visits from there on
     * belongs to a different boundary, so the wall it demonstrably passed
     * through is never tested and the ray sails on to the floor beyond.
     *
     * On screen that is a cursor that slides off a plain wall at particular
     * camera angles and lands on the ground behind it, taking the decal's
     * orientation with it — the normal comes back pointing UP. The window is
     * only 4.5 cm wide, which is exactly why it reads as intermittent. */
    WorldTrace::Params params;
    params.start = Vec3{ 4.0f, 0.3f, 4.9f };
    params.direction = Vec3{ 1.0f, 0.0f, 0.102f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();

    const auto hit = trace.single(params);
    CHECK(hit.has_value(), "the ray meets something");
    if (!hit) return;

    CHECK(hit->layer == layer::kWall, "and it is the wall it passed through, not the floor");
    CHECK(nearly(hit->point.x, 4.955f, 1.0e-2f), "on the near face of the slab");
    CHECK(nearly(hit->normal.x, -1.0f), "with the normal out of the wall, not up off the floor");
}

/* ---- layers ------------------------------------------------------------ */

void testAWindowIsNotAWall()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East, /*window=*/true);

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 1.5f, 0.3f, 4.5f };
    params.direction = Vec3{ 1.0f, 0.0f, 0.0f };
    params.maxDistance = 20.0f;

    /* THE POINT OF HAVING LAYERS AT ALL. The same geometry, three queries, three
     * different answers — and none of the three had to be told about windows. */
    params.filter = defaultLayerMatrix().filterFor(layer::kPaint);
    CHECK(!trace.single(params).has_value(), "a decal cannot be stuck to glass");

    params.filter = defaultLayerMatrix().filterFor(layer::kSight);
    CHECK(!trace.single(params).has_value(), "and you can see through it");

    params.filter = defaultLayerMatrix().filterFor(layer::kBody);
    const auto body = trace.single(params);
    CHECK(body.has_value() && body->layer == layer::kWindow,
          "but you cannot walk through it");
}

void testSightPassesGlassAndReportsIt()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East, /*window=*/true);
    buildWall(world, 8, 4, 0, Dir::East, /*window=*/false);

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 1.5f, 0.3f, 4.5f };
    params.direction = Vec3{ 1.0f, 0.0f, 0.0f };
    params.maxDistance = 30.0f;
    params.filter = defaultLayerMatrix().filterFor(layer::kShot);

    TraceHitBuffer<16> storage;
    cromwell::TraceHits hits = storage.view();
    trace.multi(params, hits);

    CHECK(hits.count() >= 2, "the shot reports the glass it passed and the wall it hit");
    if (hits.count() >= 2) {
        CHECK(hits[0].layer == layer::kWindow, "glass first");
        CHECK(hits[0].response == cromwell::Response::Overlap, "as something passed through");
        CHECK(hits[1].layer == layer::kWall, "then the wall");
        CHECK(hits[1].response == cromwell::Response::Block, "as the thing that stopped it");
    }

    const TraceHit* blocking = hits.blocking();
    CHECK(blocking != nullptr && blocking->layer == layer::kWall,
          "and the blocking hit is the wall, not the window in front of it");
}

/* ---- masses and ramps -------------------------------------------------- */

void testSolidMassIsHitOnItsTop()
{
    World world = groundWorld();
    const Lattice& lattice = world.lattice();
    world.at(lattice.index(4, 4, 0)).blocked = true;

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 4.5f, 5.0f, 4.5f };
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();

    const auto hit = trace.single(params);
    CHECK(hit.has_value(), "the mass is hit from above");
    if (!hit) return;

    CHECK(hit->layer == layer::kMass, "as a mass rather than as a floor");
    CHECK(nearly(hit->point.y, kCellHeight), "on top of the cell it fills");
    CHECK(nearly(hit->normal.y, 1.0f), "with an upward normal");
}

void testRampIsAnInclinedPlane()
{
    World world = groundWorld();
    const Lattice& lattice = world.lattice();

    Tile& tile = world.at(lattice.index(4, 4, 0));
    tile.hasFloor = true;
    tile.rampDir = Dir::North;        /* uphill toward +z */
    tile.rampBaseHeight = 0.0f;
    tile.rampRise = 0.5f;

    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 20.0f;
    params.filter = cursorFilter();

    /* Three quarters of the way up the tile, so the surface should be at
     * 0.75 * 0.5 = 0.375. A trace that treated a ramp as a flat slab would
     * report 0 here, and one that got the uphill direction backwards, 0.125. */
    params.start = Vec3{ 4.5f, 3.0f, 4.75f };

    const auto hit = trace.single(params);
    CHECK(hit.has_value(), "the ramp is hit");
    if (!hit) return;

    CHECK(hit->layer == layer::kRamp, "as a ramp");
    CHECK(nearly(hit->point.y, 0.375f), "at the height its incline gives (%.3f)",
          static_cast<double>(hit->point.y));
    CHECK(hit->normal.y > 0.8f && hit->normal.z < -0.3f,
          "with a normal tilted back down the slope");
}

/* ---- the shorthand ----------------------------------------------------- */

void testClearBetween()
{
    World world = groundWorld();
    buildWall(world, 4, 4, 0, Dir::East);

    const WorldTrace trace(world);
    const TraceFilter sight = defaultLayerMatrix().filterFor(layer::kSight);

    CHECK(!trace.clearBetween(Vec3{ 1.5f, 0.3f, 4.5f }, Vec3{ 8.5f, 0.3f, 4.5f }, sight),
          "the wall breaks the line");
    CHECK(trace.clearBetween(Vec3{ 1.5f, 0.3f, 6.5f }, Vec3{ 8.5f, 0.3f, 6.5f }, sight),
          "two rows over, the line is clear");
}

void testTraceOfZeroLengthDoesNothing()
{
    const World world = groundWorld();
    const WorldTrace trace(world);

    WorldTrace::Params params;
    params.start = Vec3{ 4.5f, 3.0f, 4.5f };
    params.direction = Vec3{ 0.0f, -1.0f, 0.0f };
    params.maxDistance = 0.0f;
    params.filter = cursorFilter();

    CHECK(!trace.single(params).has_value(), "a zero-length trace finds nothing");
}

}  // namespace

int main()
{
    testRayFindsTheFloorAtTheRightHeight();
    testBoxStopsHigherThanARayByItsHalfExtent();
    testTraceRespectsTheStoreyCeiling();

    testWallIsFoundOnceFromEitherSide();
    testMultiReportsAWallOnlyOnce();
    testWallIsFoundByARayThatLeavesTheCellInsideTheSlab();

    testAWindowIsNotAWall();
    testSightPassesGlassAndReportsIt();

    testSolidMassIsHitOnItsTop();
    testRampIsAnInclinedPlane();

    testClearBetween();
    testTraceOfZeroLengthDoesNothing();

    if (g_failures == 0) {
        std::printf("trace: all checks passed\n");
        return 0;
    }
    std::printf("trace: %d check(s) failed\n", g_failures);
    return 1;
}
