/* Viewport.hpp — the camera and the rectangle it projects onto, and every
 * conversion between the two.
 *
 * SINGLE RESPONSIBILITY: turn screen positions into world positions and back.
 * Nothing here knows what is IN the world — that is a picker's job (see
 * game/picking/) — and nothing here draws.
 *
 * THIS IS THE LAYER UNREAL AND UNITY BOTH HAVE AND raylib DOES NOT.
 * `DeprojectScreenPositionToWorld`, `ScreenPointToRay`, `WorldToScreenPoint`,
 * `ScreenToWorldPoint`, `WorldToViewportPoint` — every project rebuilds some
 * subset of those against whatever the windowing library offers, gets the
 * behind-the-camera case wrong, and finds out the first time a unit walks past
 * the eye. raylib ships two of the conversions (`GetScreenToWorldRay`,
 * `GetWorldToScreen`) and neither answers the questions that actually come up:
 * where does the cursor meet the ground, how big is this thing in pixels, is
 * that point even on screen, and where do I put the arrow when it is not.
 *
 * ================= WHY A VIEWPORT AND NOT FREE FUNCTIONS ==================
 *
 * BECAUSE THE SCREEN IS NOT ALWAYS THE WINDOW. raylib's own helpers read
 * GetScreenWidth/GetScreenHeight out of globals, which is correct exactly once:
 * a single full-window camera. A render target, a picture-in-picture tactical
 * map, split-screen co-op, an editor viewport, a scope overlay — all of them
 * project through a rectangle that is not the window, and a helper that reads
 * the window silently returns a position offset by however far the viewport sits
 * from the corner. Off-by-a-viewport is a bug you chase in the wrong file.
 *
 * So the rectangle is a constructor argument. `ofWindow()` is the common case
 * and says so at the call site; anything else states its rectangle and gets
 * answers in the same device pixels the UI kit uses (see UiContext.hpp — there
 * is one coordinate space past that boundary and this is in it).
 *
 * The object is a value: a camera and a rectangle, built per call, free to
 * construct and safe to keep.
 *
 * ITS INTERFACE IS THE ENGINE'S TYPES, NOT raylib's — Vec2, Vec3 and cromwell's
 * own Ray. It takes a raylib Camera3D because that is a RESOURCE-shaped thing
 * the render passes hand around, and Camera::toRaylib() is where one comes
 * from; everything it hands BACK is a value type the headless half can use
 * without linking a window library. See math/RaylibInterop.hpp for the policy
 * and why the two kinds are treated differently.
 *
 * ================== THE ONE THING EVERYBODY GETS WRONG =====================
 *
 * PROJECTING A POINT BEHIND THE CAMERA PRODUCES A PLAUSIBLE ANSWER. The
 * perspective divide is by w, w goes negative behind the eye, and the result is
 * a screen position mirrored through the centre of the screen — on screen, in
 * range, and completely wrong. A nameplate for a soldier who walks past the
 * camera does not vanish; it slides to the opposite side of the screen and
 * tracks the soldier backwards.
 *
 * Which is why `project` returns a ScreenPoint with `inFront` on it rather than
 * a bare Vec2, and why the field is checked rather than documented. A caller
 * that ignores it has to ignore it deliberately.
 *
 * ============================ WHAT IS NOT HERE =============================
 *
 * NO PICKING. `rayThrough` hands back a ray; what it hits is the world's
 * business. The engine has no notion of a scene graph to trace against and is
 * not getting one — TilePicker, SurfacePicker and UnitPicker all trace the
 * game's own data, which is what makes what you click agree with what movement
 * and line of sight think is there.
 *
 * NO CULLING. `project` says whether one point lands on screen; deciding which
 * of ten thousand objects to draw is a different algorithm with a different
 * shape (bounds, hierarchy, coherence) and it does not belong behind a helper
 * that costs a matrix multiply per call.
 *
 * NO CACHING OF THE MATRICES. Rebuilt per call, because a Viewport is built per
 * call and the alternative is a stale-matrix bug in exchange for a few dozen
 * nanoseconds in cold code. If something ever projects thousands of points a
 * frame, hoist the Viewport out of the loop — it is a value, that is all it
 * takes — and measure before doing anything cleverer.
 */
