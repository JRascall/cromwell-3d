/* TileCoreTests.cpp — headless verification of the tile core.
 *
 *   1. lattice arithmetic: cells, storeys, height round-trips
 *   2. the demo map builds; walls occupy whole storeys, half cover one cell
 *   3. ramps: slope band enforced, flights chain at EXACT heights
 *   4. surface plane: linear across a ramp, exact at both ends
 *   5. reachability, 6. border loops, 7. line of sight, 8. units
 *
 * Output is kept identical to the C build's, so the two can be diffed
 * directly — the only intended difference is the ramp rejection diagnostics,
 * which now name MapAuthor::setRamp instead of xcSetRamp.
 */
#include "game/border/band/Band.hpp"
#include "game/border/band/BandExtractor.hpp"
#include "game/border/loop/LoopPolyliner.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/los/RayCaster.hpp"
#include "game/los/VisibilityComputer.hpp"
#include "game/movement/graph/InfantryMoveGraph.hpp"
#include "game/movement/search/PathReconstructor.hpp"
#include "game/movement/search/Pathfinder.hpp"
#include "game/query/BlockedMass.hpp"
#include "game/query/cover/LedgeCover.hpp"
#include "game/light/RoomPartition.hpp"
#include "game/query/Terrain.hpp"
#include "game/render/scene/CutawayView.hpp"
#include "game/units/roster/DemoRosterFactory.hpp"
#include "game/units/roster/OccupancyMaskBuilder.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/units/UnitFactory.hpp"
#include "game/world/authoring/DemoMapFactory.hpp"
#include "game/world/authoring/MapAuthor.hpp"
#include "game/world/World.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace game;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

World      g_world;
ReachField g_reach;

/* --------------------------------------------------------------- test 1 */
void testLattice()
{
    std::printf("== lattice ==\n");
    const Lattice& lattice = g_world.lattice();

    CHECK(lattice.depth() == 9, "GRID_Z = %d, expected 9", lattice.depth());
    CHECK(std::fabs(kStoreyHeight - 2.0f) < 1e-6f, "STOREY_H = %f", (double)kStoreyHeight);
    CHECK(std::fabs(kCellHeight * 3.0f - kStoreyHeight) < 1e-6f, "three cells != one storey");

    /* storey base heights must land exactly on cell boundaries */
    for (int s = 0; s < lattice.storeys(); s++) {
        float offset = 0.0f;
        const int z = lattice.cellOfHeight(Lattice::storeyBaseHeight(s), &offset);
        CHECK(z == Lattice::storeyBaseZ(s), "storey %d -> cell %d, expected %d",
              s, z, Lattice::storeyBaseZ(s));
        CHECK(std::fabs(offset) < 1e-6f, "storey %d base has offset %f", s, (double)offset);
        CHECK(Lattice::storeyOfZ(z) == s, "storeyOfZ(%d) != %d", z, s);
    }

    /* a sunken road clamps to cell 0 and keeps a negative offset */
    float offset = 0.0f;
    const int z = lattice.cellOfHeight(-0.15f, &offset);
    CHECK(z == 0 && std::fabs(offset + 0.15f) < 1e-6f, "road -> cell %d off %f", z, (double)offset);

    std::printf("   CELL_H=%.4f STOREY_H=%.1f GRID_Z=%d\n",
                (double)kCellHeight, (double)kStoreyHeight, lattice.depth());
}

/* --------------------------------------------------------------- test 2 */
void testMapStructure()
{
    std::printf("== map structure ==\n");
    DemoMapFactory::build(g_world);

    const Lattice& lattice = g_world.lattice();
    const Terrain     terrain(g_world);
    const BlockedMass mass(g_world);
    const LedgeCover  ledges(g_world);

    /* a FULL wall occupies all three cells of its storey */
    int full = 0;
    for (int i = 0; i < kCellsPerStorey; i++)
        if (g_world.at(lattice.index(5, 12, Lattice::storeyBaseZ(0) + i)).edge(Dir::South).cover
            == Cover::Full) full++;
    CHECK(full == 3, "full wall covers %d/3 cells", full);

    /* HALF cover occupies exactly one — XCOM low cover is 64uu = one cell */
    int half = 0;
    for (int i = 0; i < kCellsPerStorey; i++)
        if (g_world.at(lattice.index(5, 10, Lattice::storeyBaseZ(1) + i)).edge(Dir::South).cover
            == Cover::Half) half++;
    CHECK(half == 1, "half railing covers %d/1 cells", half);

    /* floors landed in the expected cells */
    struct Level { const char* name; float height; int cell; };
    const Level levels[3] = {
        { "ground",  0.0f,                0 },
        { "storey1", kStoreyHeight,       3 },
        { "roof",    2.0f * kStoreyHeight, 6 },
    };
    for (const Level& level : levels) {
        const int cz = lattice.cellOfHeight(level.height);
        CHECK(cz == level.cell, "%s -> cell %d, expected %d", level.name, cz, level.cell);
        int n = 0;
        for (int y = 0; y < lattice.height(); y++)
            for (int x = 0; x < lattice.width(); x++)
                if (g_world.at(lattice.index(x, y, cz)).hasFloor) n++;
        std::printf("   %-8s h=%.1f -> cell %d, %d floor tiles\n",
                    level.name, (double)level.height, cz, n);
        CHECK(n > 0, "%s has no floor tiles", level.name);
    }

    /* PLINTH: a MEDIUM obstacle. Its top is one z cell up — 64uu, XCOM's
     * Cover_LowCoverHeight — so it is chest-high on a soldier rather than
     * taller than one, and it grants half cover without being a wall. */
    CHECK(g_world.at(lattice.index(21, 3, 0)).blocked, "plinth base not solid");
    CHECK(g_world.at(lattice.index(21, 3, 1)).hasFloor, "plinth top missing");

    const float plinthHeight = terrain.centerHeight(21, 3, 1);
    CHECK(std::fabs(plinthHeight - kCellHeight) < 1e-6f,
          "plinth top at %f, expected one cell (%f)", (double)plinthHeight, (double)kCellHeight);

    const std::optional<float> plinthTop = mass.topHeight(21, 3, 0);
    CHECK(plinthTop && std::fabs(*plinthTop - kCellHeight) < 1e-6f,
          "plinth blockedTop %f", (double)(plinthTop ? *plinthTop : 0.0f));

    /* it must stay vaultable, and read as HALF cover from the street */
    CHECK(plinthHeight <= kMantleMax, "plinth is too tall to mantle");
    CHECK(plinthHeight >= kLedgeHalf && plinthHeight < kLedgeFull,
          "plinth no longer reads as half cover");
    CHECK(ledges.at(19, 3, 0, Dir::East) == Cover::Half,
          "plinth does not grant half cover to the tile at its base");

    /* MICRO-RELIEF IS REAL HEIGHT, and it is GRID ALIGNED: the kerb face sits
     * exactly on a tile boundary, which is the same line the border ribbon
     * runs along. That alignment is what lets the ribbon cap the kerb rather
     * than slope across a tile face to reach it. */
    CHECK(std::fabs(terrain.centerHeight(10, 6, 0) + 0.15f) < 1e-6f, "road height wrong");
    CHECK(std::fabs(terrain.centerHeight(4, 2, 0) - 0.05f) < 1e-6f, "lawn height wrong");
    CHECK(g_world.at(lattice.index(10, 6, 0)).artTag == Art::Road,  "road not tagged");
    CHECK(g_world.at(lattice.index(4,  2, 0)).artTag == Art::Grass, "lawn not tagged");

    /* Art may only ever go DOWN from the tile plane — that is what lets a
     * border lifted 4uu above the plane clear every surface by construction,
     * with no tracing and no per-case tuning. */
    int rises = 0;
    for (int i = 0; i < lattice.cellCount(); i++)
        if (g_world.at(i).artDrop < 0.0f) rises++;
    CHECK(rises == 0, "%d tile(s) have art ABOVE the tile plane", rises);
}

