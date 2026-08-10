/* SpatialHashTests.cpp — the index must agree with the loop it replaces.
 *
 * A spatial index is an optimisation, so the only interesting question about it
 * is whether it returns exactly what a brute-force scan would. Every case here
 * therefore compares against that scan rather than against expected constants:
 * a hand-written expectation would drift the moment the hash function changed,
 * and would not catch the failures that actually happen.
 *
 * THE FAILURES THAT ACTUALLY HAPPEN, and which each have a case below:
 *   - hash collisions emitting a neighbouring cell's entries, or the same
 *     entry twice from two cells that share a bucket
 *   - negative coordinates, where a truncating divide makes the two cells
 *     either side of the origin one double-width cell
 *   - a query radius smaller or larger than the cell size
 *   - everything landing in one cell, which is legal and must still be correct
 */
#include "cromwell/spatial/SpatialHash.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("   FAIL: ");                                          \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* Deterministic, and deliberately not std::rand — a test that generates a
 * different world per run reports a different result per run. */
struct Lcg {
    std::uint32_t state = 12345u;
    std::uint32_t next() { state = state * 1664525u + 1013904223u; return state; }
    float unit() { return static_cast<float>(next() >> 8) / static_cast<float>(1u << 24); }
    float range(float lo, float hi) { return lo + unit() * (hi - lo); }
};

std::vector<int> bruteForceRadius(const std::vector<Vec3>& points, Vec3 centre, float radius)
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(points.size()); i++)
        if (distanceSquared(points[static_cast<std::size_t>(i)], centre) <= radius * radius)
            out.push_back(i);
    return out;
}

std::vector<int> bruteForceBox(const std::vector<Vec3>& points, Vec3 min, Vec3 max)
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(points.size()); i++) {
        const Vec3& p = points[static_cast<std::size_t>(i)];
        if (p.x >= min.x && p.x <= max.x &&
            p.y >= min.y && p.y <= max.y &&
            p.z >= min.z && p.z <= max.z)
            out.push_back(i);
    }
    return out;
}

