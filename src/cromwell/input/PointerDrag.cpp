#include "cromwell/input/PointerDrag.hpp"

#include <algorithm>

namespace cromwell {

DragResult PointerDrag::update(Vec2 position, bool pressed, bool down, bool released)
{
    DragResult result;
    position_ = position;

    if (pressed) {
        /* The press point is latched from THIS event's position rather than
         * from the previous frame's cursor. On a fast flick the two differ by
         * more than the slop, and a gesture would start already dragging from
         * where the mouse used to be. */
        pressPosition_ = position;
        down_ = true;
        dragging_ = false;
    }

    /* `down` is trusted over the internal flag rather than merged with it: a
     * press that arrived while the game was not looking — alt-tabbed, a modal
     * open, the pointer captured by a UI surface — leaves a button that is up
     * with a gesture that thinks it is down. Taking the level as the truth
     * unsticks it on the next frame instead of leaving a marquee stretched
     * across the screen until the player clicks again. */
    if (!down && !released) {
        down_ = false;
        dragging_ = false;
        return result;
    }

    if (down_ && !dragging_ && travelPx() > slopPx_) {
        /* LATCHED — see the header. Once the player has been shown a marquee,
         * the gesture is a drag whatever the cursor does afterwards. */
        dragging_ = true;
        result.dragStarted = true;
    }

    result.dragging = down_ && dragging_;

    if (released && down_) {
        if (dragging_) {
            result.dragEnded = true;
            /* Still true on the frame it ends, so the caller can draw the final
             * rectangle and run the query from the same branch. */
            result.dragging = true;
        } else {
            result.clicked = true;
        }
        down_ = false;
        dragging_ = false;
    }

    return result;
}

Vec2 PointerDrag::boxMin() const
{
    if (!dragging_) {
        return position_;
    }
    return { std::min(pressPosition_.x, position_.x), std::min(pressPosition_.y, position_.y) };
}

Vec2 PointerDrag::boxMax() const
{
    if (!dragging_) {
        return position_;
    }
    return { std::max(pressPosition_.x, position_.x), std::max(pressPosition_.y, position_.y) };
}

bool PointerDrag::boxContains(Vec2 screenPx) const
{
    const Vec2 low = boxMin();
    const Vec2 high = boxMax();
    return screenPx.x >= low.x && screenPx.x <= high.x
        && screenPx.y >= low.y && screenPx.y <= high.y;
}

void PointerDrag::cancel()
{
    down_ = false;
    dragging_ = false;
}

}  // namespace cromwell