/* --------------------------------------------------------------- test 3 */
void testRamps()
{
    std::printf("== ramps ==\n");
    const Lattice& lattice = g_world.lattice();

    int n = 0;
    float steepest = 0.0f;
    for (int i = 0; i < lattice.cellCount(); i++) {
        const Tile& tile = g_world.at(i);
        if (!tile.isRamp()) continue;
        n++;
        if (tile.rampRise > steepest) steepest = tile.rampRise;
        CHECK(tile.rampRise <= kRampMaxRise + 1e-6f, "ramp %d over the 45 deg cap", i);
        CHECK(tile.rampRise >= kRampMinRise - 1e-6f, "ramp %d under the 10 deg floor", i);
    }
    std::printf("   %d ramp tiles, steepest rise %.3f (cap %.3f)\n",
                n, (double)steepest, (double)kRampMaxRise);
    CHECK(n == 10, "expected 10 ramp tiles, got %d", n);

    /* the validator must REJECT out-of-band slopes rather than clamp them */
    std::printf("   (two rejection diagnostics below are expected)\n");
    MapAuthor author(g_world);
    CHECK(author.setRamp(0, 0, Dir::North, 0.0f, kStoreyHeight) == -1, "63 deg ramp was accepted");
    CHECK(author.setRamp(0, 0, Dir::North, 0.0f, 0.05f) == -1, "3 deg ramp was accepted");
    CHECK(!g_world.at(lattice.index(0, 0, 0)).isRamp(), "rejected ramp still mutated the tile");

    /* flights chain by ABSOLUTE height, so this is exact equality */
    const Tile& first  = g_world.at(lattice.index(5, 13, 0));
    const Tile& second = g_world.at(lattice.index(5, 14, 1));
    CHECK(first.isRamp() && second.isRamp(), "staircase A flights not where expected");
    if (first.isRamp() && second.isRamp()) {
        const float gap = std::fabs(first.rampTopHeight() - second.rampBaseHeight);
        CHECK(gap == 0.0f, "flight chain gap %g (must be exact)", (double)gap);
        const float landing = std::fabs(second.rampTopHeight() - kStoreyHeight);
        CHECK(landing == 0.0f, "flight top misses storey 1 by %g", (double)landing);
        std::printf("   stair A: %.2f -> %.2f -> %.2f  (chain gap %g)\n",
                    (double)first.rampBaseHeight, (double)second.rampBaseHeight,
                    (double)second.rampTopHeight(), (double)gap);
    }
}

