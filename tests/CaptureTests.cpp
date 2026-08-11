/* CaptureTests.cpp — when a second camera actually redraws.
 *
 * THE ONLY PART OF SCENE CAPTURE THAT CAN BE TESTED WITHOUT A GPU, and by some
 * distance the part most worth testing. Whether the picture looks right is a
 * thing to look at; whether the capture runs sixty times a second when it was
 * asked to run five is a performance bug with no visual symptom at all — the
 * minimap looks perfect and the frame rate is halved, and nothing points at the
 * minimap.
 *
 * Three rules are pinned down here, and each of them has cost somebody a day
 * somewhere:
 *
 *   THE FIRST CAPTURE ALWAYS HAPPENS. Otherwise an on-demand capture is a black
 *   rectangle until something asks, which reads as a broken feature.
 *
 *   AN EXPLICIT REQUEST BEATS THE TIMER. "The world changed, redraw now" must
 *   not mean "redraw within the next fifth of a second".
 *
 *   A LATE FRAME DOES NOT BANK CREDIT. Subtracting the interval instead of
 *   resetting it lets a hitch queue up several catch-up captures — the worst
 *   possible response to already being behind.
 */
#include "cromwell/gpu/target/CaptureSchedule.hpp"
#include "cromwell/collision/Layer.hpp"
#include "cromwell/overlay/ViewLayers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

constexpr float kFrame = 1.0f / 60.0f;

