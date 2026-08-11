/* PointerTests.cpp — headless verification of the pointer helpers.
 *
 * WHAT IS HERE AND WHY IT CAN BE. PointerFocus, HoverTracker and PointerDrag
 * are all pure state machines over booleans, floats and a screen position — no
 * device, no window, no camera. That is deliberate (see the note in
 * CMakeLists.txt beside them), and it is what lets the awkward cases below be
 * pinned down rather than eyeballed.
 *
 * AND THE AWKWARD CASES ARE THE WHOLE POINT. Nothing here asserts that hover
 * feels good; that is a thing you judge by moving a mouse. What it asserts is
 * the behaviour nobody can see happening and everybody assumes: that a one-frame
 * gap in the pick does not fire an exit, that a switch between two targets is
 * NOT delayed by the same grace that protects the gap, that a drag which
 * returns to its origin still commits as a drag, that a button released while
 * the game was not looking unsticks itself. Every one of those is a bug that
 * ships and gets misdiagnosed as something else.
 *
 * The projection half of this work — camera/Viewport — is not here, because it
 * projects through raylib's own matrices on purpose and cannot be built without
 * them. It is exercised by the game: the cursor picking the tile you point at
 * IS its test.
 */
#include "cromwell/input/HoverTracker.hpp"
#include "cromwell/input/PointerDrag.hpp"
#include "cromwell/input/PointerFocus.hpp"

#include <cstdio>
#include <optional>
#include <string_view>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

constexpr float kFrame = 1.0f / 60.0f;

/* ---- PointerFocus ---------------------------------------------------- */

void testFocusStartsWithTheWorldOwningEverything()
{
    PointerFocus focus;
    focus.resolve();

    CHECK(focus.worldTakesPointer(), "an unclaimed pointer belongs to the world");
    CHECK(focus.worldTakesKeys(), "an unclaimed keyboard belongs to the world");
    CHECK(focus.mouseClaimant() == nullptr, "no claimant when nothing claimed");
}

void testFocusPublishesOnTheFollowingFrame()
{
    PointerFocus focus;
    focus.resolve();
    focus.claimMouse("panel");

    /* THE ONE-FRAME LAG IS THE CONTRACT, not an accident — an immediate-mode
     * widget cannot claim before it has drawn. Claims made during a frame are
     * readable from the next one. */
    CHECK(focus.worldTakesPointer(), "a claim is not visible in the same set it was made in");

    focus.resolve();
    CHECK(focus.mouseOverUi(), "the claim publishes on the next frame");
    CHECK(!focus.worldTakesPointer(), "the world does not get a claimed pointer");
}

void testFocusReleasesWhenNobodyReclaims()
{
    PointerFocus focus;
    focus.resolve();
    focus.claimMouse("panel");
    focus.resolve();
    CHECK(focus.mouseOverUi(), "claimed");

    /* Nothing claims this frame, so the next one hands the pointer back. A
     * capture that had to be explicitly released is a capture that gets stuck. */
    focus.resolve();
    CHECK(focus.worldTakesPointer(), "an unrenewed claim lapses");
}

void testFocusKeepsMouseAndKeyboardSeparate()
{
    PointerFocus focus;
    focus.resolve();
    focus.claimKeyboard("console");
    focus.resolve();

    /* A focused text field must not also eat clicks on the world, and a hovered
     * button must not eat hotkeys. Collapsing these into one flag is the
     * standard mistake. */
    CHECK(focus.keyboardInUi(), "the keyboard is claimed");
    CHECK(focus.worldTakesPointer(), "claiming the keyboard does not claim the mouse");
}

void testFocusReportsTheLastClaimant()
{
    PointerFocus focus;
    focus.resolve();
    focus.claimMouse("dev panel");
    focus.claimMouse("hud");
    focus.resolve();

    /* Last wins, matching how the surfaces layer — later ones draw on top. */
    CHECK(std::string_view(focus.mouseClaimant()) == "hud", "the topmost claimant is named");
}