/* --------------------------------------------------------------- test 4 */
void testSurfacePlane()
{
    std::printf("== surface plane ==\n");
    const Lattice& lattice = g_world.lattice();
    const Terrain terrain(g_world);

    /* A ramp is ONE inclined plane, so sampling must be linear in the uphill
     * axis and exact at both edges. This is what lets the border ribbon cross
     * a flight as a single straight chord with zero height error. */
    const Tile& tile = g_world.at(lattice.index(5, 13, 0));
    CHECK(tile.isRamp(), "expected a ramp at (5,13,0)");
    if (!tile.isRamp()) return;

    const float low  = terrain.surfaceHeightAt(5, 13, 0, 5.5f, 13.0f);
    const float high = terrain.surfaceHeightAt(5, 13, 0, 5.5f, 14.0f);
    CHECK(std::fabs(low - tile.rampBaseHeight) < 1e-6f,
          "low edge %f != baseH %f", (double)low, (double)tile.rampBaseHeight);
    CHECK(std::fabs(high - tile.rampTopHeight()) < 1e-6f, "high edge %f", (double)high);

    float worst = 0.0f;
    for (int k = 0; k <= 20; k++) {
        const float u = (float)k / 20.0f;
        const float h = terrain.surfaceHeightAt(5, 13, 0, 5.5f, 13.0f + u);
        const float expected = tile.rampBaseHeight + u * tile.rampRise;
        const float error = std::fabs(h - expected);
        if (error > worst) worst = error;
    }
    CHECK(worst < 1e-6f, "ramp plane deviates by %g", (double)worst);
    std::printf("   ramp sampling is linear, max deviation %g\n", (double)worst);

    /* flat cells: base + micro-relief, independent of where in the tile */
    const float a = terrain.surfaceHeightAt(10, 6, 0, 10.1f, 6.9f);
    const float b = terrain.surfaceHeightAt(10, 6, 0, 10.9f, 6.1f);
    CHECK(std::fabs(a - b) < 1e-6f, "flat cell is not flat (%f vs %f)", (double)a, (double)b);
}

/* --------------------------------------------------------------- test 5 */
void testReach()
{
    std::printf("== reachability ==\n");
    const Lattice& lattice = g_world.lattice();

    /* (7,11,0) is outside the building's doorway, on the ground */
    const InfantryMoveGraph graph(g_world);
    const Pathfinder pathfinder(lattice);
    pathfinder.search(graph, { 7, 11, 0 }, 40.0f, nullptr, g_reach);

    std::vector<int> perStorey(static_cast<std::size_t>(lattice.storeys()), 0);
    int total = 0;
    for (int i = 0; i < lattice.cellCount(); i++) {
        if (!g_reach.isReachable(i)) continue;
        total++;
        perStorey[static_cast<std::size_t>(Lattice::storeyOfZ(lattice.cellAt(i).z))]++;
    }
    std::printf("   cost<=40: %d cells | per storey %d / %d / %d\n",
                total, perStorey[0], perStorey[1], perStorey[2]);

    /* the staircases must actually connect the storeys */
    for (int s = 0; s < lattice.storeys(); s++)
        CHECK(perStorey[static_cast<std::size_t>(s)] > 0,
              "storey %d unreachable - stairs are broken", s);
    CHECK(total > 500, "only %d cells reachable", total);

    /* a path back out must reconstruct and start where we asked */
    const int start = lattice.index(7, 11, 0);
    int destination = -1;
    for (int i = 0; i < lattice.cellCount(); i++) {
        if (Lattice::storeyOfZ(lattice.cellAt(i).z) == 2 && g_reach.isReachable(i)) {
            destination = i;
            break;
        }
    }
    CHECK(destination >= 0, "no roof cell reachable");
    if (destination >= 0) {
        const std::vector<int> path = PathReconstructor::reconstruct(g_reach, destination, start);
        CHECK(path.size() > 1, "path to the roof did not reconstruct");
        CHECK(path.size() > 1 && path.front() == start, "path does not start at the start cell");
        std::printf("   path to roof: %d steps\n", (int)path.size());
    }
}

