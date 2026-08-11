/* EasingTests.cpp — headless verification of the easing curves and the two
 * things that drive a value along one.
 *
 * WHAT IS ACTUALLY WORTH ASSERTING ABOUT A CURVE. Not its shape — that is a
 * judgement, and the way to check it is to look at it. What IS checkable is the
 * set of properties every curve has to satisfy to be usable at all, and which
 * are easy to break with a sign error in a formula nobody reads twice:
 *
 *   - it starts at 0 and ends at 1, exactly, or a fade never fully arrives;
 *   - the ordinary ones stay inside 0..1, or an alpha clips and a colour blows
 *     out;
 *   - the ordinary ones never go backwards, or motion visibly stutters;
 *   - the overshooting ones DO leave the range, or they are not what they claim.
 *
 * The Tween cases are about the failures that survive review: an animation that
 * settles at 99.99% of its target, and a retarget that jumps.
 */
#include "cromwell/math/Easing.hpp"
#include "cromwell/math/Tween.hpp"

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

/* Every curve in the enum. Kept as a list rather than iterating an integer
 * range so that adding a curve to the header without adding it here is a
 * visible omission rather than a silent gap in the sweep. */
const std::vector<Ease> kAllCurves = {
    Ease::Linear, Ease::SmoothStep, Ease::SmootherStep,
    Ease::QuadIn, Ease::QuadOut, Ease::QuadInOut,
    Ease::CubicIn, Ease::CubicOut, Ease::CubicInOut,
    Ease::QuintIn, Ease::QuintOut, Ease::QuintInOut,
    Ease::SineIn, Ease::SineOut, Ease::SineInOut,
    Ease::ExpoIn, Ease::ExpoOut, Ease::ExpoInOut,
    Ease::CircIn, Ease::CircOut, Ease::CircInOut,
    Ease::BackIn, Ease::BackOut, Ease::BackInOut,
    Ease::ElasticOut, Ease::BounceOut,
};

/* The ones that promise to stay within 0..1. Back and Elastic are excluded on
 * purpose — overshooting IS their job. */
bool staysInRange(Ease curve)
{
    switch (curve) {
    case Ease::BackIn:
    case Ease::BackOut:
    case Ease::BackInOut:
    case Ease::ElasticOut:
        return false;
    default:
        return true;
    }
}

/* Bounce and Elastic are monotonic in the large but not the small — they are
 * built out of reversals. Everything else must never go backwards. */
bool isMonotonic(Ease curve)
{
    switch (curve) {
    case Ease::BackIn:
    case Ease::BackOut:
    case Ease::BackInOut:
    case Ease::ElasticOut:
    case Ease::BounceOut:
        return false;
    default:
        return true;
    }
}

void everyCurveHitsItsEndpoints()
{
    /* The one that matters most. A curve that returns 0.998 at t=1 leaves a
     * fade permanently a shade short of opaque, and it is invisible until
     * someone screenshots it against the thing it was meant to match. The
     * exponential family is the usual offender — 2^-inf never reaches zero — so
     * its endpoints are pinned explicitly. */
    for (const Ease curve : kAllCurves) {
        CHECK(nearly(ease(curve, 0.0f), 0.0f), "%s does not start at 0 (%f)",
              toString(curve), ease(curve, 0.0f));
        CHECK(nearly(ease(curve, 1.0f), 1.0f), "%s does not end at 1 (%f)",
              toString(curve), ease(curve, 1.0f));
    }
}

void inputIsClampedNotExtrapolated()
{
    /* A caller whose clock overran should get the endpoint, not a value off the
     * end of the curve — which for the polynomial families grows without
     * bound. */
    for (const Ease curve : kAllCurves) {
        CHECK(nearly(ease(curve, -5.0f), 0.0f), "%s extrapolates below 0 (%f)",
              toString(curve), ease(curve, -5.0f));
        CHECK(nearly(ease(curve, 5.0f), 1.0f), "%s extrapolates above 1 (%f)",
              toString(curve), ease(curve, 5.0f));
    }
}

void ordinaryCurvesBehave()
{
    constexpr int kSamples = 200;

    for (const Ease curve : kAllCurves) {
        float previous = ease(curve, 0.0f);
        for (int step = 1; step <= kSamples; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(kSamples);
            const float value = ease(curve, t);

            if (staysInRange(curve)) {
                CHECK(value >= -1.0e-4f && value <= 1.0f + 1.0e-4f,
                      "%s left 0..1 at t=%f (%f)", toString(curve), t, value);
            }
            if (isMonotonic(curve)) {
                CHECK(value >= previous - 1.0e-4f,
                      "%s went backwards at t=%f (%f after %f)",
                      toString(curve), t, value, previous);
            }
            previous = value;
        }
    }
}