bool sameSet(std::vector<int> a, std::vector<int> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

/* A duplicate is invisible to a set comparison, and is exactly what a hash
 * collision produces, so it is checked separately. */
bool hasDuplicates(std::vector<int> ids)
{
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

/* ---------------------------------------------------------------- case 1 */
/* Random points, random queries, against brute force. Small bucket count on
 * purpose: 64 buckets for 400 points guarantees heavy collisions, which is the
 * condition the cell-key check exists to survive. */
void testAgainstBruteForce(const char* label, float extent, float cellSize, int bucketCount)
{
    Lcg rng;
    std::vector<Vec3> points;
    for (int i = 0; i < 400; i++)
        points.push_back(Vec3{ rng.range(-extent, extent),
                               rng.range(-extent, extent),
                               rng.range(-extent, extent) });

    SpatialHash hash(cellSize, bucketCount);
    for (int i = 0; i < static_cast<int>(points.size()); i++)
        hash.insert(i, points[static_cast<std::size_t>(i)]);

    CHECK(hash.size() == static_cast<int>(points.size()),
          "%s: inserted %d, index holds %d", label, (int)points.size(), hash.size());

    std::vector<int> got;
    int mismatches = 0, duplicates = 0, totalFound = 0;

    for (int q = 0; q < 200; q++) {
        const Vec3  centre{ rng.range(-extent, extent),
                            rng.range(-extent, extent),
                            rng.range(-extent, extent) };
        /* radii deliberately spanning below, at, and above the cell size */
        const float radius = rng.range(0.1f, cellSize * 3.0f);

        hash.queryRadius(centre, radius, got);
        totalFound += static_cast<int>(got.size());

        if (!sameSet(got, bruteForceRadius(points, centre, radius))) mismatches++;
        if (hasDuplicates(got)) duplicates++;
    }

    CHECK(mismatches == 0, "%s: %d/200 radius queries disagreed with brute force",
          label, mismatches);
    CHECK(duplicates == 0, "%s: %d/200 radius queries returned an id twice",
          label, duplicates);

    /* Box queries over the same set. */
    int boxMismatches = 0;
    for (int q = 0; q < 200; q++) {
        Vec3 a{ rng.range(-extent, extent), rng.range(-extent, extent), rng.range(-extent, extent) };
        Vec3 b{ rng.range(-extent, extent), rng.range(-extent, extent), rng.range(-extent, extent) };
        const Vec3 min = minPerAxis(a, b);
        const Vec3 max = maxPerAxis(a, b);

        hash.queryBox(min, max, got);
        if (!sameSet(got, bruteForceBox(points, min, max))) boxMismatches++;
    }
    CHECK(boxMismatches == 0, "%s: %d/200 box queries disagreed with brute force",
          label, boxMismatches);

    std::printf("   %-34s %4d pts, longest chain %3d, %d hits over 200 queries\n",
                label, hash.size(), hash.longestChain(), totalFound);
}

/* ---------------------------------------------------------------- case 2 */
/* Everything in one spot. Legal, degenerate, and must still be exactly right —
 * this is the army-standing-on-one-tile case the header warns about. */
void testDegenerateClustering()
{
    SpatialHash hash(4.0f, 4096);
    for (int i = 0; i < 500; i++) hash.insert(i, Vec3{ 1.0f, 1.0f, 1.0f });

    std::vector<int> got;
    hash.queryRadius(Vec3{ 1.0f, 1.0f, 1.0f }, 0.5f, got);

    CHECK(static_cast<int>(got.size()) == 500,
          "clustered: expected all 500, got %d", (int)got.size());
    CHECK(!hasDuplicates(got), "clustered: an id came back twice");
    CHECK(hash.longestChain() == 500,
          "clustered: longestChain should report the pile-up, said %d", hash.longestChain());

    hash.queryRadius(Vec3{ 40.0f, 40.0f, 40.0f }, 1.0f, got);
    CHECK(got.empty(), "clustered: far query found %d", (int)got.size());

    std::printf("   %-34s all 500 found, chain length reported\n", "degenerate clustering");
}

/* ---------------------------------------------------------------- case 3 */
/* Straddling the origin. A truncating divide instead of a floor makes the cells
 * at -0.5 and +0.5 the same cell, which is correct-looking until a query near
 * the origin quietly misses half its neighbours. */
void testNegativeCoordinates()
{
    SpatialHash hash(1.0f, 1024);
    const std::vector<Vec3> points = {
        { -0.5f, 0.0f, 0.0f }, { 0.5f, 0.0f, 0.0f },
        { -1.5f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f },
        { -0.5f, -0.5f, -0.5f }, { -2.5f, -2.5f, -2.5f },
    };
    for (int i = 0; i < static_cast<int>(points.size()); i++)
        hash.insert(i, points[static_cast<std::size_t>(i)]);

    std::vector<int> got;
    for (float radius : { 0.6f, 1.2f, 2.5f, 5.0f }) {
        const Vec3 centre = Vec3::zero();
        hash.queryRadius(centre, radius, got);
        CHECK(sameSet(got, bruteForceRadius(points, centre, radius)),
              "negative coords: radius %.1f disagreed", static_cast<double>(radius));
    }

    /* And a query centred in negative space. */
    hash.queryRadius(Vec3{ -2.5f, -2.5f, -2.5f }, 0.4f, got);
    CHECK(got.size() == 1 && got[0] == 5, "negative coords: missed the point at -2.5");

    std::printf("   %-34s cells step evenly across zero\n", "negative coordinates");
}

/* ---------------------------------------------------------------- case 4 */
/* clear() must leave nothing behind — a per-frame rebuild is the whole usage
 * pattern, so a stale entry surviving it would corrupt every later frame. */
void testClearAndRebuild()
{
    SpatialHash hash(2.0f, 256);
    std::vector<int> got;

    for (int frame = 0; frame < 3; frame++) {
        hash.clear();
        CHECK(hash.empty(), "clear: still holds %d entries", hash.size());

        const float offset = static_cast<float>(frame) * 100.0f;
        for (int i = 0; i < 50; i++)
            hash.insert(i, Vec3{ offset + static_cast<float>(i), 0.0f, 0.0f });

        hash.queryRadius(Vec3{ offset + 25.0f, 0.0f, 0.0f }, 2.5f, got);
        CHECK(static_cast<int>(got.size()) == 5,
              "clear: frame %d found %d, expected 5", frame, (int)got.size());

        /* Nothing from the previous frame's position may survive. */
        if (frame > 0) {
            hash.queryRadius(Vec3{ offset - 100.0f + 25.0f, 0.0f, 0.0f }, 2.5f, got);
            CHECK(got.empty(), "clear: frame %d still sees %d stale entries",
                  frame, (int)got.size());
        }
    }

    std::printf("   %-34s no entry survives a rebuild\n", "clear and rebuild");
}

}  // namespace

int main()
{
    std::printf("== spatial hash ==\n");

    /* Spread out, sensible cell size — the ordinary case. */
    testAgainstBruteForce("sparse, 4096 buckets", 50.0f, 4.0f, 4096);
    /* Far too few buckets for the point count: collisions everywhere. */
    testAgainstBruteForce("collision-heavy, 64 buckets", 50.0f, 4.0f, 64);
    /* Cell far smaller than the queries, so each sweeps many cells. */
    testAgainstBruteForce("fine cells (0.5)", 20.0f, 0.5f, 4096);
    /* Cell far larger than the world, so everything shares one. */
    testAgainstBruteForce("coarse cells (200)", 20.0f, 200.0f, 4096);

    testDegenerateClustering();
    testNegativeCoordinates();
    testClearAndRebuild();

    if (g_failures) std::printf("\n%d FAILURE(S)\n", g_failures);
    else            std::printf("\nall spatial hash checks passed\n");
    return g_failures ? 1 : 0;
}
