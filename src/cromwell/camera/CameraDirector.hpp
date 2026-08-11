/* CameraDirector.hpp — which camera the audience sees.
 *
 * SINGLE RESPONSIBILITY: hold the answer to "what is the screen showing right
 * now" — a default camera, an optional cut away from it, and nothing else.
 *
 * ====================== WHY THIS IS ITS OWN OBJECT =========================
 *
 * IT WAS BRIEFLY THE PLAYER CONTROLLER'S, AND THAT WAS WRONG. A controller
 * interprets input — it may ASK for a cut, the way it asks for a move order —
 * but what the screen shows is presentation state, not input state. Gameplay
 * can hold several cameras and cut between them (a kill-cam, a security feed,
 * a cinematic) without the player pressing anything, and a controller that
 * owned the view would make every one of those an input concern. So the
 * director owns the choice; the controller reads current() so its picking
 * rays agree with what the player is actually looking at; the renderer draws
 * whatever camera it is handed and never knows a switch happened.
 *
 * It is ENGINE rather than game because the question is genre-free: an RTS
 * cutting to a cinematic, an FPS's death camera and this game's plan view are
 * the same mechanism over different cameras, and there is no game noun in it.
 *
 * ========================== CUTS, NOT BLENDS (YET) =========================
 *
 * cutTo() is a hard cut: next frame is the other camera, the way Unreal's
 * SetViewTarget with no blend time behaves. A BLEND — easing position, slerping
 * orientation, matching framing across a projection change — is real machinery
 * with real traps (lens units are unrelated across projections; see
 * Projection.hpp), and it belongs HERE when something needs it: an interpolator
 * camera this class owns and reports from current() while the ease runs.
 * Deliberately unbuilt until then.
 *
 * ========================= BORROWED, NEVER OWNED ===========================
 *
 * The director holds pointers to cameras owned elsewhere — a pawn's rig, a
 * capture set's entry. A camera being destroyed must not be the one on screen:
 * cutBack() first, then remove it. The director cannot know who owns what it
 * is pointed at, and taking ownership here would make "show this camera" and
 * "keep this camera alive" the same call, which is how a view switch turns
 * into a lifetime bug.
 *
 * Header-only and GL-free: it is a choice between two pointers, and it should
 * cost what a choice costs.
 */
#pragma once

namespace cromwell {

class Camera;

class CameraDirector {
public:
    /* The camera the screen returns to — the player's own view. Set once at
     * startup, before the first frame asks current().
     *
     * CHAINS, like every configuration setter in this engine (see the API
     * style note in CLAUDE.md): a director is configured before use, and
     * `CameraDirector().withDefault(rig.camera())` reads as one expression.
     * The cut calls below return *this too — an action with no result of its
     * own may as well hand the object back. */
    CameraDirector& withDefault(Camera& camera) { default_ = &camera; return *this; }

    /* Show this camera instead. A hard cut, this frame. */
    CameraDirector& cutTo(Camera& camera) { target_ = &camera; return *this; }

    /* Back to the default. Safe to call when not cut away. */
    CameraDirector& cutBack() { target_ = nullptr; return *this; }

    bool isCutAway() const { return target_ != nullptr; }

    /* The camera the screen is showing. The default must have been set —
     * a director with no default has no answer to its only question. */
    Camera& current() const { return target_ != nullptr ? *target_ : *default_; }

private:
    Camera* default_ = nullptr;
    Camera* target_ = nullptr;
};

}  // namespace cromwell
