/* DebugRenderer.hpp — the half of debug drawing that needs a GPU.
 *
 * SINGLE RESPONSIBILITY: turn DebugDraw's segment queue into lines on screen.
 * It decides nothing about what is drawn; that is settled by the time the
 * queue reaches it.
 *
 * WHY IT IS ITS OWN CLASS AND NOT A FUNCTION. Because it holds the one policy
 * question the queue cannot answer — whether debug geometry is visible at all —
 * and that is a per-session toggle a key has to reach. It is also where a
 * profiler zone belongs, and a zone wants somewhere to live.
 *
 * ======================== TWO PASSES, AND WHY ==============================
 *
 * A DEBUG LINE INSIDE A WALL IS THE COMMON CASE, not the exception: the reason
 * you are drawing a trace is usually that it went somewhere it should not have,
 * and the geometry it went into is exactly what hides it. So segments default
 * to x-ray (see DebugSegment::depthTested) and are drawn with the depth test
 * off, over the scene.
 *
 * The depth-tested ones still get their own pass, because occlusion is
 * sometimes the information — "did this ray actually clear the parapet" is
 * answered by whether the line disappears. Two passes rather than a sort,
 * because there are exactly two states and a comparator would be more code
 * than the second loop.
 *
 * ORDER: depth-tested first, then x-ray on top. An x-ray line that lost to a
 * depth-tested one would not be x-ray.
 *
 * ===================== WHY raylib's DrawLine3D IS FINE =====================
 *
 * IT LOOKS LIKE A DRAW CALL PER LINE AND IS NOT. rlgl accumulates vertices into
 * a batch and flushes when the batch fills or the primitive mode changes, so a
 * few thousand DrawLine3D calls in a row are a handful of draws. Building a
 * mesh here would be a second buffer to size, upload and keep in step, for a
 * system whose whole point is to be trivially available.
 *
 * If a debug frame ever gets heavy enough to matter, the fix is to draw less —
 * which is a correct thing to want anyway, because nobody can read ten thousand
 * lines.
 */
#pragma once

#include "cromwell/debug/DebugDraw.hpp"

namespace cromwell {

class DebugRenderer {
public:
    /* Draws the queue. MUST BE CALLED INSIDE BeginMode3D — it emits world-space
     * lines and has no camera of its own, deliberately: it is one pass among
     * the scene's others and taking a camera would invite it to disagree with
     * them.
     *
     * Does not consume the queue. Ageing is DebugDraw::advance's job, at the
     * top of the frame, so that a caller drawing the scene twice (a reflection
     * probe, a shadow pass) does not silently eat everything on the first
     * pass. */
    void draw(const DebugDraw& queue) const;

    /* Whether anything is drawn at all. Off hides every line without any caller
     * having to stop making them, which is the toggle that matters — debug
     * calls tend to live in the code for a while after they were useful. */
    bool visible() const { return visible_; }
    void setVisible(bool visible) { visible_ = visible; }
    void toggle() { visible_ = !visible_; }

private:
    bool visible_ = true;
};

}  // namespace cromwell
