/* Projection.hpp — building a camera, and choosing how it projects.
 *
 * SINGLE RESPONSIBILITY: assemble a Camera3D correctly for either projection,
 * and convert between "how much world do I want in frame" and the numbers
 * raylib actually wants.
 *
 * ================== THE TRAP THIS FILE EXISTS TO CLOSE =====================
 *
 * raylib's Camera3D HAS ONE FIELD CALLED fovy AND IT MEANS TWO DIFFERENT
 * THINGS.
 *
 *     CAMERA_PERSPECTIVE   fovy is a vertical ANGLE, in degrees.
 *     CAMERA_ORTHOGRAPHIC  fovy is the visible HEIGHT, in world units.
 *
 * Same name, same type, unrelated units, and nothing checks. Set fovy to 24
 * meaning "twenty-four metres of map" and leave the projection at its default
 * and you get a 24-degree perspective camera: a plausible, renderable,
 * completely wrong picture that looks like a zoomed-in view rather than like a
 * bug. Set fovy to 50 meaning degrees and switch to orthographic and you get
 * fifty metres of world, which on a 24-tile map is mostly empty space.
 *
 * So nothing here takes a bare `fovy`. Every function names what it wants —
 * `fovDegrees`, `worldHeight` — and fills the field to match the projection it
 * was told to build. That is the whole point of the file.
 *
 * ================== SWITCHING A CAMERA'S PROJECTION LATER ==================
 *
 * A camera cannot simply have its `projection` flipped, for the reason above:
 * the fovy that framed the scene under one is meaningless under the other. What
 * transfers is the FRAMING — how much world is in shot at the subject's
 * distance — and `matchFraming` below converts one to the other so a toggle
 * lands on the same view rather than on a random zoom level.
 *
 * The game's own camera (OrbitCamera) is perspective at 50 degrees, and this
 * header is what a projection toggle on it would be built from.
 *
 * HEADER-ONLY, and it names raylib, so it belongs to the engine's rendering
 * half — do not include it from anything in cromwell_base.
 */
#pragma once

#include "cromwell/math/RaylibInterop.hpp"
#include "cromwell/math/Vec3.hpp"

#include "raylib.h"

#include <algorithm>
#include <cmath>

