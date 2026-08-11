/* HoverTracker.hpp — what the pointer is over, as a state machine rather than
 * as a variable.
 *
 * SINGLE RESPONSIBILITY: take this frame's raw pick and turn it into enter,
 * exit and dwell — the events a caller actually reacts to.
 *
 * WHY THIS IS NOT JUST `if (hit != hovered_)`. That line is what every project
 * writes first, and it works until the pick is noisy — which it always is,
 * because a pick is a raycast against geometry and a raycast at a boundary
 * flickers. The cursor sits exactly on the seam between two tiles, on the
 * silhouette edge of a unit, on the gap between two floors, and the answer
 * alternates every frame. The highlight strobes, the tooltip opens and closes,
 * the path preview rebuilds sixty times a second, and the bug gets diagnosed as
 * a rendering problem because that is where it is visible.
 *
 * CLAUDE.md states the general rule under hot-loop discipline: when a choice is
 * re-made repeatedly, give the incumbent a small discount. This is that rule
 * applied to hover, in the only currency hover has — TIME. The current target
 * survives a short run of frames that pick nothing, so a one-frame miss at a
 * seam is absorbed instead of being shown.
 *
 * THE GRACE APPLIES TO LOSS, NOT TO CHANGE. Losing the target to nothing is
 * held for `graceSeconds`; moving to a DIFFERENT valid target switches
 * immediately. That asymmetry is the whole design. Delaying a switch would make
 * the cursor feel laggy, which is worse than the flicker it would fix; delaying
 * a loss is invisible, because the thing that was hovered stays hovered and the
 * pointer is still moving toward whatever comes next.
 *
 * DWELL IS THE OTHER HALF. A tooltip that opens the instant the cursor crosses
 * something opens constantly while the cursor is merely passing over things.
 * `heldSeconds` counts how long the current target has been held, and
 * `dwelled()` is the gate every delayed affordance wants — tooltips, inspect
 * panels, hover-to-expand.
 *
 * TEMPLATED ON THE HANDLE because a hover target is whatever the caller
 * identifies things by and the engine has no opinion: a cell index, a unit
 * pointer, an entity id, a widget id. It needs to be copyable and comparable
 * with ==, and nothing else.
 *
 * COLD CODE — one comparison and a float add per frame per tracker. Header-only
 * because it is a template; there is nothing to put in a .cpp.
 */
#pragma once

#include <optional>

namespace cromwell {

/* What changed this frame. ONE-SHOT DATA CARRIER (see ui/core/UiColor.hpp):
 * returned by value from update(), read at the call site, dead immediately. */
struct HoverChange {
    /* A target was taken up — either from nothing, or from a different one.
     * `entered` and `exited` are both true on a direct switch, which is what
     * lets a caller run its teardown and its setup from one result. */
    bool entered = false;

    /* A target was let go — to nothing, or to a different one. Fires only when
     * the grace period has actually elapsed, so it is safe to tear down state
     * on it. */
    bool exited = false;

    /* Either of the above. The condition most callers branch on. */
    bool changed = false;
};

template <typename T>
class HoverTracker {
public:
    HoverTracker() = default;

    /* Seconds a target is held after the pick goes empty. Zero disables the
     * hysteresis and gives the naive behaviour.
     *
     * The default is about two frames at 60 Hz, which is enough to swallow a
     * seam flicker and short enough that nobody perceives the highlight lagging
     * off the end of a sweep. Raise it for a noisier pick, not to paper over a
     * picker that is genuinely wrong. */
    explicit HoverTracker(float graceSeconds) : graceSeconds_(graceSeconds) {}

    void setGraceSeconds(float seconds) { graceSeconds_ = seconds; }
    float graceSeconds() const { return graceSeconds_; }

    /* Feed the frame's raw pick. `deltaSeconds` drives dwell and grace. */
    HoverChange update(const std::optional<T>& picked, float deltaSeconds)
    {
        HoverChange change;

        if (picked.has_value()) {
            graceRemaining_ = 0.0f;

            if (target_.has_value() && *target_ == *picked) {
                heldSeconds_ += deltaSeconds;
                return change;  /* the common frame: nothing happened */
            }

            /* A DIRECT SWITCH IS IMMEDIATE — see the header on why the grace is
             * asymmetric. Both flags are set so one branch can do both halves. */
            change.exited = target_.has_value();
            change.entered = true;
            change.changed = true;

            previous_ = target_;
            target_ = picked;
            heldSeconds_ = 0.0f;
            return change;
        }

        /* The pick found nothing. */
        if (!target_.has_value()) {
            return change;
        }

        /* Held over the gap. Dwell keeps counting: a target that survives its
         * grace was never really lost, so a tooltip already open should not
         * restart its timer because the ray grazed a seam. */
        graceRemaining_ += deltaSeconds;
        heldSeconds_ += deltaSeconds;
        if (graceRemaining_ < graceSeconds_) {
            return change;
        }

        change.exited = true;
        change.changed = true;
        previous_ = target_;
        target_.reset();
        heldSeconds_ = 0.0f;
        graceRemaining_ = 0.0f;
        return change;
    }

    /* The settled target — what the caller should treat as hovered. Not the raw
     * pick: during a grace period this still reports the held target, which is
     * the entire point. */
    const std::optional<T>& target() const { return target_; }
    bool has() const { return target_.has_value(); }

    /* What was hovered before, for a caller that needs to undo something on the
     * outgoing target during a direct switch. */
    const std::optional<T>& previous() const { return previous_; }

    /* How long the current target has been held. Zero when nothing is. */
    float heldSeconds() const { return target_.has_value() ? heldSeconds_ : 0.0f; }

    /* The tooltip gate: true once the current target has been held for at least
     * `seconds`. False when nothing is hovered. */
    bool dwelled(float seconds) const
    {
        return target_.has_value() && heldSeconds_ >= seconds;
    }

    /* Drop the target without reporting an exit. For the cases where hover
     * stops being meaningful rather than stops being true — a mode change, a
     * unit dying, the window losing focus. Callers that need the teardown
     * should run it themselves; a silent clear is what makes this different
     * from feeding an empty pick. */
    void clear()
    {
        previous_ = target_;
        target_.reset();
        heldSeconds_ = 0.0f;
        graceRemaining_ = 0.0f;
    }

private:
    std::optional<T> target_;
    std::optional<T> previous_;

    float heldSeconds_ = 0.0f;
    float graceRemaining_ = 0.0f;
    float graceSeconds_ = 0.033f;
};

}  // namespace cromwell