/* ---- HoverTracker ----------------------------------------------------- */

void testHoverEntersAndExits()
{
    HoverTracker<int> hover;

    HoverChange change = hover.update(std::optional<int>{ 7 }, kFrame);
    CHECK(change.entered && change.changed, "taking up a target enters");
    CHECK(!change.exited, "nothing was let go");
    CHECK(hover.target().value_or(-1) == 7, "the target is held");

    change = hover.update(std::optional<int>{ 7 }, kFrame);
    CHECK(!change.changed, "holding the same target is not an event");

    /* Long enough to outlast the default grace. */
    hover.update(std::nullopt, 1.0f);
    CHECK(!hover.has(), "the target is released once the grace elapses");
}

void testHoverSwallowsAOneFrameGap()
{
    HoverTracker<int> hover;
    hover.update(std::optional<int>{ 3 }, kFrame);

    /* THE BUG THIS PREVENTS: a ray grazing a seam misses for one frame, and the
     * highlight strobes at 60 Hz. */
    const HoverChange gap = hover.update(std::nullopt, kFrame);
    CHECK(!gap.exited, "a single missed frame does not exit");
    CHECK(hover.target().value_or(-1) == 3, "the target survives the gap");

    const HoverChange recovered = hover.update(std::optional<int>{ 3 }, kFrame);
    CHECK(!recovered.changed, "recovering the same target is not an enter");
}

void testHoverSwitchesImmediately()
{
    HoverTracker<int> hover(0.5f);  /* a long grace, to prove it does not apply */
    hover.update(std::optional<int>{ 1 }, kFrame);

    const HoverChange change = hover.update(std::optional<int>{ 2 }, kFrame);
    CHECK(change.entered && change.exited, "a direct switch is both halves at once");
    CHECK(hover.target().value_or(-1) == 2, "the new target takes over at once");
    CHECK(hover.previous().value_or(-1) == 1, "the outgoing target is readable");
}

void testHoverDwell()
{
    HoverTracker<int> hover;
    hover.update(std::optional<int>{ 9 }, kFrame);
    CHECK(!hover.dwelled(0.4f), "a fresh target has not dwelled");

    for (int frame = 0; frame < 30; ++frame) {
        hover.update(std::optional<int>{ 9 }, kFrame);
    }
    CHECK(hover.dwelled(0.4f), "half a second of holding satisfies a 0.4s gate");

    /* A gap the grace absorbs must not restart the tooltip timer — the target
     * was never really lost. */
    hover.update(std::nullopt, kFrame);
    CHECK(hover.dwelled(0.4f), "an absorbed gap does not reset the dwell");
}

void testHoverClearIsSilent()
{
    HoverTracker<int> hover;
    hover.update(std::optional<int>{ 4 }, kFrame);
    hover.clear();

    CHECK(!hover.has(), "clear drops the target");
    const HoverChange change = hover.update(std::nullopt, 1.0f);
    CHECK(!change.exited, "clear does not queue an exit for the next update");
}

/* ---- PointerDrag ------------------------------------------------------ */

void testClickWithinSlop()
{
    PointerDrag drag(6.0f);
    drag.update({ 100.0f, 100.0f }, true, true, false);

    /* Three pixels of hand tremor is a click, and on a trackpad it is the
     * normal case rather than the exception. */
    drag.update({ 102.0f, 101.0f }, false, true, false);
    const DragResult result = drag.update({ 103.0f, 101.0f }, false, false, true);

    CHECK(result.clicked, "a press and release inside the slop is a click");
    CHECK(!result.dragEnded, "and not a drag");
}

void testDragCrossesSlop()
{
    PointerDrag drag(6.0f);
    drag.update({ 100.0f, 100.0f }, true, true, false);

    const DragResult started = drag.update({ 140.0f, 100.0f }, false, true, false);
    CHECK(started.dragStarted && started.dragging, "crossing the slop starts a drag");

    const DragResult ended = drag.update({ 140.0f, 180.0f }, false, false, true);
    CHECK(ended.dragEnded, "release ends the drag");
    CHECK(!ended.clicked, "a drag is not also a click");
    CHECK(ended.dragging, "the box is still readable on the frame it commits");
}