/* --------------------------------------------------------------- test 6 */
void testBorders()
{
    std::printf("== border loops ==\n");
    const Lattice& lattice = g_world.lattice();
    const Terrain  terrain(g_world);

    BandExtractor extractor(g_world);
    LoopPolyliner polyliner(g_world);
    LoopSet       loops;
    Band          band(lattice.cellCount());
    std::vector<BorderPoint> polyline;

    const float caps[2] = { 6.0f, 12.0f };
    for (float cap : caps) {
        band.reset(lattice.cellCount());
        int bandCount = 0;
        for (int i = 0; i < lattice.cellCount(); i++)
            if (g_reach.cost(i) <= cap) { band.mark(i); bandCount++; }

        /* How many cells sit right ON the cap? Costs accumulate kDiagonalCost
         * (1.4, not representable in binary), so a float32 sum drifts from
         * the float64 one by ~1 ULP per diagonal. Cells landing exactly on
         * the cap can therefore fall either side of it between builds — the
         * band differs at its rim, not in its shape. */
        int onRim = 0;
        for (int i = 0; i < lattice.cellCount(); i++)
            if (g_reach.isReachable(i) && std::fabs(g_reach.cost(i) - cap) < 0.02f) onRim++;

        extractor.extract(band, loops);

        int open = 0;
        for (const Loop& loop : loops.loops()) if (!loop.closed) open++;
        std::printf("   cap %.0f: %d cells (%d exactly on the rim) -> %d loops, %d edges | closed %d/%d\n",
                    (double)cap, bandCount, onRim, loops.loopCount(), loops.edgeCount(),
                    loops.loopCount() - open, loops.loopCount());
        CHECK(loops.loopCount() > 0, "no loops for cap %.0f", (double)cap);

        /* the deterministic corner rule makes every walk a cycle */
        if (open) {
            static const char* kDirNames[4] = { "N", "E", "S", "W" };
            int shown = 0;
            for (int l = 0; l < loops.loopCount() && shown < 5; l++) {
                const Loop& loop = loops.loop(l);
                if (loop.closed) continue;
                const EdgeId firstEdge = loops.edgeAt(loop, 0);
                const EdgeId lastEdge  = loops.edgeAt(loop, loop.count - 1);
                const Cell a = lattice.cellAt(firstEdge.cell());
                const Cell b = lattice.cellAt(lastEdge.cell());
                std::printf("      open chain %d: %d edges  start (%d,%d,z%d).%s  end (%d,%d,z%d).%s\n",
                            l, loop.count,
                            a.x, a.y, a.z, kDirNames[toIndex(firstEdge.dir())],
                            b.x, b.y, b.z, kDirNames[toIndex(lastEdge.dir())]);
                shown++;
            }
        }
        CHECK(open == 0, "%d OPEN strips at cap %.0f", open, (double)cap);

        /* the successor relation must be a permutation: no edge twice */
        std::vector<unsigned char> hit(
            static_cast<std::size_t>(lattice.cellCount() * kDirCount), 0);
        int duplicates = 0;
        for (EdgeId edge : loops.edges()) {
            if (hit[static_cast<std::size_t>(edge.raw())]) duplicates++;
            else hit[static_cast<std::size_t>(edge.raw())] = 1;
        }
        CHECK(duplicates == 0, "%d duplicate edges at cap %.0f", duplicates, (double)cap);

        /* Every boundary edge in the band must appear in some loop. The
         * predicate here mirrors BandConnectivity: MEMBERSHIP at a compatible
         * height, walls ignored. */
        int missing = 0, shown = 0;
        static const char* kDirNames[4] = { "N", "E", "S", "W" };
        for (int i = 0; i < lattice.cellCount(); i++) {
            if (!band.contains(i)) continue;
            const Cell cell = lattice.cellAt(i);

            for (Dir d : kAllDirs) {
                const int nx = cell.x + dx(d);
                const int ny = cell.y + dy(d);
                const float midX = (float)cell.x + 0.5f + (float)dx(d) * 0.5f;
                const float midY = (float)cell.y + 0.5f + (float)dy(d) * 0.5f;
                const float myHeight = terrain.surfaceHeightAt(cell, midX, midY);

                bool present = false;
                if (lattice.inBounds(nx, ny))
                    for (int zz = 0; zz < lattice.depth(); zz++) {
                        const int neighbour = lattice.index(nx, ny, zz);
                        if (!band.contains(neighbour)) continue;
                        if (std::fabs(terrain.surfaceHeightAt(nx, ny, zz, midX, midY) - myHeight)
                            <= kWalkStep) present = true;
                    }
                if (present || hit[static_cast<std::size_t>(EdgeId(i, d).raw())]) continue;
                missing++;
                if (shown++ < 6)
                    std::printf("      unwalked (%d,%d,z%d).%s  ramp=%d  h=%.3f\n",
                                cell.x, cell.y, cell.z, kDirNames[toIndex(d)],
                                (int)g_world.at(i).isRamp(), (double)myHeight);
            }
        }
        CHECK(missing == 0, "%d boundary edges never walked at cap %.0f", missing, (double)cap);

        /* Height invariants, and they differ by owner:
         *  - on a RAMP the vertex must sit EXACTLY on the inclined plane. That
         *    is what lets a flight be crossed by one straight chord.
         *  - on FLAT ground the vertex may be CAPPED up to the tile across the
         *    boundary, because the ribbon straddles that boundary and would
         *    otherwise be buried under a kerb's slab. Bounded to one step, so
         *    a wall can never drag the line up with it. */
        float worstRamp = 0.0f, worstCap = 0.0f, worstBelow = 0.0f;
        int vertexCount = 0, rampVertices = 0, cappedVertices = 0;
        for (int l = 0; l < loops.loopCount(); l++) {
            polyliner.build(loops, l, 0.0f, 0.11f, false, polyline);
            CHECK(!polyline.empty(), "loop %d produced no polyline", l);

            for (const BorderPoint& point : polyline) {
                vertexCount++;
                const Cell owner = lattice.cellAt(point.owner);
                const float ownHeight = terrain.surfaceHeightAt(owner, point.x, point.y);
                const float delta = point.height - ownHeight;

                if (g_world.at(point.owner).isRamp()) {
                    rampVertices++;
                    if (std::fabs(delta) > worstRamp) worstRamp = std::fabs(delta);
                } else {
                    if (delta > 1e-5f) {
                        cappedVertices++;
                        if (delta > worstCap) worstCap = delta;
                    }
                    if (-delta > worstBelow) worstBelow = -delta;
                }
            }
        }
        std::printf("   polyline verts %d (%d ramp, %d capped) | ramp err %.3e | cap %.3f\n",
                    vertexCount, rampVertices, cappedVertices,
                    (double)worstRamp, (double)worstCap);
        CHECK(worstRamp < 1e-5f, "ramp vertex off the plane by %g", (double)worstRamp);
        CHECK(worstBelow < 1e-5f, "a vertex sank BELOW its own surface by %g", (double)worstBelow);
        CHECK(worstCap <= kWalkStep + 1e-5f,
              "cap lifted a vertex %g, past one step - a wall is dragging the line",
              (double)worstCap);
    }
}

/* ---------------------------------------------------------- test 6b ----
 * SUPPRESSION, checked against the unmasked walk rather than against a second
 * implementation of it. The rings nest, so the outer band's boundary coincides
 * with the inner one's wherever geometry rather than budget is what stops the
 * unit — and two ribbons on one grid line is the amber one hiding the blue.
 *
 * Two invariants, and between them they pin it down: nothing survives that
 * blue already draws, and nothing is lost that blue does not. The second is
 * the one that catches a rotation or run-splitting mistake, which would
 * otherwise show up as a ribbon quietly missing a few edges. */