#pragma once

#include "cromwell/math/Ray.hpp"
#include "cromwell/math/Vec2.hpp"
#include "cromwell/math/Vec3.hpp"

#include "raylib.h"

#include <optional>

namespace cromwell {

/* Where a world point landed on screen, and whether to believe it.
 *
 * ONE-SHOT DATA CARRIER (see the note in ui/core/UiColor.hpp): produced by one
 * call, read at the call site, dead within the frame. */
struct ScreenPoint {
    /* In front of the near plane. FALSE MEANS `position` IS MEANINGLESS — see
     * the header. Everything else in this struct is still valid. */
    bool inFront = false;

    /* In front AND inside the viewport rectangle. The test a HUD marker wants;
     * `inFront` alone is the test an offscreen ARROW wants, because an arrow
     * exists precisely for points that are in front and off the edge. */
    bool onScreen = false;

    /* Device pixels, in the viewport's own coordinates — already offset by the
     * viewport's origin, so it can be handed straight to a widget. */
    Vec2 position;

    /* Metres along the view axis. The quantity perspective actually divides by:
     * use it for size falloff, LOD and depth sorting. A point beside the camera
     * has a large `distance` and a near-zero `depth`, and it is `depth` that
     * decides how big it draws. */
    float depth = 0.0f;

    /* Straight-line metres from the eye. For range checks and fade cutoffs,
     * where "how far away is it" means what it sounds like. */
    float distance = 0.0f;
};

/* An offscreen indicator's placement: the arrow at the edge of the screen
 * pointing at something you cannot see.
 *
 * ONE-SHOT DATA CARRIER. */
struct EdgeMarker {
    /* False when the point is comfortably on screen and needs no marker. */
    bool offscreen = false;

    /* Device pixels, already inset by the margin, so a marker drawn centred
     * here is fully visible. */
    Vec2 position;

    /* Screen-space heading from the viewport centre toward the target, in the
     * y-down convention Vec2::fromAngle uses — feed it straight to a rotated
     * chevron and it points the right way. */
    float angleRadians = 0.0f;
};

class Viewport {
public:
    /* The whole window, which is the common case and the one raylib assumes.
     * REQUIRES A WINDOW — it reads raylib's screen size. */
    static Viewport ofWindow(const Camera3D& camera);

    /* A viewport at the origin, `sizePx` across. For a render target. */
    Viewport(const Camera3D& camera, Vec2 sizePx);

    /* A viewport somewhere other than the corner — split-screen, an inset map,
     * an editor pane. Screen positions in and out are in WINDOW coordinates;
     * the offset is applied here so callers never do it twice. */
    Viewport(const Camera3D& camera, Vec2 originPx, Vec2 sizePx);

    const Camera3D& camera() const { return camera_; }
    Vec2  origin() const { return origin_; }
    Vec2  size() const { return size_; }
    Vec2  centre() const { return { origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.5f }; }
    float aspect() const { return size_.y > 0.0f ? size_.x / size_.y : 1.0f; }
    bool  contains(Vec2 screenPx) const;

    /* ---- screen to world ------------------------------------------------ */

    /* The ray under a screen position — Unreal's DeprojectScreenPositionToWorld,
     * Unity's ScreenPointToRay. Hand it to a picker.
     *
     * Under an orthographic camera the origin slides across the near plane with
     * the cursor rather than sitting at the eye, because an ortho projection has
     * no convergence point. That difference is why this exists rather than a
     * hand-rolled `camera.position + direction`: the hand-rolled version is
     * correct until the day someone switches the camera to ortho for a tactical
     * view, and then every pick is wrong by half a screen. */
    Ray rayThrough(Vec2 screenPx) const;

