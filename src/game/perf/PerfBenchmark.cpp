/* PerfBenchmark.cpp — see PerfBenchmark.hpp.
 *
 * Every case here is a query the AI will make constantly once there is one:
 * "who is standing there", "what can this body see", "where can it walk". They
 * are timed separately because they fail differently — one scales with the
 * roster, one with the map, one with both.
 */
#include "game/perf/PerfBenchmark.hpp"

#include "cromwell/spatial/SpatialHash.hpp"
#include "game/los/VisibilityComputer.hpp"
#include "game/los/VisibilityField.hpp"
#include "game/movement/graph/MoveGraph.hpp"
#include "game/movement/occupancy/BlockedMask.hpp"
#include "game/movement/search/Pathfinder.hpp"
#include "game/movement/search/ReachField.hpp"
#include "game/query/Standability.hpp"
#include "game/units/UnitFactory.hpp"
#include "game/units/roster/OccupancyMaskBuilder.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"
#include "game/world/authoring/DemoMapFactory.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

namespace game {

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/* The roster sizes every case is repeated at. Four is the demo roster; 256 is
 * well past what a tactics map would hold, and that is the point — a cost that
 * scales with the roster shows up as a straight line through these, and one
 * that does not stays flat. */
constexpr int kRosterSizes[] = { 4, 16, 64, 256 };

/* Deterministic placement: the first `count` flat-standable cells in lattice
 * order, one body each so nothing overlaps. Teams alternate so the mask builder
 * has both friendlies to skip and enemies to block on. */
void fillRoster(const World& world, UnitRoster& roster, int count)
{
    roster.clear();
    roster.bindLattice(world.lattice());

    const Lattice&     lattice = world.lattice();
    const Standability standable(world);

    int placed = 0;
    for (int z = 0; z < lattice.depth() && placed < count; z++)
    for (int y = 0; y < lattice.height() && placed < count; y++)
    for (int x = 0; x < lattice.width() && placed < count; x++) {
        if (!standable.isFlatStandable(x, y, z)) continue;
        roster.add(makeSoldier(Cell{ x, y, z },
                               (placed % 2) ? Team::Enemy : Team::Player));
        placed++;
    }
}

/* ---- the cases -------------------------------------------------------- */

/* "Who is standing here?", asked of every cell. This is the raw roll call,
 * with no ray casting on top of it — the clearest view of how the roster scan
 * grows. */
double timeOccupantSweep(const World& world, const UnitRoster& roster, int iterations)
{
    const Lattice& lattice = world.lattice();
    volatile int   sink = 0;

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++) {
        int found = 0;
        for (int index = 0; index < lattice.cellCount(); index++)
            if (roster.occupantAt(lattice.cellAt(index)) != nullptr) found++;
        sink = found;
    }
    (void)sink;
    return millisSince(start) / iterations;
}

/* One body's full visibility sweep — the query an AI makes to decide what it
 * can see. With a roster attached, every ray step also asks the roll call. */
double timeVisibilitySweep(const World& world, const UnitRoster* roster,
                           const Cell& from, int iterations)
{
    VisibilityField field;

    const VisibilityComputer computer =
        roster ? VisibilityComputer(world, *roster, &roster->at(0))
               : VisibilityComputer(world);

    computer.compute(from, field);   /* warm */

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++) computer.compute(from, field);
    return millisSince(start) / iterations;
}

/* Where can this body walk? Includes the mask build and the search's own
 * per-cell setup, because a caller pays both. */
double timeReachSearch(const World& world, const UnitRoster& roster, int iterations)
{
    const Unit&                      mover = roster.at(0);
    const std::unique_ptr<MoveGraph> graph = mover.createMoveGraph(world);
    const Pathfinder                 pathfinder(world.lattice());

    BlockedMask mask;
    ReachField  reach;

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++) {
        OccupancyMaskBuilder::build(roster, &mover, world.lattice(), mask);
        pathfinder.search(*graph, mover.position(), 6.0f, &mask, reach);
    }
    return millisSince(start) / iterations;
}

/* THE SCRATCH WIPE, ALONE. A search fans out from the mover and usually
 * touches a small neighbourhood, but it first clears three full-map arrays —
 * cost, predecessor and arrival kind. That clearing is what a generation stamp
 * would remove, and whether removing it is worth a stamp read on every cost()
 * lookup depends entirely on the ratio between these two numbers. It scales
 * with the MAP; the search beside it scales with the reachable area. */
double timeReachReset(const World& world, int iterations)
{
    ReachField reach;
    reach.reset(world.lattice().cellCount());   /* warm: first call allocates */

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++) reach.reset(world.lattice().cellCount());
    return millisSince(start) / iterations;
}

/* The search on its own, with the wipe already paid. */
double timeReachSearchOnly(const World& world, const UnitRoster& roster, int iterations)
{
    const Unit&                      mover = roster.at(0);
    const std::unique_ptr<MoveGraph> graph = mover.createMoveGraph(world);
    const Pathfinder                 pathfinder(world.lattice());

    BlockedMask mask;
    OccupancyMaskBuilder::build(roster, &mover, world.lattice(), mask);

    ReachField reach;
    pathfinder.search(*graph, mover.position(), 6.0f, &mask, reach);

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++)
        pathfinder.search(*graph, mover.position(), 6.0f, &mask, reach);
    return millisSince(start) / iterations;
}