bool nearly(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

/* How many of `frames` ticks actually captured, first one included. */
int capturesOver(CaptureSchedule& schedule, int frames, float deltaSeconds = kFrame)
{
    int count = 0;
    for (int frame = 0; frame < frames; ++frame) {
        if (schedule.tick(deltaSeconds)) ++count;
    }
    return count;
}

void testEveryFrameCapturesEveryFrame()
{
    CaptureSchedule schedule = CaptureSchedule::everyFrame();
    CHECK(capturesOver(schedule, 10) == 10, "every-frame means every frame");
}

void testTheFirstCaptureAlwaysHappens()
{
    /* ON DEMAND, never asked. Without this rule the texture is black and the
     * feature looks broken rather than idle. */
    CaptureSchedule onDemand = CaptureSchedule::onDemand();
    CHECK(onDemand.tick(kFrame), "an on-demand capture still renders once, immediately");
    CHECK(onDemand.everCaptured(), "and says so");
    CHECK(!onDemand.tick(kFrame), "then never again unasked");

    /* And on an interval, so a minimap is not black for its first fifth of a
     * second every time it opens. */
    CaptureSchedule interval = CaptureSchedule::interval(0.2f);
    CHECK(interval.tick(kFrame), "an interval capture renders on its first frame");
}

void testIntervalCapturesAtItsRate()
{
    CaptureSchedule schedule = CaptureSchedule::interval(0.2f);

    /* One second at 60 Hz. The first frame captures, then one per 0.2 s: five
     * more at 0.2, 0.4, 0.6, 0.8 and 1.0 — six in total rather than sixty. */
    const int captures = capturesOver(schedule, 60);
    CHECK(captures >= 5 && captures <= 7,
          "a fifth-of-a-second interval captures about six times a second, not sixty (%d)",
          captures);
}

void testAnExplicitRequestBeatsTheTimer()
{
    CaptureSchedule schedule = CaptureSchedule::interval(10.0f);
    schedule.tick(kFrame);                       /* the free first one */
    CHECK(!schedule.tick(kFrame), "and then a long wait");

    /* A WALL WAS BLOWN OUT. No timer can know that, so the caller says so — and
     * it has to take effect on this frame, not in ten seconds. */
    schedule.request();
    CHECK(schedule.tick(kFrame), "a request renders on the very next tick");
    CHECK(!schedule.tick(kFrame), "and is consumed, not sticky");

    /* Same for on-demand, which is the mode built around it. */
    CaptureSchedule onDemand = CaptureSchedule::onDemand();
    onDemand.tick(kFrame);
    onDemand.request();
    CHECK(onDemand.tick(kFrame), "an on-demand capture renders when asked");
    CHECK(!onDemand.tick(kFrame), "once");
}

void testALateFrameDoesNotBankCatchUpCaptures()
{
    CaptureSchedule schedule = CaptureSchedule::interval(0.1f);
    schedule.tick(kFrame);  /* the first */

    /* A HALF-SECOND HITCH — five intervals' worth in one frame. The wrong
     * implementation subtracts the interval and owes itself four more captures,
     * so the frames right after a stutter each run a second scene pass. That
     * turns one bad frame into five. */
    CHECK(schedule.tick(0.5f), "the frame after a long stall captures");

    int extra = 0;
    for (int frame = 0; frame < 4; ++frame) {
        if (schedule.tick(kFrame)) ++extra;
    }
    CHECK(extra == 0, "and does not then fire four catch-up captures (%d)", extra);
}

void testPhaseStaggersSeveralCaptures()
{
    /* FOUR CAPTURES, THE CASE THE PHASE EXISTS FOR. Unstaggered they land on
     * the same frame forever: four frames in five cost nothing and the fifth
     * carries four extra scene passes. The average looks fine and the frame
     * time has a spike in it five times a second. */
    constexpr float kInterval = 0.2f;
    CaptureSchedule schedules[4] = {
        CaptureSchedule::interval(kInterval, 0.00f),
        CaptureSchedule::interval(kInterval, 0.05f),
        CaptureSchedule::interval(kInterval, 0.10f),
        CaptureSchedule::interval(kInterval, 0.15f),
    };

    /* Past the free first capture, which every schedule takes on frame one by
     * design — see testTheFirstCaptureAlwaysHappens. */
    for (CaptureSchedule& schedule : schedules) schedule.tick(kFrame);

    int busiestFrame = 0;
    int total = 0;
    for (int frame = 0; frame < 120; ++frame) {  /* two seconds at 60 Hz */
        int thisFrame = 0;
        for (CaptureSchedule& schedule : schedules) {
            if (schedule.tick(kFrame)) ++thisFrame;
        }
        busiestFrame = std::max(busiestFrame, thisFrame);
        total += thisFrame;
    }

    CHECK(busiestFrame == 1, "at most one staggered capture runs on any frame (%d)",
          busiestFrame);

    /* And the phase shifted WHEN, not how often — four captures at 5 Hz over
     * two seconds is about forty. */
    CHECK(total >= 34 && total <= 46, "while each still runs at its own rate (%d)", total);
}

void testAnOversizedPhaseCannotRunAway()
{
    /* A phase larger than the interval must wrap rather than pre-load the timer
     * past its threshold — otherwise the schedule starts already overdue and
     * fires on every frame, which is the opposite of what staggering is for. */
    CaptureSchedule schedule = CaptureSchedule::interval(0.1f, 5.0f);
    schedule.tick(kFrame);  /* the free first one */

    /* One second at 60 Hz on a tenth-of-a-second interval is about ten, plus at
     * most one extra as the wrapped phase settles. Sixty would mean the phase
     * had left the timer permanently overdue. */
    const int captures = capturesOver(schedule, 60);
    CHECK(captures >= 8 && captures <= 12,
          "an oversized phase still captures at its interval, not every frame (%d)",
          captures);
}

void testInvalidateForcesARedraw()
{
    CaptureSchedule schedule = CaptureSchedule::onDemand();
    schedule.tick(kFrame);
    CHECK(!schedule.tick(kFrame), "settled");

    /* The target was resized, so whatever it holds is meaningless. */
    schedule.invalidate();
    CHECK(!schedule.everCaptured(), "invalidate forgets there was ever a picture");
    CHECK(schedule.tick(kFrame), "so the next tick renders one");
}

void testAZeroIntervalIsEveryFrame()
{
    /* A degenerate interval must not divide by anything or stall — it is just
     * the every-frame case arrived at from the other direction. */
    CaptureSchedule schedule = CaptureSchedule::interval(0.0f);
    CHECK(capturesOver(schedule, 5) == 5, "a zero interval captures every frame");

    /* And a negative one is clamped rather than accepted. */
    CaptureSchedule negative = CaptureSchedule::interval(-3.0f);
    CHECK(negative.intervalSeconds() >= 0.0f, "a negative interval is clamped to zero");
}

/* ---- layers ------------------------------------------------------------ */

void testDrawLayersAreTheGamesAndFeaturesAreTheEngines()
{
    /* THE LINE THE SPLIT IS ABOUT. The engine names the features it implements;
     * it names no CATEGORIES of drawable thing, because a game embedding it may
     * have units or may have none. This test is here to fail if a game concept
     * ever creeps back into ViewLayers. */
    ViewLayers layers;

    CHECK(layers.features.shadows && layers.features.ambientOcclusion,
          "the engine's own passes are named fields");
    CHECK(layers.draw == DrawLayerMask::all(),
          "and a project that registers nothing still draws everything");

    /* Draw layers are opaque to the engine — it can only mask them. */
    const DrawLayerId anything{ 7 };
    CHECK(layers.drawing(anything), "unknown categories default to drawn");
    layers.hide(anything);
    CHECK(!layers.drawing(anything), "and can be masked off without the engine knowing "
                                     "what they are");
    layers.show(anything);
    CHECK(layers.drawing(anything), "and back on");
}

void testMaskIdsOfDifferentKindsCannotBeConfused()
{
    /* A collision layer and a draw layer are both "index into 32", and mixing
     * them would be a bug no runtime check could catch. They are distinct types
     * for exactly that reason — this test documents the guarantee; the compiler
     * enforces it. */
    const DrawLayerId draw{ 3 };
    const LayerId collision{ 3 };
    CHECK(draw.index() == collision.index(), "the same index");
    CHECK(!DrawLayerMask::none().has(draw), "in unrelated sets");
    CHECK(!LayerMask::none().has(collision), "that share no bits");

    /* The default id matches nothing, so something never assigned a layer
     * cannot pass a filter that happens to include category zero. */
    CHECK(!DrawLayerMask::all().has(DrawLayerId{}), "an unset id is in no mask, not even all");
}

void testUnshadedDropsEveryFeatureAndKeepsEveryLayer()
{
    const ViewLayers layers = ViewLayers::unshaded();
    CHECK(!layers.features.shadows && !layers.features.ambientOcclusion
              && !layers.features.decals && !layers.features.sky,
          "unshaded turns off the engine's passes");
    CHECK(layers.draw == DrawLayerMask::all(), "but still draws the world");

    /* THE DERIVED SWITCH, which is what replaced a separate capability flag.
     * A camera asking for either screen-space feature is one that needs a depth
     * prepass, and nothing else has to be remembered. */
    CHECK(ViewLayers::all().needsDepthPrepass(), "occlusion and decals need a prepass");
    CHECK(!layers.unshaded().needsDepthPrepass(),
          "and a camera wanting neither does not pay for one");
}

void testNamesDriveEnumeration()
{
    /* What makes a dev panel able to list a project's layers without being told
     * about them — and what makes a new project's layers appear in it for free. */
    DrawLayerNames names;
    names.name(DrawLayerId{ 0 }, "world");
    names.name(DrawLayerId{ 2 }, "units");

    int seen = 0;
    names.forEach([&](DrawLayerId, const std::string&) { ++seen; });
    CHECK(seen == 2, "only the registered categories are enumerated (%d)", seen);

    CHECK(names.find("units") == DrawLayerId{ 2 }, "and they are findable by name");
    CHECK(!names.find("nothing").valid(), "an unknown name is invalid, not category zero");

    /* Never blank: a nameless checkbox is worse than a placeholder. */
    CHECK(!names.labelOf(DrawLayerId{ 9 }).empty(), "an unregistered id still has a label");
}

void testSetPhaseRePhasesWithoutChangingTheRate()
{
    /* The collection re-phases what a caller built, so the setter has to wrap
     * the same way the constructor does — otherwise a set could hand a schedule
     * a phase past its interval and put it permanently overdue. */
    CaptureSchedule schedule = CaptureSchedule::interval(0.1f);
    schedule.setPhase(5.0f);

    CHECK(schedule.phase() < 0.1f, "an oversized phase is wrapped into the interval");
    CHECK(nearly(schedule.intervalSeconds(), 0.1f), "and the rate is untouched");

    schedule.tick(kFrame);  /* the free first one */
    const int captures = capturesOver(schedule, 60);
    CHECK(captures >= 8 && captures <= 12, "so it still captures at its own rate (%d)",
          captures);
}

}  // namespace

int main()
{
    testEveryFrameCapturesEveryFrame();
    testTheFirstCaptureAlwaysHappens();
    testIntervalCapturesAtItsRate();
    testAnExplicitRequestBeatsTheTimer();
    testALateFrameDoesNotBankCatchUpCaptures();
    testPhaseStaggersSeveralCaptures();
    testAnOversizedPhaseCannotRunAway();
    testSetPhaseRePhasesWithoutChangingTheRate();
    testDrawLayersAreTheGamesAndFeaturesAreTheEngines();
    testMaskIdsOfDifferentKindsCannotBeConfused();
    testUnshadedDropsEveryFeatureAndKeepsEveryLayer();
    testNamesDriveEnumeration();
    testInvalidateForcesARedraw();
    testAZeroIntervalIsEveryFrame();

    if (g_failures == 0) {
        std::printf("capture: all checks passed\n");
        return 0;
    }
    std::printf("capture: %d check(s) failed\n", g_failures);
    return 1;
}
