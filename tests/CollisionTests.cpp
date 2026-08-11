/* CollisionTests.cpp — headless verification of the trace layer.
 *
 * WHY THIS FILE IS WORTH MORE THAN MOST. Everything under cromwell/collision is
 * arithmetic with an exactly knowable answer: a box sweeping along +X from
 * x = 0.5 with a half-extent of 0.25 touches a cell whose face is at x = 3 after
 * exactly 2.25 metres, and no amount of looking at the game will tell you
 * whether it did. The failures here are all of the same shape — off by a
 * half-extent, off by a cell, a normal on the wrong axis, a contact reported at
 * the box's centre instead of its surface — and every one of them shows up in
 * play as something vague like "collision feels sticky" and gets chased through
 * the wrong system for a day.
 *
 * WHAT IS DELIBERATELY NOT TESTED. Nothing here checks that the trace layer is
 * FAST; that is xcom_perf's job and it needs a world to be fast against.
 * Nothing checks that the game's tiles are correctly classified into layers —
 * that is the game's own concern and its own test.
 *
 * THE NUMBERS BELOW ARE HAND-COMPUTED, not captured from a run. A test that
 * asserts what the code currently does is a change detector, not a
 * specification, and it will happily lock in the off-by-a-half-extent it was
 * written to catch.
 */
#include "cromwell/collision/GridTrace.hpp"
#include "cromwell/collision/Intersect.hpp"
#include "cromwell/collision/LayerMatrix.hpp"
#include "cromwell/collision/TraceHit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

bool nearly(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

bool nearly(Vec3 a, Vec3 b, float tolerance = 1.0e-4f)
{
    return nearly(a.x, b.x, tolerance) && nearly(a.y, b.y, tolerance)
        && nearly(a.z, b.z, tolerance);
}

/* ---- layers ----------------------------------------------------------- */

void testLayerMaskSetOperations()
{
    const LayerId world(0);
    const LayerId body(1);
    const LayerId trigger(2);

    const LayerMask solid = LayerMask::of(world) | LayerMask::of(body);
    CHECK(solid.has(world) && solid.has(body), "both layers are in the union");
    CHECK(!solid.has(trigger), "an unrelated layer is not");
    CHECK(!solid.without(body).has(body), "without removes");
    CHECK(LayerMask::none().empty(), "the empty mask is empty");

    /* The default-constructed id must match nothing — see LayerId's note on why
     * it is invalid rather than layer zero. */
    CHECK(!LayerMask::all().has(LayerId{}), "an unset layer is in no mask, not even all");
}

void testMatrixIsSymmetricAndReplaceable()
{
    LayerMatrix matrix;
    const LayerId bullet(0);
    const LayerId wall(1);

    matrix.setResponse(bullet, wall, Response::Block);
    CHECK(matrix.response(bullet, wall) == Response::Block, "set one way");
    CHECK(matrix.response(wall, bullet) == Response::Block, "readable the other way");

    /* RELAXING A RULE MUST WORK. Without the clear in setResponse, Block would
     * still be set alongside Overlap and would keep winning — a table whose
     * entries can only ever get stronger. */
    matrix.setResponse(bullet, wall, Response::Overlap);
    CHECK(matrix.response(bullet, wall) == Response::Overlap, "a response can be relaxed");
    CHECK(!matrix.blockedBy(bullet).has(wall), "and the blocking mask no longer holds it");
}

void testFilterComesFromTheMatrix()
{
    LayerMatrix matrix;
    const LayerId bullet(0);
    const LayerId wall(1);
    const LayerId body(2);
    const LayerId trigger(3);
    const LayerId smoke(4);

    matrix.nameLayer(bullet, "bullet");
    matrix.nameLayer(wall, "wall");

    matrix.setResponse(bullet, wall, Response::Block);
    matrix.setResponse(bullet, body, Response::Block);
    matrix.setResponse(bullet, trigger, Response::Overlap);
    /* smoke left unset — the default is Ignore, which is what an unconfigured
     * pair should be. */

    const TraceFilter filter = matrix.filterFor(bullet);
    CHECK(filter.responseTo(wall) == Response::Block, "the row's blocks become blocks");
    CHECK(filter.responseTo(trigger) == Response::Overlap, "and its overlaps, overlaps");
    CHECK(filter.responseTo(smoke) == Response::Ignore, "an unconfigured pair is ignored");
    CHECK(filter.relevant().has(wall) && filter.relevant().has(trigger),
          "relevant covers both responses");
    CHECK(!filter.relevant().has(smoke), "and excludes what is ignored");

    /* The per-call-site adjustment — a shot that should pass through the cover
     * it was fired from. */
    CHECK(filter.ignoring(wall).responseTo(wall) == Response::Ignore, "ignoring drops it");
    CHECK(filter.alsoOverlapping(wall).responseTo(wall) == Response::Block,
          "and a block is not silently downgraded to an overlap");
}

void testMatrixDescribesItself()
{
    LayerMatrix matrix;
    const LayerId bullet(0);
    const LayerId wall(1);
    matrix.nameLayer(bullet, "bullet");
    matrix.nameLayer(wall, "wall");
    matrix.setResponse(bullet, wall, Response::Block);

    const std::string description = matrix.describe(bullet);
    CHECK(description.find("bullet") != std::string::npos, "names the layer");
    CHECK(description.find("wall") != std::string::npos, "and what it blocks");
    CHECK(matrix.findLayer("wall") == wall, "layers are findable by name");
    CHECK(!matrix.findLayer("nonexistent").valid(), "and an unknown name is invalid");
}

/* ---- ray and box sweeps ------------------------------------------------ */

void testRayHitsBoxFaceExactly()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);  /* x from 3 to 4 */

    const SweepContact contact = sweepBox(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3::zero(),
                                          Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit, "the ray reaches the cell");
    CHECK(nearly(contact.distance, 2.5f), "3.0 minus 0.5 is 2.5, not 2.0 or 3.0");
    CHECK(nearly(contact.normal, Vec3{ -1.0f, 0.0f, 0.0f }), "the normal faces the ray");
    CHECK(nearly(contact.point, Vec3{ 3.0f, 0.5f, 0.5f }), "the contact is on the face");
    CHECK(nearly(contact.end, contact.point), "for a ray, end and point coincide");
    CHECK(!contact.startPenetrating, "it did not start inside");
}

