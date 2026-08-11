/* PointerDrag.hpp — was that a click or a drag, and if a drag, over what
 * rectangle?
 *
 * SINGLE RESPONSIBILITY: watch one mouse button and separate the two gestures
 * that share it.
 *
 * THE PROBLEM IS THAT A CLICK IS A FAILED DRAG. Press and release are the same
 * two events either way; the only thing that distinguishes them is whether the
 * cursor moved in between, and by how much. Every project rediscovers this the
 * first time a player issues a move order to the wrong tile because their hand
 * shifted three pixels during the click — or the first time a selection box
 * appears and vanishes because the mouse twitched.
 *
 * SLOP IS NOT OPTIONAL AND IT IS NOT ZERO. A press-release with no movement at
 * all is rare on a real desk with a real hand, and it is rarer still on a
 * trackpad, where tapping moves the cursor by a pixel or two as a matter of
 * physics. So there is a threshold, and a gesture that never crosses it is a
 * click no matter how long it took. The default is a few device pixels, which
 * is roughly what desktop toolkits use.
 *
 * MEASURED FROM THE PRESS, NOT FRAME TO FRAME. Travel accumulates from where
 * the button went down, so a slow drift over twenty frames is still a drag; a
 * per-frame delta test would never trip on it and the gesture would commit as a
 * click at the far end of the screen.
 *
 * ONCE A DRAG, ALWAYS A DRAG. Crossing the threshold latches. Dragging out and
 * back to the press point releases as a drag over a degenerate box, not as a
 * click — because the player has seen a selection rectangle on screen and a
 * click would be a different outcome than the one they were shown. Feedback
 * that has already been given decides the gesture.
 *
 * THE BOX IS IN SCREEN SPACE, and stops there. Turning a rectangle of pixels
 * into a set of units is a world query and belongs with the world — project the
 * candidates and test them against the box, or unproject the corners into a
 * frustum, depending on how many there are. See camera/Viewport.hpp for both
 * directions.
 *
 * NO RAYLIB, so this is testable without a window. Feed it the button state
 * from wherever the caller gets input.
 *
 * COLD CODE — one distance test per frame.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"

namespace cromwell {

/* What the gesture did this frame. ONE-SHOT DATA CARRIER (see
 * ui/core/UiColor.hpp) — mirrors ui::InteractionResult deliberately, so the two
 * read the same way at a call site. */
struct DragResult {
    /* Pressed and released without crossing the slop. Fires on RELEASE, unlike
     * the widget kit's buttons which fire on press — and the difference is the
     * whole reason this class exists. A control knows on press that it was hit;
     * a world gesture cannot know until release whether it was a click at all.
     * See the note in ui/core/UiContext.hpp for the other side of that. */
    bool clicked = false;

    /* The frame the slop was crossed. The signal to start drawing a marquee. */
    bool dragStarted = false;

    /* True on every frame of a live drag, including the one it started. */
    bool dragging = false;

    /* The frame the button came up on a drag. The signal to run the query and
     * commit the selection — `box()` is still valid during this frame. */
    bool dragEnded = false;
};

class PointerDrag {
public:
    PointerDrag() = default;

    /* `slopPx` is in device pixels — the same space as everything else past the
     * UI boundary (see UiContext.hpp). Scale it by the display scale if the
     * gesture should feel the same on a high-DPI monitor; six device pixels on a
     * 200% display is three reference pixels, which is a tighter tolerance than
     * intended. */
    explicit PointerDrag(float slopPx) : slopPx_(slopPx) {}

    void setSlopPx(float slopPx) { slopPx_ = slopPx; }
    float slopPx() const { return slopPx_; }

    /* One frame of button state. `pressed` and `released` are edges, `down` is
     * the level — the same three the UI kit takes, so a caller feeds both from
     * one place.
     *
     * Taking all three rather than deriving the edges internally keeps this
     * agnostic about where input came from: a replay, a test, a network stream
     * and a mouse all present the same three booleans. */
    DragResult update(Vec2 position, bool pressed, bool down, bool released);

    /* ---- what the caller draws and queries -------------------------------- */

    bool active() const { return down_; }
    bool dragging() const { return dragging_; }

    Vec2 pressPosition() const { return pressPosition_; }
    Vec2 position() const { return position_; }

    /* Straight-line pixels from the press. What the slop is tested against, and
     * exposed because a caller may want to fade a marquee in over it. */
    float travelPx() const { return (position_ - pressPosition_).length(); }

    /* The marquee, normalised so min is always the top-left corner regardless of
     * which way the drag went. Degenerate (min == max) when not dragging.
     *
     * Corners rather than a UiRect on purpose: a rect type lives in the UI draw
     * list, and an input header that pulled the draw list in would drag the
     * whole widget kit behind every gesture. `UiRect::fromCorners` closes the
     * gap in one line at the one call site that draws it. */
    Vec2 boxMin() const;
    Vec2 boxMax() const;

    /* True when a screen position falls inside the marquee. The test a caller
     * runs per candidate after projecting it — see the header on why the query
     * itself is not here. */
    bool boxContains(Vec2 screenPx) const;

    /* Abandon the gesture without reporting a click or a drag end. For Escape,
     * for losing window focus, for a mode change mid-drag. A gesture that is
     * cancelled must not commit, and feeding it a fake release would. */
    void cancel();

private:
    float slopPx_ = 6.0f;

    Vec2 pressPosition_;
    Vec2 position_;

    bool down_ = false;
    bool dragging_ = false;
};

}  // namespace cromwell