void testDragLatches()
{
    PointerDrag drag(6.0f);
    drag.update({ 100.0f, 100.0f }, true, true, false);
    drag.update({ 200.0f, 100.0f }, false, true, false);

    /* BACK TO THE START IS STILL A DRAG. The player has watched a rectangle
     * stretch across the screen; committing a click instead would produce an
     * outcome they were never shown. */
    const DragResult result = drag.update({ 100.0f, 100.0f }, false, false, true);
    CHECK(result.dragEnded, "a drag that returns to its origin still commits as a drag");
    CHECK(!result.clicked, "and never as a click");
}

void testDragBoxIsNormalised()
{
    PointerDrag drag(6.0f);
    drag.update({ 200.0f, 200.0f }, true, true, false);
    drag.update({ 100.0f, 150.0f }, false, true, false);  /* up and to the left */

    CHECK(drag.boxMin().x == 100.0f && drag.boxMin().y == 150.0f, "min is the top-left");
    CHECK(drag.boxMax().x == 200.0f && drag.boxMax().y == 200.0f, "max is the bottom-right");
    CHECK(drag.boxContains({ 150.0f, 175.0f }), "a point inside is inside");
    CHECK(!drag.boxContains({ 150.0f, 100.0f }), "a point above is outside");
}

void testTravelIsMeasuredFromThePress()
{
    PointerDrag drag(20.0f);
    drag.update({ 0.0f, 0.0f }, true, true, false);

    /* Five pixels a frame never trips a per-frame delta test, and after five
     * frames the cursor is 25 px from where the button went down. Accumulating
     * from the press is what catches the slow drift. */
    for (int frame = 1; frame <= 5; ++frame) {
        drag.update({ static_cast<float>(frame) * 5.0f, 0.0f }, false, true, false);
    }
    CHECK(drag.dragging(), "a slow drift past the slop is still a drag");
}

void testDragUnsticksOnAMissedRelease()
{
    PointerDrag drag(6.0f);
    drag.update({ 100.0f, 100.0f }, true, true, false);
    drag.update({ 200.0f, 200.0f }, false, true, false);
    CHECK(drag.dragging(), "dragging");

    /* Alt-tab: the release happened somewhere the game never saw. The level
     * being false is the only evidence, and it has to be enough — otherwise a
     * marquee stays stretched across the screen until the next click. */
    drag.update({ 200.0f, 200.0f }, false, false, false);
    CHECK(!drag.active() && !drag.dragging(), "a button that is simply up ends the gesture");
}

void testCancelDoesNotCommit()
{
    PointerDrag drag(6.0f);
    drag.update({ 100.0f, 100.0f }, true, true, false);
    drag.cancel();

    const DragResult result = drag.update({ 101.0f, 100.0f }, false, false, true);
    CHECK(!result.clicked, "a cancelled gesture does not click on release");
    CHECK(!result.dragEnded, "nor commit a drag");
}

}  // namespace

int main()
{
    testFocusStartsWithTheWorldOwningEverything();
    testFocusPublishesOnTheFollowingFrame();
    testFocusReleasesWhenNobodyReclaims();
    testFocusKeepsMouseAndKeyboardSeparate();
    testFocusReportsTheLastClaimant();

    testHoverEntersAndExits();
    testHoverSwallowsAOneFrameGap();
    testHoverSwitchesImmediately();
    testHoverDwell();
    testHoverClearIsSilent();

    testClickWithinSlop();
    testDragCrossesSlop();
    testDragLatches();
    testDragBoxIsNormalised();
    testTravelIsMeasuredFromThePress();
    testDragUnsticksOnAMissedRelease();
    testCancelDoesNotCommit();

    if (g_failures == 0) {
        std::printf("pointer: all checks passed\n");
        return 0;
    }
    std::printf("pointer: %d check(s) failed\n", g_failures);
    return 1;
}