void testBoxSweepStopsShortByItsHalfExtent()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);

    /* THE OFF-BY-A-HALF-EXTENT TEST. A box of half-extent 0.25 travelling from
     * x = 0.5 touches the face at x = 3 when its CENTRE reaches 2.75. A sweep
     * that reported 2.5 would bury a quarter of the box in the wall. */
    const SweepContact contact = sweepBox(Vec3{ 0.5f, 0.5f, 0.5f },
                                          Vec3{ 0.25f, 0.25f, 0.25f },
                                          Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit, "the box reaches the cell");
    CHECK(nearly(contact.distance, 2.25f), "it stops a half-extent short of the face");
    CHECK(nearly(contact.end.x, 2.75f), "which puts its centre at 2.75");
    CHECK(nearly(contact.point.x, 3.0f), "while the surfaces touch at 3.0");
    CHECK(nearly(contact.normal, Vec3{ -1.0f, 0.0f, 0.0f }), "normal out of the face hit");
}

void testSweepMissesWhenItPassesBeside()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);

    /* Two cells over on y. The slab test must reject on a perpendicular axis,
     * which is the case a naive "is the x face crossed" check gets wrong. */
    const SweepContact contact = sweepBox(Vec3{ 0.5f, 2.5f, 0.5f }, Vec3::zero(),
                                          Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);
    CHECK(!contact.hit, "a ray passing beside the cell misses it");
}

void testSweepRespectsMaxDistance()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);
    const SweepContact contact = sweepBox(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3::zero(),
                                          Vec3{ 1.0f, 0.0f, 0.0f }, 2.0f, cell);
    CHECK(!contact.hit, "a trace that ends before the cell does not reach it");
}