void testSuppressedBorders()
{
    std::printf("== border suppression ==\n");
    const Lattice& lattice = g_world.lattice();

    BandExtractor extractor(g_world);
    LoopPolyliner polyliner(g_world);
    Band inner(lattice.cellCount()), outer(lattice.cellCount());
    std::vector<BorderPoint> polyline;

    for (int i = 0; i < lattice.cellCount(); i++) {
        if (g_reach.cost(i) <= 6.0f)  inner.mark(i);
        if (g_reach.cost(i) <= 12.0f) outer.mark(i);
    }

    LoopSet full, masked;
    extractor.extract(outer, full);
    extractor.extract(outer, &inner, masked);

    std::vector<unsigned char> inFull(
        static_cast<std::size_t>(lattice.cellCount() * kDirCount), 0);
    for (EdgeId edge : full.edges()) inFull[static_cast<std::size_t>(edge.raw())] = 1;

    /* what survived: in the unmasked walk, and not on a cell blue covers */
    int shared = 0, foreign = 0, duplicates = 0;
    std::vector<unsigned char> seen(
        static_cast<std::size_t>(lattice.cellCount() * kDirCount), 0);
    for (EdgeId edge : masked.edges()) {
        if (inner.contains(edge.cell())) shared++;
        if (!inFull[static_cast<std::size_t>(edge.raw())]) foreign++;
        if (seen[static_cast<std::size_t>(edge.raw())]) duplicates++;
        else seen[static_cast<std::size_t>(edge.raw())] = 1;
    }

    /* what was dropped: every one of them, and only them, on a blue cell */
    int lost = 0;
    for (EdgeId edge : full.edges())
        if (!inner.contains(edge.cell()) && !seen[static_cast<std::size_t>(edge.raw())]) lost++;

    std::printf("   outer %d edges -> %d kept in %d runs (%d dropped as shared)\n",
                full.edgeCount(), masked.edgeCount(), masked.loopCount(),
                full.edgeCount() - masked.edgeCount());

    CHECK(shared == 0, "%d suppressed edges survived - amber is still over blue", shared);
    CHECK(foreign == 0, "%d edges appeared that the unmasked walk never found", foreign);
    CHECK(duplicates == 0, "%d edges emitted twice", duplicates);
    CHECK(lost == 0, "%d unshared edges were dropped - the amber ring has holes", lost);
    CHECK(masked.edgeCount() > 0, "suppression removed the whole outer ring");

    /* Every run must still make a polyline. An open run of a single edge is
     * legitimate — one grid line of amber standing past the blue — and the
     * count<2 guard used to throw it away. */
    int runs = 0, closedRuns = 0;
    for (int l = 0; l < masked.loopCount(); l++) {
        const Loop& loop = masked.loop(l);
        runs++;
        if (loop.closed) closedRuns++;

        polyliner.build(masked, l, 0.0f, 0.11f, false, polyline);
        CHECK(polyline.size() >= 2, "run %d of %d edges produced %d points",
              l, loop.count, (int)polyline.size());
    }
    std::printf("   %d runs (%d still closed - loops with nothing shared)\n", runs, closedRuns);
}

/* --------------------------------------------------------------- test 7 */
void testLos()
{
    std::printf("== line of sight ==\n");
    const Lattice& lattice = g_world.lattice();

    VisibilityField visibility;
    VisibilityComputer(g_world).compute({ 7, 10, 0 }, visibility);

    int seen = 0, peek = 0;
    for (int i = 0; i < lattice.cellCount(); i++) {
        if (visibility.at(i) == Visibility::Direct) seen++;
        else if (visibility.at(i) == Visibility::PeekOnly) { seen++; peek++; }
    }
    std::printf("   from (7,10,0): %d cells visible (%d peek-only)\n", seen, peek);
    CHECK(seen > 100, "only %d cells visible - LOS looks broken", seen);

    /* you can see your own tile, and not through a full wall into the
     * building's far corner at ground level */
    CHECK(visibility.at(lattice.index(7, 10, 0)) != Visibility::None, "cannot see own tile");

    RayCaster::Hit hit;
    const bool clear = RayCaster(g_world).cast(7.5f, 10.5f, kEyeHeight,
                                               5.5f, 18.5f, kEyeHeight, &hit);
    CHECK(!clear, "ray reached deep inside the building through a wall");
}

/* --------------------------------------------------------------- test 8 */
void testUnits()
{
    std::printf("== units ==\n");
    const Lattice& lattice = g_world.lattice();

    UnitRoster roster;
    DemoRosterFactory::build(roster);
    CHECK(roster.size() == 4, "expected 4 units, got %d", roster.size());

    Unit& soldier = roster.at(0);
    Unit& tank    = roster.at(1);
    CHECK(tank.footprint().tileCount() == 4,
          "tank footprint is %d tiles", tank.footprint().tileCount());

    /* the tank occupies all four of its anchor's tiles */
    for (const Cell& cell : tank.footprint().cellsAt(tank.position()))
        CHECK(roster.occupantAt(cell) == &tank, "tank missing from (%d,%d)", cell.x, cell.y);

    /* 1x1 units are transparent to sight; only the hull blocks */
    CHECK(roster.lineOfSightBlockerAt(soldier.position()) == nullptr, "soldier blocks LOS");
    CHECK(roster.lineOfSightBlockerAt(tank.position()) == &tank, "tank does not block LOS");

    /* enemies are walls to the mover; friendlies are not */
    BlockedMask mask;
    OccupancyMaskBuilder::build(roster, &soldier, lattice, mask);
    CHECK(mask.isBlocked(lattice.index(2, 16, 0)), "enemy cell not blocked");
    CHECK(!mask.isBlocked(lattice.index(tank.position())), "friendly tank blocks the soldier");

    /* enemy occupancy must actually shrink the reachable set */
    const InfantryMoveGraph graph(g_world);
    const Pathfinder pathfinder(lattice);
    ReachField blockedReach;
    pathfinder.search(graph, soldier.position(), 40.0f, &mask, blockedReach);

    int reachedWithBlocking = 0;
    for (int i = 0; i < lattice.cellCount(); i++)
        if (blockedReach.isReachable(i)) reachedWithBlocking++;

    std::printf("   reach from the soldier with enemies as walls: %d cells\n", reachedWithBlocking);
    CHECK(reachedWithBlocking > 0, "occupancy-aware search reached nothing");
    CHECK(!blockedReach.isReachable(lattice.index(2, 16, 0)), "walked onto an enemy");

    /* hull blocking: a ray at hull height across the tank is stopped, and the
     * same ray with the tank as the shooter (ignored) is not */
    const float ax = (float)tank.position().x - 1.5f;
    const float ay = (float)tank.position().y + 1.0f;
    const float bx = (float)tank.position().x + 3.5f;
    const float by = (float)tank.position().y + 1.0f;
    const float h  = tank.baseHeight(g_world) + 0.4f;   /* below hull top */

    const bool terrainClear = RayCaster(g_world).cast(ax, ay, h, bx, by, h);
    const bool hullClear    = RayCaster(g_world, roster, nullptr).cast(ax, ay, h, bx, by, h);
    const bool ownClear     = RayCaster(g_world, roster, &tank).cast(ax, ay, h, bx, by, h);

    std::printf("   ray across the tank: terrain %s | hull %s | own hull ignored %s\n",
                terrainClear ? "clear" : "blocked",
                hullClear    ? "clear" : "blocked",
                ownClear     ? "clear" : "blocked");
    CHECK(terrainClear, "terrain alone already blocks the test ray - pick another lane");
    CHECK(!hullClear, "tank hull did not block sight");
    CHECK(ownClear, "the tank's own hull blocked its own view");
}