void overshootingCurvesOvershoot()
{
    /* If Back stopped overshooting there would be no reason to have it, and
     * nothing else in the suite would notice. */
    float highest = 0.0f;
    float lowest = 1.0f;
    for (int step = 0; step <= 200; ++step) {
        const float t = static_cast<float>(step) / 200.0f;
        highest = std::max(highest, ease(Ease::BackOut, t));
        lowest = std::min(lowest, ease(Ease::BackIn, t));
    }
    CHECK(highest > 1.01f, "back-out should overshoot its target, peaked at %f", highest);
    CHECK(lowest < -0.01f, "back-in should undershoot its start, dipped to %f", lowest);
}

void easedCurvesAreFasterOrSlowerWhereTheyClaim()
{
    /* The direction of the ease, which is the thing people get backwards. At
     * the midpoint an OUT curve has covered more than half the distance (it
     * front-loads), and an IN curve less. */
    CHECK(ease(Ease::CubicOut, 0.5f) > 0.6f,
          "cubic-out should be past halfway at t=0.5, got %f", ease(Ease::CubicOut, 0.5f));
    CHECK(ease(Ease::CubicIn, 0.5f) < 0.4f,
          "cubic-in should be short of halfway at t=0.5, got %f", ease(Ease::CubicIn, 0.5f));
    CHECK(nearly(ease(Ease::CubicInOut, 0.5f), 0.5f, 1.0e-3f),
          "an in-out curve should be exactly halfway at t=0.5, got %f",
          ease(Ease::CubicInOut, 0.5f));

    /* The UI kit's exponent form and this one are the same curve at exponent 2,
     * which is what lets the two coexist without being two different looks. */
    for (int step = 0; step <= 10; ++step) {
        const float t = static_cast<float>(step) / 10.0f;
        const float named = ease(Ease::QuadInOut, t);
        const float exponent = t < 0.5f ? 0.5f * std::pow(t * 2.0f, 2.0f)
                                        : 1.0f - 0.5f * std::pow((1.0f - t) * 2.0f, 2.0f);
        CHECK(nearly(named, exponent, 1.0e-4f),
              "quad-in-out and exponent 2 disagree at t=%f (%f vs %f)", t, named, exponent);
    }
}

void timelineRunsAndStops()
{
    Timeline timeline;
    timeline.withDuration(1.0f).withCurve(Ease::Linear).start();
    CHECK(timeline.running() && !timeline.finished(), "a started timeline should be running");

    for (int step = 0; step < 5; ++step) {
        timeline.advance(0.1f);
    }
    CHECK(nearly(timeline.progress(), 0.5f), "expected halfway, got %f", timeline.progress());

    timeline.advance(1.0f);
    CHECK(nearly(timeline.progress(), 1.0f), "a timeline should land exactly on 1, got %f",
          timeline.progress());
    CHECK(!timeline.running() && timeline.finished(), "a Once timeline should stop at its end");

    /* Zero duration completes immediately rather than dividing by it — the
     * "animate over n seconds, configurable to 0" case. */
    Timeline instant;
    instant.withDuration(0.0f).withCurve(Ease::CubicOut).start();
    CHECK(instant.finished() && nearly(instant.value(), 1.0f),
          "a zero-duration timeline should already be finished");
}

void timelineDelaysBeforeItMoves()
{
    Timeline timeline;
    timeline.withDuration(1.0f).withCurve(Ease::Linear)
            .withMode(Timeline::Mode::Once).withDelay(0.5f).start();

    timeline.advance(0.25f);
    CHECK(timeline.waiting() && nearly(timeline.progress(), 0.0f),
          "a delayed timeline should not have moved, at %f", timeline.progress());

    /* A frame that spans the end of the delay must spend the REMAINDER on the
     * animation, not throw it away — over a staggered list the dropped frames
     * add up to visible drift. */
    timeline.advance(0.5f);
    CHECK(!timeline.waiting(), "the delay should have expired");
    CHECK(nearly(timeline.progress(), 0.25f),
          "the leftover 0.25s should have been spent animating, at %f", timeline.progress());
}