void testStartingInsideReportsPenetration()
{
    const Aabb cell = Aabb::unitCell(0, 0, 0);

    /* Deep in x, shallow in y: the separation must come out along y, because
     * that is the cheaper way out. Pushing along x would move it three times as
     * far and, in a real world, through whatever is behind. */
    const SweepContact contact = sweepBox(Vec3{ 0.5f, 0.9f, 0.5f }, Vec3::zero(),
                                          Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit && contact.startPenetrating, "an overlapping start is reported");
    CHECK(nearly(contact.distance, 0.0f), "at zero distance");
    CHECK(nearly(contact.normal, Vec3{ 0.0f, 1.0f, 0.0f }),
          "and the separation is the shallowest axis, not the travel axis");
}

void testDiagonalSweepPicksTheCorrectEntryAxis()
{
    const Aabb cell = Aabb::unitCell(2, 2, 0);

    /* Travelling diagonally at 45 degrees from the origin corner. The box enters
     * through x and y at the same instant here, so what is being pinned down is
     * that the entry distance is right; a normal on either axis is defensible at
     * an exact corner. */
    const Vec3 direction = Vec3{ 1.0f, 1.0f, 0.0f }.normalised();
    const SweepContact contact = sweepBox(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3::zero(),
                                          direction, 10.0f, cell);

    CHECK(contact.hit, "the diagonal reaches the cell");
    CHECK(nearly(contact.distance, 1.5f * std::sqrt(2.0f)),
          "distance is measured along the ray, not along an axis");
}

/* ---- sphere sweeps ----------------------------------------------------- */

void testSphereMatchesBoxOnAFace()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);

    /* Straight at the middle of a face: the rounded corners never come into it,
     * so the sphere and its bounding box must agree exactly. If they do not, the
     * refinement is firing when it should not. */
    const SweepContact sphere = sweepSphere(Vec3{ 0.5f, 0.5f, 0.5f }, 0.25f,
                                            Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);
    CHECK(sphere.hit, "the sphere reaches the cell");
    CHECK(nearly(sphere.distance, 2.25f), "and stops exactly where the box did");
}

void testSphereRoundsOffTheCorner()
{
    const Aabb cell = Aabb::unitCell(1, 1, 0);  /* corner at (1,1) */

    /* Aimed at the corner along the diagonal. The bounding box would touch when
     * the centre reached (0.75, 0.75) — the square corner of the expanded box.
     * The true sphere touches later, when the centre is `radius` from the corner
     * point itself, at (1,1) - (r/sqrt2)(1,1) = (0.8232, 0.8232).
     *
     * THE DIFFERENCE IS THE ENTIRE REASON THE REFINEMENT EXISTS. A box-only
     * answer stops a sphere short of a corner by up to 41% of its radius, which
     * reads as an invisible wall on every outside corner in the level. */
    const Vec3 direction = Vec3{ 1.0f, 1.0f, 0.0f }.normalised();
    const float radius = 0.25f;

    const SweepContact contact = sweepSphere(Vec3{ 0.0f, 0.0f, 0.5f }, radius,
                                             direction, 10.0f, cell);
    CHECK(contact.hit, "the sphere reaches the corner");

    const float centreToCorner = (Vec3{ 1.0f, 1.0f, 0.5f } - contact.end).length();
    CHECK(nearly(centreToCorner, radius, 1.0e-3f),
          "at contact the centre is exactly one radius from the corner");

    const float boxAnswer = (Vec3{ 0.75f, 0.75f, 0.5f } - Vec3{ 0.0f, 0.0f, 0.5f }).length();
    CHECK(contact.distance > boxAnswer + 1.0e-3f,
          "which is strictly later than the bounding box would have said");
}