/* --------------------------------------------------------------- test 9 */
/* THE OCCUPANCY INDEX MUST AGREE WITH THE SCAN IT REPLACED, cell for cell,
 * after every kind of edit. An index is a second copy of something already
 * known, so the only interesting question about it is whether it drifts — and
 * drift shows up as a unit that is invisible to sight or standing in a wall,
 * which is a bug nobody traces back to a cache.
 *
 * So this holds two rosters with identical contents, one bound and one not,
 * and compares every cell after each edit. The unbound one still runs the
 * original walk-every-unit scan, which makes it the reference. */
void testOccupancyIndex()
{
    std::printf("== occupancy index ==\n");
    const Lattice& lattice = g_world.lattice();

    UnitRoster indexed, reference;
    indexed.bindLattice(lattice);          /* reference is deliberately unbound */

    const auto populate = [](UnitRoster& roster) {
        roster.add(makeSoldier(Cell{ 11, 10, 0 }, Team::Player));
        roster.add(makeVehicle(Cell{ 16,  4, 0 }, Team::Player));
        roster.add(makeSoldier(Cell{  2, 16, 0 }, Team::Enemy));
        roster.add(makeSoldier(Cell{  6, 18, Lattice::storeyBaseZ(2) }, Team::Enemy));
    };
    populate(indexed);
    populate(reference);

    /* Compares every cell, and reports the FIRST disagreement rather than one
     * per cell — a drifted index disagrees about thousands at once. */
    const auto agree = [&](const char* stage) {
        int mismatches = 0;
        Cell first{};
        for (int i = 0; i < lattice.cellCount(); i++) {
            const Cell  cell = lattice.cellAt(i);
            const Unit* a = indexed.occupantAt(cell);
            const Unit* b = reference.occupantAt(cell);

            /* Different rosters, so compare by index rather than by pointer. */
            const int ai = a ? indexed.indexOf(a) : -1;
            const int bi = b ? reference.indexOf(b) : -1;
            if (ai != bi) {
                if (!mismatches) first = cell;
                mismatches++;
            }
        }
        CHECK(mismatches == 0,
              "%s: index disagrees with the scan in %d cells, first (%d,%d,%d)",
              stage, mismatches, first.x, first.y, first.z);
        if (!mismatches) std::printf("   %-22s agrees on all %d cells\n",
                                     stage, lattice.cellCount());
    };

    agree("after spawn");

    /* A move. The tank is 2x2, so this also covers a footprint whose old and
     * new positions OVERLAP — the case where a careless (erase then write)
     * blanks cells the body is keeping. */
    indexed.at(1).setPosition(Cell{ 17, 4, 0 });
    reference.at(1).setPosition(Cell{ 17, 4, 0 });
    agree("after overlapping move");

    indexed.at(0).setPosition(Cell{ 12, 12, 0 });
    reference.at(0).setPosition(Cell{ 12, 12, 0 });
    agree("after disjoint move");

    /* A death. The body stays in the roster but stops occupying. */
    indexed.at(2).kill();
    reference.at(2).kill();
    agree("after a death");

    /* Moving onto the cell a dead body is lying in must be legal and visible. */
    indexed.at(0).setPosition(Cell{ 2, 16, 0 });
    reference.at(0).setPosition(Cell{ 2, 16, 0 });
    agree("after moving onto a corpse");

    /* `exclude` still works: a body never blocks itself. */
    const Unit& tank = indexed.at(1);
    CHECK(indexed.occupantAt(tank.position(), &tank) == nullptr,
          "a body blocked itself through the index");
}

/* -------------------------------------------------------------- test 10 */
/* THE OCCLUSION SUMMARY MUST SAY WHAT THE TILES SAY. RayCaster reads bits
 * instead of Tiles now, so a wrong bit is a wall that shoots through or a
 * window that does not — and it would look like a map authoring bug, not a
 * cache bug, which is the expensive kind of wrong.
 *
 * Checked against the tiles directly rather than against a second ray caster:
 * comparing two implementations only tells you they agree, and keeping a
 * reference copy of the walk is the "two implementations to keep in step"
 * problem the escape hatch exists to avoid. This asks whether the DERIVED data
 * matches its source, which is the only thing that can drift.
 *
 * It also re-checks after a demolition, because the grid is rebuilt on
 * invalidation and a rebuild that does not happen is the whole risk. */
