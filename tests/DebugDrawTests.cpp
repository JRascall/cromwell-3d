/* DebugDrawTests.cpp — the parts of debug drawing that cannot be judged by
 * looking.
 *
 * MOST OF THIS SUBSYSTEM IS VISUAL AND BELONGS IN FRONT OF A HUMAN. Whether a
 * debug sphere reads as a sphere, whether the arrow heads are the right size,
 * whether the colours are legible over a lit scene — no assertion will ever
 * answer any of that, and pretending otherwise produces tests that pass while
 * the thing is unusable.
 *
 * What IS worth pinning down is the part with no visible symptom until it is
 * far too late:
 *
 *   LIFETIME. The default is "one frame", and it is one frame because of the
 *   ORDER of advance() against the draw. Get that backwards and every line
 *   vanishes before it is ever shown — which reads as "debug drawing does not
 *   work" rather than as an ordering bug, and is exactly the sort of thing that
 *   gets a working facility abandoned.
 *
 *   THE CAPSULE'S CAP CENTRES. The drawing must sit at halfHeight minus radius,
 *   the same quantity TraceShape::segmentHalf reports. If it does not, the
 *   picture of the swept shape is a different size from the shape that was
 *   swept — and then the drawing, which exists to be believed, lies.
 *
 *   OVERFLOW. A debug facility that silently stopped drawing would be read as
 *   "the thing I am debugging stopped happening".
 */
#include "cromwell/collision/Shape.hpp"
#include "cromwell/debug/DebugDraw.hpp"

#include <cmath>
#include <cstdio>

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

/* The queue is a process-wide singleton, so every test starts from empty
 * rather than from whatever the last one left. */
DebugDraw& fresh()
{
    DebugDraw& draw = DebugDraw::get();
    draw.clear();
    return draw;
}

void testDefaultLifetimeIsExactlyOneFrame()
{
    DebugDraw& draw = fresh();
    draw.line(Vec3::zero(), Vec3::up(), debugColour::white());

    /* Present for the frame it was added in — this is when the renderer sees
     * it. */
    CHECK(draw.segments().size() == 1, "the line is there to be drawn");

    draw.advance(1.0f / 60.0f);
    CHECK(draw.empty(), "and gone by the next frame, without being asked");
}

void testAPositiveLifetimePersists()
{
    DebugDraw& draw = fresh();
    draw.line(Vec3::zero(), Vec3::up(), debugColour::white(), 0.5f);

    for (int frame = 0; frame < 29; ++frame) draw.advance(1.0f / 60.0f);
    CHECK(!draw.empty(), "half a second of line survives 29 frames at 60 Hz");

    for (int frame = 0; frame < 3; ++frame) draw.advance(1.0f / 60.0f);
    CHECK(draw.empty(), "and expires shortly after");
}

void testShapesTessellateToLines()
{
    DebugDraw& draw = fresh();

    draw.box(Vec3::zero(), Vec3::one(), debugColour::white());
    CHECK(draw.segments().size() == 12, "a box is twelve edges (%zu)",
          draw.segments().size());

    fresh().point(Vec3::zero(), 1.0f, debugColour::white());
    CHECK(draw.segments().size() == 3, "a point is three crossed lines");

    fresh().sphere(Vec3::zero(), 1.0f, debugColour::white(), 0.0f, /*segments=*/12);
    CHECK(draw.segments().size() == 36, "a sphere is three twelve-sided circles (%zu)",
          draw.segments().size());

    /* An arrow is its shaft and four barbs — and a DEGENERATE one is just the
     * shaft, because normalising a zero-length direction to build the barbs is
     * where a NaN would come from. */
    fresh().arrow(Vec3::zero(), Vec3{ 0.0f, 5.0f, 0.0f }, debugColour::white());
    CHECK(draw.segments().size() == 5, "an arrow is a shaft and four barbs");

    fresh().arrow(Vec3::zero(), Vec3::zero(), debugColour::white());
    CHECK(draw.segments().size() == 1, "a zero-length arrow is just the shaft");
    for (const DebugSegment& segment : draw.segments()) {
        CHECK(std::isfinite(segment.to.x) && std::isfinite(segment.to.y)
                  && std::isfinite(segment.to.z),
              "and produces no NaN");
    }
}