void testSpherePassesACornerTheBoxWouldCatch()
{
    const Aabb cell = Aabb::unitCell(1, 1, 0);  /* corner edge along z at (1,1) */
    const float radius = 0.25f;

    /* THE FALSE POSITIVE THE REFINEMENT REMOVES, and it needs two perpendicular
     * axes to show up. Sweeping along +z past the cell's vertical corner edge,
     * offset diagonally by 0.22 on both x and y:
     *
     *   the expanded box reaches to 0.75 on each axis, and 0.78 is inside it,
     *   so a box-bounded sweep reports a hit;
     *   the true distance to the corner edge is 0.22 * sqrt(2) = 0.311, which is
     *   more than the radius, so the sphere passes cleanly.
     *
     * Approach it along ONE axis instead and the two agree exactly — which is
     * why the earlier face test is not enough to catch this. */
    const float offset = 0.22f;
    const Vec3 begin{ 1.0f - offset, 1.0f - offset, -2.0f };

    const SweepContact sphere = sweepSphere(begin, radius, Vec3{ 0.0f, 0.0f, 1.0f },
                                            10.0f, cell);
    const SweepContact box = sweepBox(begin, Vec3{ radius, radius, radius },
                                      Vec3{ 0.0f, 0.0f, 1.0f }, 10.0f, cell);

    CHECK(box.hit, "the bounding box does catch on the corner");
    CHECK(!sphere.hit, "and the sphere correctly passes it");

    /* Pulled in to where it genuinely does clip the edge, the sphere must find
     * it — a refinement that rejected everything would pass the check above. */
    const float clipping = 0.10f;
    const SweepContact grazing = sweepSphere(Vec3{ 1.0f - clipping, 1.0f - clipping, -2.0f },
                                             radius, Vec3{ 0.0f, 0.0f, 1.0f }, 10.0f, cell);
    CHECK(grazing.hit, "a sphere that really does clip the corner still hits it");
}

/* ---- capsule sweeps ---------------------------------------------------- */

void testCapsuleWithNoSegmentIsASphere()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);
    const float radius = 0.25f;

    /* halfHeight clamped up to the radius, so the segment has zero length and
     * the capsule IS a sphere. The two must agree to the bit, because the
     * capsule path is literally the sphere path with a zero stretch — if these
     * ever differ, the mapping back has introduced something. */
    const Vec3 start{ 0.5f, 0.5f, 0.5f };
    const Vec3 direction{ 1.0f, 0.0f, 0.0f };

    const SweepContact sphere = sweepSphere(start, radius, direction, 10.0f, cell);
    const SweepContact capsule = sweepCapsule(start, radius, radius, direction, 10.0f, cell);

    CHECK(sphere.hit && capsule.hit, "both reach the cell");
    CHECK(nearly(sphere.distance, capsule.distance),
          "a zero-segment capsule is exactly a sphere");
}

void testCapsuleSideIsItsRadiusNotItsHeight()
{
    const Aabb cell = Aabb::unitCell(3, 0, 0);

    /* Travelling horizontally into a face. A tall capsule stops at its RADIUS,
     * not at its half-height — the classic mistake is to sweep the bounding box,
     * which for a 0.9 half-height character would stop it nearly a metre short
     * of every wall. */
    const SweepContact contact = sweepCapsule(Vec3{ 0.5f, 0.5f, 0.5f }, 0.25f, 0.9f,
                                              Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit, "the capsule reaches the wall");
    CHECK(nearly(contact.distance, 2.25f), "and stops one radius short of it");
    CHECK(nearly(contact.normal, Vec3{ -1.0f, 0.0f, 0.0f }), "with the face normal");

    /* The bounding box's answer, for contrast — what a sweep that ignored the
     * shape would have said. */
    const SweepContact asBox = sweepBox(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3{ 0.25f, 0.9f, 0.25f },
                                        Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);
    CHECK(nearly(asBox.distance, 2.25f), "which happens to agree on a flat face");
}