void testOcclusionGrid()
{
    std::printf("== occlusion summary ==\n");

    const auto verify = [](const World& world, const char* stage) {
        const Lattice&       lattice = world.lattice();
        const OcclusionGrid& grid    = world.occlusion();

        int mismatches = 0;
        for (int z = 0; z < lattice.depth(); z++)
        for (int y = 0; y < lattice.height(); y++)
        for (int x = 0; x < lattice.width(); x++) {
            const int           index = lattice.index(x, y, z);
            const Tile&         tile  = world.at(index);
            const std::uint16_t word  = grid.at(index);

            /* every face, cover grade and glass */
            for (Dir d : kAllDirs) {
                const Edge edge = world.effectiveEdge(x, y, z, d);
                if (occ::coverOf(word, toIndex(d)) != static_cast<int>(edge.cover)) mismatches++;
                if (occ::hasWindow(word, toIndex(d)) != edge.window) mismatches++;
            }

            const bool ramp        = tile.isRamp();
            const bool offsetFloor = std::fabs(tile.floorOffset) >= 1e-6f;

            const bool slab = tile.hasFloor && !ramp && !offsetFloor;
            if (((word & occ::kSlab) != 0) != slab) mismatches++;
            if (((word & occ::kCanopy) != 0) != tile.canopy) mismatches++;
            if (((word & occ::kBlocked) != 0) != tile.blocked) mismatches++;

            /* the escape hatch must cover everything the fast path cannot
             * decide on its own */
            const bool needsTile = tile.blocked || ramp
                                || (tile.hasFloor && !ramp && offsetFloor);
            if (((word & occ::kNeedsTile) != 0) != needsTile) mismatches++;
        }

        CHECK(mismatches == 0, "%s: %d disagreements with the tiles", stage, mismatches);
        if (!mismatches)
            std::printf("   %-20s matches tiles across all %d cells\n",
                        stage, lattice.cellCount());
    };

    World world;
    DemoMapFactory::build(world);
    verify(world, "as authored");

    /* Blow a hole, exactly as the bake benchmark's demolition does: clearing
     * destructible cover and floors must invalidate and rebuild the summary. */
    const Lattice& lattice = world.lattice();
    int edits = 0;
    for (int y = 10; y <= 14; y++)
    for (int x = 9; x <= 13; x++)
    for (int i = 0; i < kCellsPerStorey; i++) {
        if (!lattice.isValid(x, y, i)) continue;
        Tile& tile = world.at(lattice.index(x, y, i));
        for (Dir d : kAllDirs) {
            Edge& edge = tile.edge(d);
            if (edge.cover != Cover::None && edge.destructible) { edge = Edge{}; edits++; }
        }
        if (tile.blocked && tile.blockedDestructible) { tile.blocked = false; edits++; }
        if (tile.hasFloor && tile.floorDestructible)  { tile.hasFloor = false; edits++; }
    }
    std::printf("   demolished %d tile properties\n", edits);
    CHECK(edits > 0, "the demolition changed nothing - pick another site");
    verify(world, "after demolition");
}

}  // namespace

/* THE CUTAWAY, AND SPECIFICALLY THE PART THAT PROTECTS THE LIGHTING.
 *
 * The bug this guards against has happened once already: the sun's depth pass
 * read the player's storey cut, so hiding a floor deleted the roof from the
 * shadow map and the room below jumped to full sunlight. The facing cut is a
 * second, worse version of the same hazard because it re-decides every time
 * the camera turns.
 *
 * The defence is that a DEFAULT CutawayView cuts nothing, so the sun and the
 * probes are correct by construction rather than by a caller remembering to
 * pass the right thing. That is a property worth a test, because it is exactly
 * the kind of default somebody later "tidies" into a zero. */
void testCutaway()
{
    std::printf("== cutaway ==\n");

    /* 1. the default shows the entire world — the shadow pass's guarantee */
    const CutawayView whole;
    CHECK(whole.maxStorey >= kDefaultStoreyCount,
          "default cutaway stops at storey %d, must reach past any lattice", whole.maxStorey);
    for (int i = 0; i < kSurfaceFacingCount; i++)
        CHECK(whole.shows(static_cast<SurfaceFacing>(i)),
              "default cutaway hides facing %d", i);
    CHECK(CutawayView::whole().facings == whole.facings, "whole() differs from the default");

    /* 2. an unclassified surface can never be cut, from any angle. This is what
     *    makes a miss in the emitter's classifier cost a wall that fails to
     *    open rather than a hole in the world. */
    for (int degrees = 0; degrees < 360; degrees += 15) {
        const float radians = static_cast<float>(degrees) * 3.14159265f / 180.0f;
        const unsigned mask = facingsVisibleFrom(std::cos(radians), std::sin(radians));
        CHECK((mask & bitOf(SurfaceFacing::None)) != 0u,
              "facing None was cut at %d degrees", degrees);
    }

    /* 3. the rule itself: a camera at the south-east looks north-west, so the
     *    +X and -Z faces are the near ones and come off; the other two stay as
     *    the building's backdrop. Getting this inverted removes the far walls
     *    and every building reads as a floating floor. */
    const unsigned fromSouthEast = facingsVisibleFrom(-1.0f, 1.0f);
    CHECK(!(fromSouthEast & bitOf(SurfaceFacing::PlusX)),  "+X wall survived a camera east of it");
    CHECK(!(fromSouthEast & bitOf(SurfaceFacing::MinusZ)), "-Z wall survived a camera south of it");
    CHECK(fromSouthEast & bitOf(SurfaceFacing::MinusX),    "-X backdrop wall was cut");
    CHECK(fromSouthEast & bitOf(SurfaceFacing::PlusZ),     "+Z backdrop wall was cut");

    /* 4. rotating 90 degrees swaps which pair opens, which is the behaviour
     *    that makes the cutaway feel like it follows the camera at all */
    const unsigned fromNorthWest = facingsVisibleFrom(1.0f, -1.0f);
    CHECK(fromNorthWest & bitOf(SurfaceFacing::PlusX),  "+X did not come back when the camera swung round");
    CHECK(fromNorthWest & bitOf(SurfaceFacing::MinusZ), "-Z did not come back when the camera swung round");
    CHECK(!(fromNorthWest & bitOf(SurfaceFacing::MinusX)), "-X survived a camera west of it");
    CHECK(!(fromNorthWest & bitOf(SurfaceFacing::PlusZ)),  "+Z survived a camera north of it");

    /* 5. straight down has no horizontal direction to be in front of, so it
     *    cuts nothing and leaves the storey cut to do the work alone */
    CHECK(facingsVisibleFrom(0.0f, 0.0f) == kAllFacings,
          "a top-down camera cut a wall");

    /* 6. a wall seen edge-on stays up. The dot product is near zero there and
     *    without a deadband it would cut and uncut on camera noise while the
     *    player merely held a rotation key. */
    const unsigned dueNorth = facingsVisibleFrom(0.0f, 1.0f);
    CHECK(dueNorth & bitOf(SurfaceFacing::PlusX),  "+X cut while edge-on");
    CHECK(dueNorth & bitOf(SurfaceFacing::MinusX), "-X cut while edge-on");
    CHECK(!(dueNorth & bitOf(SurfaceFacing::MinusZ)), "-Z not cut with the camera due south of it");
}