namespace cromwell {

/* Named rather than raylib's int, so a caller cannot pass a fov where a
 * projection was wanted. */
enum class Projection { Perspective, Orthographic };

inline Projection projectionOf(const Camera3D& camera)
{
    return camera.projection == CAMERA_ORTHOGRAPHIC ? Projection::Orthographic
                                                    : Projection::Perspective;
}

/* How far a perspective camera has to be to fit `worldHeight` vertically in
 * frame. The bridge between the two projections' units, and what lets a
 * perspective camera be framed by saying how much world it should see rather
 * than by guessing a distance.
 *
 * VERTICAL, because fovy is. On a non-square target the horizontal extent is
 * this times the aspect ratio, so framing a wide map in a wide window needs the
 * height that follows from the width — divide by the aspect first. */
inline float framingDistance(float worldHeight, float fovDegrees)
{
    const float halfAngle = fovDegrees * 0.5f * DEG2RAD;
    const float tangent = std::tan(halfAngle);
    return tangent > 1e-6f ? (worldHeight * 0.5f) / tangent : worldHeight;
}

/* The inverse: how much world a perspective camera sees vertically at a given
 * distance. */
inline float framedHeight(float distance, float fovDegrees)
{
    return 2.0f * distance * std::tan(fovDegrees * 0.5f * DEG2RAD);
}

/* A camera looking from one point at another.
 *
 * `fovDegreesOrWorldHeight` is read according to `projection` — degrees for
 * perspective, world units for orthographic — which is exactly the ambiguity
 * the header describes, made explicit by the parameter's name. */
inline Camera3D makeCamera(Vec3 position, Vec3 target, Projection projection,
                           float fovDegreesOrWorldHeight, Vec3 up = Vec3::up())
{
    Camera3D camera{};
    camera.position = toRaylib(position);
    camera.target = toRaylib(target);
    camera.projection = projection == Projection::Orthographic ? CAMERA_ORTHOGRAPHIC
                                                               : CAMERA_PERSPECTIVE;
    camera.fovy = fovDegreesOrWorldHeight;

    /* A LOOK-AT WHOSE UP IS PARALLEL TO ITS VIEW DIRECTION IS DEGENERATE —
     * MatrixLookAt takes a cross product that comes out zero and every vertex
     * becomes NaN. It is not an exotic case: it is what "look straight down"
     * means, which is every top-down camera ever made. Caught here rather than
     * left to each caller, because the symptom is a black screen with nothing
     * in the log. */
    const Vec3 view = (target - position).normalised();
    Vec3 chosen = up.normalised();
    if (std::fabs(dot(view, chosen)) > 0.999f) {
        chosen = anyPerpendicular(view);
    }
    camera.up = toRaylib(chosen);
    return camera;
}

/* Straight down at `centre`, framing `worldSize` units.
 *
 * ORTHOGRAPHIC IS USUALLY RIGHT FOR A MAP: under perspective, two units the
 * same distance apart appear different distances apart depending where they are
 * on the map, and the picture stops being readable as a plan. Perspective is
 * available because a tilted "tactical overview" wants it, and because being
 * able to see the difference side by side is worth more than an opinion.
 *
 * `altitude` is only used under ORTHOGRAPHIC, where it changes nothing about
 * the framing — an ortho camera's height decides only what is in front of its
 * near plane — so it just has to clear the tallest thing on the map. Under
 * perspective the altitude IS the framing and is computed from the fov, which
 * is why it cannot be passed there.
 *
 * `mapUp` is the world direction that points up in the picture. It must be
 * horizontal, since the camera is looking down the world's up axis; -Z by
 * default, i.e. north up. */
inline Camera3D topDownCamera(Vec3 centre, float worldSize, Projection projection,
                              float fovDegrees = 50.0f, float altitude = 100.0f,
                              Vec3 mapUp = Vec3{ 0.0f, 0.0f, -1.0f })
{
    Vec3 up = mapUp;
    up.y = 0.0f;
    if (up.lengthSquared() < 1e-8f) up = Vec3{ 0.0f, 0.0f, -1.0f };

    const float size = worldSize > 1e-4f ? worldSize : 1.0f;

    if (projection == Projection::Orthographic) {
        return makeCamera(centre + Vec3{ 0.0f, std::max(altitude, 0.001f), 0.0f }, centre,
                          Projection::Orthographic, size, up);
    }

    /* Under perspective the height is not a free choice: it is whatever frames
     * the same amount of world. Deriving it is what makes the two projections
     * show the SAME view rather than two arbitrary ones. */
    return makeCamera(centre + Vec3{ 0.0f, framingDistance(size, fovDegrees), 0.0f }, centre,
                      Projection::Perspective, fovDegrees, up);
}

/* The same view, through the other projection.
 *
 * WHAT TRANSFERS IS THE FRAMING, not the fovy — see the header. Going to
 * orthographic, the visible height at the target's distance becomes the ortho
 * height; coming back, that height decides how far away the camera has to be.
 * The result is a toggle that lands on the picture you were already looking at
 * instead of on a random zoom.
 *
 * `fovDegrees` is the perspective side's field of view — the one the camera has
 * now if it is perspective, or the one it should get if it is not. */
inline Camera3D matchFraming(const Camera3D& camera, Projection projection,
                             float fovDegrees = 50.0f)
{
    const Vec3 position = fromRaylib(camera.position);
    const Vec3 target = fromRaylib(camera.target);
    const Vec3 up = fromRaylib(camera.up);

    if (projectionOf(camera) == projection) return camera;

    if (projection == Projection::Orthographic) {
        /* Perspective to ortho: how much world is in shot AT THE TARGET is what
         * the ortho camera should show. Measured at the target rather than at
         * some fixed distance, because the target is what the viewer is
         * actually looking at and is the only distance whose framing they would
         * notice changing. */
        const float distance = (target - position).length();
        return makeCamera(position, target, Projection::Orthographic,
                          framedHeight(distance, camera.fovy), up);
    }

    /* Ortho to perspective: the camera has to move to the distance that frames
     * the same height. Its direction is kept, so the view swings in depth
     * rather than sideways. */
    const Vec3 back = (position - target).normalised();
    const float distance = framingDistance(camera.fovy, fovDegrees);
    return makeCamera(target + back * distance, target, Projection::Perspective, fovDegrees,
                      up);
}

}  // namespace cromwell