void testCapsuleStandsOnItsCap()
{
    const Aabb cell = Aabb::unitCell(0, 0, 0);  /* top face at y = 1 */

    /* Dropping straight down: the capsule's foot is halfHeight below its centre,
     * so the centre comes to rest at 1 + halfHeight. Using the segment length
     * instead of the half-height here — forgetting to add the cap back — is the
     * "character sinks to its ankles" bug. */
    const float radius = 0.3f;
    const float halfHeight = 0.9f;

    const SweepContact contact = sweepCapsule(Vec3{ 0.5f, 5.0f, 0.5f }, radius, halfHeight,
                                              Vec3{ 0.0f, -1.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit, "the capsule lands");
    CHECK(nearly(contact.end.y, 1.0f + halfHeight), "with its centre a half-height up");
    CHECK(nearly(contact.point.y, 1.0f), "and the contact on the surface it stands on");
    CHECK(nearly(contact.normal, Vec3{ 0.0f, 1.0f, 0.0f }), "normal up");
}

void testCapsuleRoundsTheTopEdgeOfAStep()
{
    /* THE CASE THAT MOTIVATES AN EXACT CAPSULE AT ALL — a foot catching a step
     * edge. A knee-high block, and a capsule walking into it slightly above the
     * top: its lower cap must round over the edge rather than stopping flat
     * against a phantom vertical face, and the contact must be reported on the
     * REAL block rather than on the stretched box the maths goes through.
     *
     * The block's top is at y = 1. The capsule's centre travels at y = 1.15 with
     * a radius of 0.3, so its cap dips to 0.85 — below the top — and the contact
     * is on the top edge at x = 1. */
    const Aabb block = Aabb::unitCell(1, 0, 0);
    const float radius = 0.3f;

    const SweepContact contact = sweepCapsule(Vec3{ -1.0f, 1.15f, 0.5f }, radius, 0.9f,
                                              Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, block);

    CHECK(contact.hit, "the capsule meets the step");
    CHECK(contact.point.y <= 1.0f + 1.0e-4f,
          "and the contact is on the real block, not on the stretched one (%.3f)",
          static_cast<double>(contact.point.y));
    CHECK(contact.point.x >= 1.0f - 1.0e-3f && contact.point.x <= 1.0f + 1.0e-3f,
          "at the block's leading face");
}

void testCapsulePenetratingIsReported()
{
    const Aabb cell = Aabb::unitCell(0, 0, 0);

    /* Standing with its centre inside the block — what happens when a floor is
     * raised under a character, or one spawns badly. */
    const SweepContact contact = sweepCapsule(Vec3{ 0.5f, 0.5f, 0.5f }, 0.3f, 0.9f,
                                              Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f, cell);

    CHECK(contact.hit && contact.startPenetrating, "an overlapping capsule is reported");
    CHECK(nearly(contact.normal.length(), 1.0f), "with a usable push direction");
}

void testCapsuleDispatchesThroughSweepShape()
{
    const Aabb cell = Aabb::unitCell(0, 0, 0);
    const TraceShape shape = TraceShape::capsule(0.3f, 0.9f);

    CHECK(shape.kind() == TraceShape::Kind::Capsule, "it is a capsule");
    CHECK(nearly(shape.segmentHalf(), 0.6f), "whose inner segment is halfHeight minus radius");
    CHECK(nearly(shape.halfExtents().y, 0.9f), "and whose bounds are its half-height");

    const SweepContact viaShape = sweepShape(shape, Vec3{ 0.5f, 5.0f, 0.5f },
                                             Vec3{ 0.0f, -1.0f, 0.0f }, 10.0f, cell);
    const SweepContact direct = sweepCapsule(Vec3{ 0.5f, 5.0f, 0.5f }, 0.3f, 0.9f,
                                             Vec3{ 0.0f, -1.0f, 0.0f }, 10.0f, cell);
    CHECK(nearly(viaShape.distance, direct.distance), "and the dispatch reaches it");

    /* A half-height below the radius is clamped, not accepted — see
     * TraceShape::capsule. */
    const TraceShape squashed = TraceShape::capsule(0.5f, 0.2f);
    CHECK(nearly(squashed.halfHeight(), 0.5f), "a too-short capsule becomes a sphere");
    CHECK(nearly(squashed.segmentHalf(), 0.0f), "with no segment left");
}

/* ---- grid traversal ---------------------------------------------------- */

void testRayVisitsEveryCellOnce()
{
    /* 4.2 m from x = 0.5 reaches into cell 4 and stops short of the boundary at
     * x = 5. Deliberately not 4.5, which would land the ray's end EXACTLY on
     * that boundary — a ray that finishes flush against a face does touch it,
     * so the traversal reports the cell beyond, and a test sitting on that
     * knife edge is testing the tie-break rather than the walk. */
    std::vector<GridCell> visited;
    traceGrid(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3{ 1.0f, 0.0f, 0.0f }, 4.2f, Vec3::zero(),
              [&](const GridCell& cell) { visited.push_back(cell); return false; });

    CHECK(visited.size() == 5, "a 4.2 m ray along x from 0.5 crosses five cells");
    for (std::size_t index = 0; index < visited.size(); ++index) {
        CHECK(visited[index].x == static_cast<int>(index), "in order, no gaps");
        CHECK(visited[index].y == 0 && visited[index].z == 0, "and no drift off-axis");
    }
}

void testTraversalIsOrderedByDistance()
{
    std::vector<float> distances;
    const Vec3 direction = Vec3{ 1.0f, 0.37f, 0.11f }.normalised();
    traceGrid(Vec3{ 0.5f, 0.5f, 0.5f }, direction, 12.0f, Vec3::zero(),
              [&](const GridCell& cell) { distances.push_back(cell.slabDistance); return false; });

    CHECK(distances.size() > 12, "an off-axis ray crosses cells on every axis");
    bool ordered = true;
    for (std::size_t index = 1; index < distances.size(); ++index) {
        if (distances[index] < distances[index - 1] - 1.0e-5f) ordered = false;
    }
    CHECK(ordered, "slab distances never go backwards");
}

void testTraversalVisitsNoCellTwice()
{
    /* THE PROPERTY THE WHOLE SLAB SCHEME RESTS ON. A box wide enough to span
     * several cells, travelling diagonally so that all three axes step — the
     * arrangement where a naive implementation revisits corner cells. */
    std::vector<long long> keys;
    const Vec3 direction = Vec3{ 1.0f, 0.6f, 0.3f }.normalised();
    traceGrid(Vec3{ 0.5f, 0.5f, 0.5f }, direction, 15.0f, Vec3{ 0.7f, 0.7f, 0.7f },
              [&](const GridCell& cell) {
                  keys.push_back((static_cast<long long>(cell.x + 64) << 20)
                                 | (static_cast<long long>(cell.y + 64) << 10)
                                 | static_cast<long long>(cell.z + 64));
                  return false;
              });

    std::vector<long long> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    const bool unique = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    CHECK(unique, "no cell is enumerated twice (%zu cells)", keys.size());
}

void testBoxTraversalCoversItsFootprint()
{
    /* A box 1.4 cells across, standing still at the origin corner region: it
     * straddles two cells on each axis, so the starting footprint alone is
     * eight cells. Reporting fewer means a sweep that begins overlapping
     * geometry would not notice. */
    int count = 0;
    traceGrid(Vec3{ 1.0f, 1.0f, 1.0f }, Vec3{ 0.0f, 0.0f, 0.0f }, 0.0f,
              Vec3{ 0.2f, 0.2f, 0.2f },
              [&](const GridCell&) { ++count; return false; });
    CHECK(count == 8, "a box straddling all three axis boundaries occupies eight cells");
}

void testTraversalStopsWhenTheVisitorSaysSo()
{
    int count = 0;
    traceGrid(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3{ 1.0f, 0.0f, 0.0f }, 100.0f, Vec3::zero(),
              [&](const GridCell&) { ++count; return count == 3; });
    CHECK(count == 3, "the walk ends the moment the visitor returns true");
}

void testTraversalTerminatesOnADegenerateDirection()
{
    int count = 0;
    traceGrid(Vec3{ 0.5f, 0.5f, 0.5f }, Vec3::zero(), 100.0f, Vec3::zero(),
              [&](const GridCell&) { ++count; return false; });
    CHECK(count == 1, "a zero direction visits the starting cell and stops");
}

/* ---- the multi-hit buffer ---------------------------------------------- */

TraceHit hitAt(float distance, Response response = Response::Overlap)
{
    TraceHit hit;
    hit.distance = distance;
    hit.response = response;
    return hit;
}

void testHitsStaySorted()
{
    TraceHitBuffer<8> storage;
    TraceHits hits = storage.view();

    hits.add(hitAt(5.0f));
    hits.add(hitAt(1.0f));
    hits.add(hitAt(3.0f));

    CHECK(hits.count() == 3, "all three kept");
    CHECK(nearly(hits[0].distance, 1.0f) && nearly(hits[1].distance, 3.0f)
              && nearly(hits[2].distance, 5.0f),
          "and sorted by distance whatever order they arrived in");
}

void testOverflowKeepsTheNearest()
{
    TraceHitBuffer<2> storage;
    TraceHits hits = storage.view();

    hits.add(hitAt(1.0f));
    hits.add(hitAt(9.0f));
    CHECK(!hits.overflowed(), "not full yet");

    hits.add(hitAt(4.0f));
    CHECK(hits.overflowed(), "overflow is reported, not hidden");
    CHECK(hits.count() == 2, "still bounded");
    CHECK(nearly(hits[0].distance, 1.0f) && nearly(hits[1].distance, 4.0f),
          "and what survives is the nearest, not the first to arrive");

    hits.add(hitAt(20.0f));
    CHECK(nearly(hits[1].distance, 4.0f), "a farther hit does not displace a nearer one");
}

void testBlockingIsTheFirstBlockNotTheFirstHit()
{
    TraceHitBuffer<8> storage;
    TraceHits hits = storage.view();

    hits.add(hitAt(1.0f, Response::Overlap));
    hits.add(hitAt(2.0f, Response::Overlap));
    hits.add(hitAt(3.0f, Response::Block));
    hits.add(hitAt(4.0f, Response::Block));

    const TraceHit* blocking = hits.blocking();
    CHECK(blocking != nullptr, "there is a blocking hit");
    CHECK(nearly(blocking->distance, 3.0f), "and it is the nearest block, past the overlaps");

    TraceHitBuffer<4> passThrough;
    TraceHits open = passThrough.view();
    open.add(hitAt(1.0f, Response::Overlap));
    CHECK(open.blocking() == nullptr, "a trace that blocked at nothing reports nothing");
}

}  // namespace