void timelineRepeats()
{
    Timeline loop;
    loop.withDuration(1.0f).withCurve(Ease::Linear).withMode(Timeline::Mode::Loop).start();
    loop.advance(2.5f);
    CHECK(loop.running(), "a loop should never finish on its own");
    CHECK(nearly(loop.progress(), 0.5f), "a loop should wrap, at %f", loop.progress());

    /* A very long frame — a debugger pause, a stalled load — must land in the
     * right phase rather than wherever one subtraction left it. */
    Timeline pong;
    pong.withDuration(1.0f).withCurve(Ease::Linear).withMode(Timeline::Mode::PingPong).start();
    pong.advance(0.5f);
    CHECK(nearly(pong.progress(), 0.5f), "ping-pong should run forward first");
    pong.advance(1.0f);
    CHECK(nearly(pong.progress(), 0.5f), "ping-pong should have reflected, at %f",
          pong.progress());
    pong.advance(0.25f);
    CHECK(nearly(pong.progress(), 0.25f), "ping-pong should be travelling back, at %f",
          pong.progress());
}

void tweenArrivesExactly()
{
    TweenFloat value(0.0f);
    value.withDuration(1.0f).withCurve(Ease::CubicOut).moveTo(100.0f);

    value.advance(0.5f);
    CHECK(value.value() > 0.0f && value.value() < 100.0f,
          "the tween should be in flight, at %f", value.value());
    CHECK(value.moving(), "the tween should report that it is moving");

    value.advance(1.0f);
    CHECK(value.value() == 100.0f,
          "a tween must land EXACTLY on its target, got %.6f", value.value());
    CHECK(!value.moving(), "an arrived tween should stop moving");
}

void tweenRetargetsWithoutJumping()
{
    TweenFloat value(0.0f);
    value.withDuration(1.0f).withCurve(Ease::Linear).moveTo(100.0f);
    value.advance(0.5f);

    const float midFlight = value.value();
    value.moveTo(0.0f);

    /* The new move starts from where the value IS. Reading it immediately after
     * retargeting must not snap it back to where the last move began. */
    CHECK(nearly(value.value(), midFlight),
          "retargeting jumped the value from %f to %f", midFlight, value.value());

    value.advance(1.0f);
    CHECK(value.value() == 0.0f, "the retargeted tween should reach its new target, at %f",
          value.value());
}

void tweenToleratesBeingToldItsTargetEveryFrame()
{
    /* THE ONE THAT MATTERS FOR IMMEDIATE MODE. A caller with nowhere to put
     * "only when it changed" restates the target every frame; a tween that
     * restarted each time would sit on the first frame of its curve forever. */
    TweenFloat value(0.0f);
    value.withDuration(0.5f).withCurve(Ease::Linear);
    for (int frame = 0; frame < 60; ++frame) {
        value.moveTo(1.0f);
        value.advance(1.0f / 60.0f);
    }
    CHECK(value.value() == 1.0f,
          "a target restated every frame should still arrive, stuck at %f", value.value());

    /* And restating the target of an ARRIVED tween must not set it moving
     * again. */
    value.moveTo(1.0f);
    CHECK(!value.moving(), "restating a reached target should not restart the tween");
}

void tweenSnapsWhenTold()
{
    TweenFloat value(0.0f);
    value.withDuration(1.0f).withCurve(Ease::Linear).moveTo(50.0f);
    value.advance(0.2f);

    value.snapTo(10.0f);
    CHECK(value.value() == 10.0f && !value.moving(), "snapTo should jump and stop");

    /* A snap must also update the target, or the next advance would drag the
     * value back toward whatever it was heading for before. */
    value.advance(1.0f);
    CHECK(value.value() == 10.0f, "a snapped tween should stay put, drifted to %f",
          value.value());
}

}  // namespace

int main()
{
    everyCurveHitsItsEndpoints();
    inputIsClampedNotExtrapolated();
    ordinaryCurvesBehave();
    overshootingCurvesOvershoot();
    easedCurvesAreFasterOrSlowerWhereTheyClaim();
    timelineRunsAndStops();
    timelineDelaysBeforeItMoves();
    timelineRepeats();
    tweenArrivesExactly();
    tweenRetargetsWithoutJumping();
    tweenToleratesBeingToldItsTargetEveryFrame();
    tweenSnapsWhenTold();

    if (g_failures == 0) {
        std::printf("easing tests: all passed\n");
        return 0;
    }
    std::printf("easing tests: %d failure(s)\n", g_failures);
    return 1;
}