/* ---- the swarm question ------------------------------------------------
 * EVERY AGENT ASKS "WHO IS NEAR ME", ONCE PER FRAME. That is what local
 * steering is — flocking, separation, collision avoidance — and it is the
 * dominant cost of a crowd, well ahead of pathfinding, because a coarse route
 * is computed rarely and avoidance runs every frame for everybody.
 *
 * Done against every other agent it is O(n^2). Done against a spatial index it
 * is O(n) with a small constant. These two rows are that difference, which is
 * the difference between a horde and a demo.
 *
 * A frame budget is 16.7ms at 60Hz, and steering may have a fraction of it. */
struct SwarmTiming {
    double bruteForceMs = 0.0;
    double indexedMs = 0.0;
    double buildMs = 0.0;
    long long neighboursFound = 0;
};

SwarmTiming timeSwarmNeighbours(int agents, float worldExtent, float queryRadius)
{
    /* Deterministic scatter, so the numbers are comparable between runs. */
    std::vector<Vec3> positions;
    positions.reserve(static_cast<std::size_t>(agents));
    std::uint32_t seed = 12345u;
    const auto nextUnit = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < agents; i++)
        positions.push_back(Vec3{ nextUnit() * worldExtent,
                                  nextUnit() * 2.0f,          /* a thin slab, as a crowd is */
                                  nextUnit() * worldExtent });

    SwarmTiming out;
    const float radiusSquared = queryRadius * queryRadius;

    /* --- brute force: every agent against every other --- */
    {
        volatile long long sink = 0;
        const auto start = Clock::now();
        long long found = 0;
        for (int i = 0; i < agents; i++)
            for (int j = 0; j < agents; j++) {
                if (i == j) continue;
                if (distanceSquared(positions[static_cast<std::size_t>(i)],
                                    positions[static_cast<std::size_t>(j)]) <= radiusSquared)
                    found++;
            }
        sink = found;
        out.bruteForceMs = millisSince(start);
        out.neighboursFound = static_cast<long long>(sink);
    }

    /* --- indexed: rebuild the hash, then one query per agent ---
     * The rebuild is timed as part of the frame, because it IS part of the
     * frame — the index is thrown away and refilled every tick. */
    {
        /* Cell size at the query radius: the sweet spot is roughly one cell per
         * query, so a query touches 8 cells in 3D rather than hundreds. */
        SpatialHash      hash(queryRadius, 8192);
        std::vector<int> neighbours;
        neighbours.reserve(64);

        const auto buildStart = Clock::now();
        hash.clear();
        for (int i = 0; i < agents; i++)
            hash.insert(i, positions[static_cast<std::size_t>(i)]);
        out.buildMs = millisSince(buildStart);

        volatile long long sink = 0;
        const auto start = Clock::now();
        long long found = 0;
        for (int i = 0; i < agents; i++) {
            hash.queryRadius(positions[static_cast<std::size_t>(i)], queryRadius, neighbours);
            found += static_cast<long long>(neighbours.size()) - 1;   /* minus itself */
        }
        sink = found;
        out.indexedMs = millisSince(start) + out.buildMs;

        /* Both paths must agree, or the speed-up is measuring a bug. */
        if (static_cast<long long>(sink) != out.neighboursFound)
            std::printf("  MISMATCH: brute force found %lld neighbours, index found %lld\n",
                        out.neighboursFound, static_cast<long long>(sink));
    }

    return out;
}

/* AT WHAT RESOLUTION? The cell size is the only tuning knob a spatial hash has,
 * and it is not a correctness knob — every value gives identical answers. It
 * trades two costs against each other:
 *
 *   cells too SMALL -> a query sweeps many of them; the loop overhead and the
 *                      bucket lookups dominate, and most cells are empty
 *   cells too LARGE -> each holds candidates far outside the radius, and every
 *                      one costs a distance test that fails
 *
 * The received wisdom is "cell size about the query radius". This measures it
 * instead, because the received wisdom does not know the crowd's density. */