void testCapsuleDrawingMatchesTheShapeItDepicts()
{
    const float radius = 0.3f;
    const float halfHeight = 0.9f;

    DebugDraw& draw = fresh();
    draw.capsule(Vec3::zero(), radius, halfHeight, debugColour::white(), 0.0f,
                 /*segments=*/8);

    /* The four uprights run between the cap centres, so the tallest and
     * shortest points of any SIDE line give away where the caps were placed. */
    float highest = -1000.0f;
    for (const DebugSegment& segment : draw.segments()) {
        highest = std::max(highest, std::max(segment.from.y, segment.to.y));
    }

    /* THE NUMBER THAT MUST AGREE WITH THE SWEEP. The topmost drawn geometry is
     * the upper cap's arc, which reaches a full radius above the cap centre —
     * so the outside of the capsule, which is halfHeight. A drawing that used
     * halfHeight as the cap centre would reach halfHeight + radius and depict
     * something a radius taller than what gets swept. */
    CHECK(nearly(highest, halfHeight, 1.0e-3f),
          "the drawn capsule is exactly as tall as its half-height (%.3f)",
          static_cast<double>(highest));

    const TraceShape shape = TraceShape::capsule(radius, halfHeight);
    CHECK(nearly(shape.segmentHalf(), halfHeight - radius),
          "and the shape it depicts puts its cap centres in the same place");

    /* A capsule squashed below its own radius is a sphere, in the drawing as in
     * the sweep — the two clamp identically or the picture disagrees again. */
    fresh().capsule(Vec3::zero(), 0.5f, 0.2f, debugColour::white(), 0.0f, 8);
    float squashedHighest = -1000.0f;
    for (const DebugSegment& segment : draw.segments()) {
        squashedHighest = std::max(squashedHighest, std::max(segment.from.y, segment.to.y));
    }
    CHECK(nearly(squashedHighest, 0.5f, 1.0e-3f), "a squashed capsule draws as a sphere");
}

void testOverflowIsReportedRatherThanSilent()
{
    DebugDraw& draw = fresh();

    for (int index = 0; index < DebugDraw::kMaxSegments + 100; ++index) {
        draw.line(Vec3::zero(), Vec3::up(), debugColour::white());
    }

    CHECK(static_cast<int>(draw.segments().size()) == DebugDraw::kMaxSegments,
          "the queue is bounded");
    CHECK(draw.dropped() == 100, "and says exactly how much it threw away (%d)",
          draw.dropped());

    draw.advance(1.0f);
    CHECK(draw.dropped() == 0, "the count resets with the frame, so it stays actionable");
}

void testDepthTestDefaultsToXray()
{
    DebugDraw& draw = fresh();
    draw.line(Vec3::zero(), Vec3::up(), debugColour::white());

    /* The default matters: the usual reason to draw a line is that something
     * went somewhere it should not have, and the geometry it went into is
     * exactly what would hide it. */
    CHECK(!draw.segments()[0].depthTested, "debug lines draw through geometry by default");

    fresh().line(Vec3::zero(), Vec3::up(), debugColour::white(), 0.0f, /*depthTested=*/true);
    CHECK(draw.segments()[0].depthTested, "and occlusion is available when it is the point");
}

}  // namespace

int main()
{
    testDefaultLifetimeIsExactlyOneFrame();
    testAPositiveLifetimePersists();
    testShapesTessellateToLines();
    testCapsuleDrawingMatchesTheShapeItDepicts();
    testOverflowIsReportedRatherThanSilent();
    testDepthTestDefaultsToXray();

    if (g_failures == 0) {
        std::printf("debug draw: all checks passed\n");
        return 0;
    }
    std::printf("debug draw: %d check(s) failed\n", g_failures);
    return 1;
}