/* WHICH SIDE OF A WALL IS THE OUTSIDE, checked against the demo map itself
 * rather than against a second implementation of the rule.
 *
 * THE CASE THAT DROVE THIS TEST. The building's south facade runs x = 4..11,
 * with the doorway at x = 7. The balcony overhang roofs the ground in front of
 * x = 4..9 — so by the enclosure test that decides RoomPartition's `outdoor`
 * flag, the porch under it is INDOORS, exactly like the room behind the wall.
 * A rule that asked "is one side indoors and the other out" therefore declined
 * to face those six segments and the wall in front of the doorway would not
 * open, while x = 10..11 past the overhang opened normally. Half a facade
 * cutting is worse than none, because it looks like a rendering bug rather
 * than a decision.
 *
 * The facade is checked end to end for that reason: the bug was a BAND, and a
 * test that sampled one column would have passed straight through it. */
void testWallFacing()
{
    std::printf("== wall facing ==\n");

    const RoomPartition rooms(g_world);

    /* the whole south facade faces south, overhang or no overhang */
    int faced = 0;
    for (int x = 4; x <= 11; x++) {
        if (x == 7) continue;   /* the doorway itself — no wall to face */

        const std::optional<Dir> outward = outwardWallDirection(rooms, x, 12, 0, Dir::South);
        CHECK(outward.has_value(), "south facade x=%d has no outward direction", x);
        if (outward) {
            CHECK(*outward == Dir::South, "south facade x=%d faces %d, expected South",
                  x, toIndex(*outward));
            faced++;
        }
    }
    std::printf("   south facade: %d/7 segments faced (x=4..9 sit under the balcony overhang)\n",
                faced);

    /* the north facade faces the other way — the rule is not simply agreeing
     * with whichever direction it was asked about */
    const std::optional<Dir> north = outwardWallDirection(rooms, 6, 19, 0, Dir::North);
    CHECK(north.has_value() && *north == Dir::North, "north facade does not face north");

    /* and the sides */
    const std::optional<Dir> west = outwardWallDirection(rooms, 4, 15, 0, Dir::West);
    const std::optional<Dir> east = outwardWallDirection(rooms, 11, 15, 0, Dir::East);
    CHECK(west.has_value() && *west == Dir::West, "west facade does not face west");
    CHECK(east.has_value() && *east == Dir::East, "east facade does not face east");

    /* THE OTHER HALF OF THE RULE: open ground has no wall worth facing. Both
     * sides are the same volume, so there is nothing behind it to reveal — and
     * this is what stops the classifier from quietly facing everything. */
    const std::optional<Dir> openField = outwardWallDirection(rooms, 20, 8, 0, Dir::North);
    CHECK(!openField.has_value(), "a face in open ground was given an outward direction");

    /* A wall's two sides must not both claim to be the outside. Asking from
     * the far cell has to give the exact opposite answer, or a slab could land
     * in two facing buckets depending on which neighbour emitted it. */
    for (int x = 4; x <= 11; x++) {
        if (x == 7) continue;
        const std::optional<Dir> fromInside  = outwardWallDirection(rooms, x, 12, 0, Dir::South);
        const std::optional<Dir> fromOutside = outwardWallDirection(rooms, x, 11, 0, Dir::North);
        CHECK(fromInside.has_value() == fromOutside.has_value(),
              "south facade x=%d disagrees about whether it faces at all", x);
        if (fromInside && fromOutside)
            CHECK(*fromInside == *fromOutside,
                  "south facade x=%d faces %d from inside but %d from outside",
                  x, toIndex(*fromInside), toIndex(*fromOutside));
    }
}

int main()
{
    testLattice();
    testMapStructure();
    testRamps();
    testSurfacePlane();
    testReach();
    testBorders();
    testSuppressedBorders();
    testLos();
    testUnits();
    testOccupancyIndex();
    testOcclusionGrid();
    testCutaway();
    testWallFacing();

    if (g_failures) std::printf("\n%d FAILURE(S)\n", g_failures);
    else            std::printf("\nall tile-core checks passed\n");
    return g_failures ? 1 : 0;
}