int main()
{
    testLayerMaskSetOperations();
    testMatrixIsSymmetricAndReplaceable();
    testFilterComesFromTheMatrix();
    testMatrixDescribesItself();

    testRayHitsBoxFaceExactly();
    testBoxSweepStopsShortByItsHalfExtent();
    testSweepMissesWhenItPassesBeside();
    testSweepRespectsMaxDistance();
    testStartingInsideReportsPenetration();
    testDiagonalSweepPicksTheCorrectEntryAxis();

    testSphereMatchesBoxOnAFace();
    testSphereRoundsOffTheCorner();
    testSpherePassesACornerTheBoxWouldCatch();

    testCapsuleWithNoSegmentIsASphere();
    testCapsuleSideIsItsRadiusNotItsHeight();
    testCapsuleStandsOnItsCap();
    testCapsuleRoundsTheTopEdgeOfAStep();
    testCapsulePenetratingIsReported();
    testCapsuleDispatchesThroughSweepShape();

    testRayVisitsEveryCellOnce();
    testTraversalIsOrderedByDistance();
    testTraversalVisitsNoCellTwice();
    testBoxTraversalCoversItsFootprint();
    testTraversalStopsWhenTheVisitorSaysSo();
    testTraversalTerminatesOnADegenerateDirection();

    testHitsStaySorted();
    testOverflowKeepsTheNearest();
    testBlockingIsTheFirstBlockNotTheFirstHit();

    if (g_failures == 0) {
        std::printf("collision: all checks passed\n");
        return 0;
    }
    std::printf("collision: %d check(s) failed\n", g_failures);
    return 1;
}
