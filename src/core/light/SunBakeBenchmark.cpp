/* SunBakeBenchmark.cpp — how long does a sun bake actually take?
 *
 * SINGLE RESPONSIBILITY: run SunBaker over the demo map at a few settings and
 * print the numbers, so the "can we bake static shadows given destructible
 * terrain?" question is answered with a measurement instead of an argument.
 *
 * Headless on purpose — no window, no GPU. The cost being measured is
 * raycasting against the tile lattice, which is core's business.
 */
#include "core/light/SunBakeBenchmark.hpp"

#include "core/light/SunBaker.hpp"
#include "core/world/DemoMapFactory.hpp"
#include "core/world/World.hpp"

#include <cmath>
#include <cstdio>

namespace xcom {
namespace {

/* The renderer's default sun: azimuth 125 degrees, elevation 48. Kept in sync
 * by hand with SunLight's defaults — this is a benchmark, not a second source
 * of truth for the lighting. */
SunSample defaultSun()
{
    const float azimuth = 125.0f * 3.14159265f / 180.0f;
    const float elevation = 48.0f * 3.14159265f / 180.0f;
    const float horizontal = std::cos(elevation);

    SunSample sun;
    sun.directionX = -horizontal * std::cos(azimuth);
    sun.directionY = -std::sin(elevation);
    sun.directionZ = -horizontal * std::sin(azimuth);

    /* Roughly six times the real sun's angular radius. Games routinely
     * exaggerate it: the true 0.0047 rad gives shadows so sharp they read as
     * cut paper at tile scale. */
    sun.angularRadius = 0.03f;
    return sun;
}

/* What DestructionSystem does to the DATA, without the app layer core cannot
 * see: clear destructible cover, floors and blocked mass inside the radius.
 * Note it only ever REMOVES occluders, which is what makes the correctness
 * check below a one-sided test. */
int demolish(World& world, const Cell& centre, float radiusTiles)
{
    const Lattice& lattice = world.lattice();
    const int reach = static_cast<int>(std::ceil(radiusTiles));
    int edits = 0;

    for (int y = centre.y - reach; y <= centre.y + reach; y++)
    for (int x = centre.x - reach; x <= centre.x + reach; x++) {
        if (!lattice.inBounds(x, y)) continue;
        const float dx = static_cast<float>(x - centre.x);
        const float dy = static_cast<float>(y - centre.y);
        if (std::sqrt(dx * dx + dy * dy) > radiusTiles) continue;

        /* A grenade edits the whole storey column, as the real one does. */
        for (int i = 0; i < kCellsPerStorey; i++) {
            const int z = Lattice::storeyBaseZ(Lattice::storeyOfZ(centre.z)) + i;
            if (!lattice.isValid(x, y, z)) continue;

            Tile& tile = world.at(lattice.index(x, y, z));
            for (Dir d : kAllDirs) {
                Edge& edge = tile.edge(d);
                if (edge.cover != Cover::None && edge.destructible) {
                    edge = Edge{};
                    edits++;
                }
            }
            if (tile.blocked && tile.blockedDestructible) { tile.blocked = false; edits++; }
            if (tile.hasFloor && tile.floorDestructible)  { tile.hasFloor = false; edits++; }
            if (tile.canopy) { tile.canopy = false; edits++; }
        }
    }
    return edits;
}

void report(const char* label, const SunBakeStats& stats)
{
    std::printf("  %-26s %6d patches  %8d texels  %10lld rays  %8.1f ms  (%.1f Mray/s)\n",
                label, stats.patches, stats.texels,
                static_cast<long long>(stats.rays), stats.milliseconds,
                stats.raysPerSecond() / 1.0e6);
}

void runOne(int texelsPerTile, int raysPerTexel)
{
    World world;
    DemoMapFactory::build(world);

    SunBaker baker(world, texelsPerTile, raysPerTexel);
    const SunSample sun = defaultSun();

    std::printf("\n%d x %d texels per cell face, %d sun rays per texel\n",
                texelsPerTile, texelsPerTile, raysPerTexel);

    report("full map", baker.bakeAll(sun));

    /* The same grenade the --boom flag throws, and the radius
     * DestructionSystem uses. */
    const SunBakeStats incremental =
        baker.bakeRegion(sun, Cell{ 11, 12, 0 }, 2.2f);
    report("after one grenade", incremental);
}

/* THE QUESTION THIS WHOLE FILE EXISTS TO ANSWER: after a wall is blown out,
 * does an incremental re-bake actually put the light back?
 *
 * A stale bake keeps the dead wall's shadow on the floor and leaves the new
 * hole dark. So this bakes, detonates, re-bakes only the affected region, and
 * checks the result moved in the right direction. Destruction here only ever
 * REMOVES occluders, so every changed texel must get BRIGHTER — a texel that
 * darkened would mean the incremental region missed something and left a
 * stale value behind. */
void runDestructionCheck(const Cell& blast, float kRadius)
{
    constexpr int kTexels = 16;
    constexpr int kRays = 16;

    World world;
    DemoMapFactory::build(world);

    SunBaker baker(world, kTexels, kRays);
    const SunSample sun = defaultSun();
    baker.bakeAll(sun);

    /* Snapshot BOTH the values and the slot table. Patch slots move when
     * geometry changes, so comparing the two arrays index-by-index would be
     * comparing different surfaces — the stable key is (cell, face). */
    const std::vector<float> before = baker.visibility();
    const std::vector<int>   beforeSlots = baker.slotTable();

    /* Blow a hole exactly as DestructionSystem would: clear destructible
     * cover, floors and blocked mass in the blast radius. */
    const int edits = demolish(world, blast, kRadius);

    /* Geometry changed, so the patch set is rebuilt — carrying surviving
     * texels across — and only the affected region is re-baked. */
    baker.refreshGeometry();
    const SunBakeStats stats = baker.bakeRegion(sun, blast, kRadius);
    const std::vector<float>& after = baker.visibility();

    const int perPatch = baker.texelsPerPatch();
    int brighter = 0, darker = 0, survived = 0;
    float gained = 0.0f;

    for (std::size_t key = 0; key < beforeSlots.size(); key++) {
        const int oldSlot = beforeSlots[key];
        if (oldSlot < 0) continue;

        const Cell cell = world.lattice().cellAt(
            static_cast<int>(key / SunBaker::kFacesPerCell));
        const int face = static_cast<int>(key % SunBaker::kFacesPerCell);

        const int newSlot = baker.slotOf(cell, face);
        if (newSlot < 0) continue;   /* the surface itself was destroyed */
        survived++;

        for (int i = 0; i < perPatch; i++) {
            const float delta =
                after[static_cast<std::size_t>(newSlot) * perPatch + i]
              - before[static_cast<std::size_t>(oldSlot) * perPatch + i];
            if (delta > 0.01f) { brighter++; gained += delta; }
            else if (delta < -0.01f) darker++;
        }
    }
    const char* verdict = (darker > 0) ? "FAIL stale"
                        : (brighter > 0) ? "pass"
                                         : "no change";

    std::printf("  (%2d,%2d) r=%.1f  %3d edits  %5d re-baked texels  %6.1f ms  "
                "%6d brighter  %5d darker  %s\n",
                blast.x, blast.y, static_cast<double>(kRadius), edits,
                stats.texels, stats.milliseconds, brighter, darker, verdict);
    (void)survived;
    (void)gained;
}

}  // namespace

int runSunBakeBenchmark()
{
    std::printf("sun bake benchmark — single threaded, demo map\n");
    std::printf("(the work is embarrassingly parallel; these are one core)\n");

    runOne(8, 8);
    runOne(16, 16);
    runOne(32, 16);

    /* Several sites rather than one, so a pass is not a lucky pick. "darker"
     * must stay zero everywhere: destruction only removes occluders, so any
     * texel that got DARKER is a stale value the affected region failed to
     * cover. */
    std::printf("\ndestruction correctness — 16 x 16 texels, 16 rays\n");
    std::printf("  bake, demolish, re-bake only the affected region, compare by (cell, face)\n\n");
    runDestructionCheck(Cell{ 11, 12, 0 }, 2.2f);
    runDestructionCheck(Cell{ 10, 11, 0 }, 3.0f);
    runDestructionCheck(Cell{  9, 13, 0 }, 4.0f);
    runDestructionCheck(Cell{ 12, 10, 0 }, 5.0f);
    runDestructionCheck(Cell{ 11, 12, 3 }, 4.0f);
    runDestructionCheck(Cell{ 13, 14, 3 }, 5.0f);

    std::printf("\n");
    return 0;
}

}  // namespace xcom