void reportCellSizeSweep(int agents, float worldExtent, float queryRadius)
{
    std::vector<Vec3> positions;
    positions.reserve(static_cast<std::size_t>(agents));
    std::uint32_t seed = 999u;
    const auto nextUnit = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < agents; i++)
        positions.push_back(Vec3{ nextUnit() * worldExtent,
                                  nextUnit() * 2.0f,
                                  nextUnit() * worldExtent });

    std::printf("\ncell size sweep - %d agents, %.1f unit query radius\n",
                agents, static_cast<double>(queryRadius));
    std::printf("%-14s %14s %14s %12s\n",
                "cell size", "rebuild+query", "vs radius", "longest chain");

    for (float multiple : { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f }) {
        const float cellSize = queryRadius * multiple;

        SpatialHash      hash(cellSize, 8192);
        std::vector<int> neighbours;
        neighbours.reserve(64);

        /* warm */
        hash.clear();
        for (int i = 0; i < agents; i++) hash.insert(i, positions[static_cast<std::size_t>(i)]);

        volatile long long sink = 0;
        const auto start = Clock::now();

        constexpr int kRuns = 5;
        long long found = 0;
        for (int run = 0; run < kRuns; run++) {
            hash.clear();
            for (int i = 0; i < agents; i++)
                hash.insert(i, positions[static_cast<std::size_t>(i)]);
            for (int i = 0; i < agents; i++) {
                hash.queryRadius(positions[static_cast<std::size_t>(i)], queryRadius, neighbours);
                found += static_cast<long long>(neighbours.size());
            }
        }
        sink = found;
        (void)sink;

        std::printf("%-14.2f %11.3f ms %13.2fx %12d\n",
                    static_cast<double>(cellSize),
                    millisSince(start) / kRuns,
                    static_cast<double>(multiple),
                    hash.longestChain());
    }
}

/* The mask build alone, so its share of the row above is visible. */
double timeMaskBuild(const World& world, const UnitRoster& roster, int iterations)
{
    const Unit& mover = roster.at(0);
    BlockedMask mask;

    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++)
        OccupancyMaskBuilder::build(roster, &mover, world.lattice(), mask);
    return millisSince(start) / iterations;
}

}  // namespace

int runPerfBenchmark()
{
    World world;
    DemoMapFactory::build(world);

    std::printf("simulation perf benchmark - single threaded, demo map (%d x %d x %d = %d cells)\n",
                world.lattice().width(), world.lattice().height(),
                world.lattice().depth(), world.lattice().cellCount());

    /* Pure terrain, no roster at all: this is RayCaster and nothing else, so it
     * is the row that moves when the tile lookup changes and stays put when the
     * roster lookup does. */
    {
        UnitRoster anchor;
        fillRoster(world, anchor, 1);
        const Cell from = anchor.at(0).position();
        std::printf("\nvisibility sweep, terrain only (no roster)\n");
        std::printf("  %8.3f ms per sweep\n", timeVisibilitySweep(world, nullptr, from, 5));

        /* Is the full-map scratch wipe worth removing? Only the ratio says. */
        const double reset  = timeReachReset(world, 2000);
        const double search = timeReachSearchOnly(world, anchor, 2000);
        std::printf("\nreach search, split\n");
        std::printf("  %8.4f ms scratch wipe (scales with the MAP)\n", reset);
        std::printf("  %8.4f ms search itself (scales with what it REACHES)\n", search);
        std::printf("  wipe is %.0f%% of a search\n", 100.0 * reset / (reset + search));
    }

    std::printf("\n%-8s %14s %14s %14s %14s\n",
                "bodies", "occupant/cell", "mask build", "reach search", "visibility");
    std::printf("  (each column is one full query, averaged)\n");

    for (int count : kRosterSizes) {
        UnitRoster roster;
        fillRoster(world, roster, count);
        if (roster.size() < count) {
            std::printf("  (only %d standable cells - skipping %d)\n", roster.size(), count);
            continue;
        }

        const Cell from = roster.at(0).position();

        /* Fewer iterations as the work grows, so the whole run stays short; all
         * figures are per-query regardless. */
        const int sweepIters = count <= 16 ? 3 : 1;

        const double occupant   = timeOccupantSweep(world, roster, 5);
        const double mask       = timeMaskBuild(world, roster, 20);
        const double reach      = timeReachSearch(world, roster, 20);
        const double visibility = timeVisibilitySweep(world, &roster, from, sweepIters);

        std::printf("%-8d %11.3f ms %11.3f ms %11.3f ms %11.3f ms\n",
                    count, occupant, mask, reach, visibility);
    }

    /* ---- the swarm question --------------------------------------------
     * Not about this game — nothing here has a crowd. It is the engine-level
     * measurement that decides whether a horde is affordable at all, so it
     * lives with the other numbers rather than in a note somewhere. */
    std::printf("\n\"who is near me\", asked once per agent per frame\n");
    std::printf("  crowd in a 60 x 60 unit space, 2 unit avoidance radius\n");
    std::printf("  (a 60Hz frame is 16.7 ms in total, and steering gets a slice of it)\n\n");
    std::printf("%-8s %14s %14s %10s %14s\n",
                "agents", "every-pair", "spatial hash", "speed-up", "neighbours");

    for (int agents : { 100, 500, 2000, 5000 }) {
        const SwarmTiming timing = timeSwarmNeighbours(agents, 60.0f, 2.0f);
        std::printf("%-8d %11.3f ms %11.3f ms %9.1fx %14lld\n",
                    agents, timing.bruteForceMs, timing.indexedMs,
                    timing.indexedMs > 0.0 ? timing.bruteForceMs / timing.indexedMs : 0.0,
                    timing.neighboursFound);
    }

    reportCellSizeSweep(2000, 60.0f, 2.0f);

    std::printf("\n");
    return 0;
}

}  // namespace game