    /* A point `metres` in front of the camera, under the cursor — Unity's
     * ScreenToWorldPoint with a z. `metres` is measured along the VIEW AXIS, not
     * along the ray, so a plane of these points is flat and parallel to the
     * screen rather than a sphere around the eye. That is what makes it usable
     * for placing a held item or a cursor plane at a fixed depth. */
    Vec3 pointAtDepth(Vec2 screenPx, float metres) const;

    /* Where the cursor's ray meets an arbitrary plane. Nothing when the ray is
     * parallel to it, or when the plane is behind the camera — a ray that would
     * only meet the plane going backwards has not met it. */
    std::optional<Vec3> pointOnPlane(Vec2 screenPx, Vec3 planePoint, Vec3 planeNormal) const;

    /* The horizontal plane at `height` — the RTS question, and the one asked
     * most. Where on the ground is the cursor: for a move order, a build
     * placement, an area-of-effect template, a drag-selection corner.
     *
     * NOT A SUBSTITUTE FOR A PICK against real geometry. This answers where the
     * cursor meets an infinite flat plane, which is exactly right for a cursor
     * that must land somewhere even when it is over the sky, and exactly wrong
     * for "which unit am I pointing at". */
    std::optional<Vec3> pointOnGround(Vec2 screenPx, float height = 0.0f) const;

    /* ---- world to screen ------------------------------------------------ */

    /* Unreal's ProjectWorldLocationToScreen, Unity's WorldToScreenPoint, with
     * the behind-the-camera case represented rather than mangled. See the
     * header, and read `inFront` before `position`. */
    ScreenPoint project(Vec3 worldPosition) const;

    /* ---- normalised (0..1) viewport space -------------------------------- */

    /* Unity's viewport space: (0,0) top-left, (1,1) bottom-right, in the y-down
     * convention the rest of this engine's screen space uses. For layout that
     * must hold at any resolution — a reticle a third of the way up, a safe-area
     * inset — where a pixel constant would be a different place on every
     * monitor. */
    Vec2 toNormalised(Vec2 screenPx) const;
    Vec2 fromNormalised(Vec2 normalised) const;

    /* ---- sizing and framing ---------------------------------------------- */

    /* How many device pixels one metre spans at `depth` metres along the view
     * axis. The conversion behind every "how big should this be" question:
     * sizing a selection ring, choosing an icon size, deciding a pick radius in
     * world units from a slop in pixels, picking an LOD.
     *
     * Constant under an orthographic camera, which is the entire point of one. */
    float pixelsPerMetreAt(float depth) const;

    /* The screen radius of a sphere of `metres` at `centre`, in device pixels.
     * Zero when the centre is behind the camera. Approximate at the edges of a
     * wide field of view — it scales the radius by the depth of the centre
     * rather than solving the true silhouette, which is the standard trade and
     * is invisible for anything smaller than the screen. */
    float projectedRadius(Vec3 centre, float metres) const;

    /* Where to put the arrow for something off screen. `marginPx` insets the
     * result from the viewport edge so the marker itself is not half cut off.
     *
     * Handles the behind-the-camera case, which is the reason this is a method
     * and not three lines at the call site: a point behind the eye projects to
     * the WRONG SIDE, so the direction is taken from the flipped position and
     * the marker points backwards over the correct shoulder. */
    EdgeMarker edgeMarker(Vec3 worldPosition, float marginPx = 0.0f) const;

    /* The same, for a screen position already in hand. Clamps into the inset
     * rectangle; a position already inside is returned unchanged. */
    Vec2 clampToEdge(Vec2 screenPx, float marginPx = 0.0f) const;

private:
    /* BY VALUE, and it used to be a pointer. Forty bytes is nothing next to the
     * lifetime hazard: cameras are now frequently built on demand — a
     * Camera::toRaylib() at the call site, a matchFraming conversion — and a
     * Viewport holding a pointer to one of those is holding a pointer to a
     * temporary that died at the end of the expression. The symptom is a
     * projection that is right until the compiler reuses the stack slot.
     *
     * Copying also makes a Viewport a plain value, which is what it reads like
     * at every call site anyway. */
    Camera3D camera_;
    Vec2 origin_;
    Vec2 size_;
};

}  // namespace cromwell
