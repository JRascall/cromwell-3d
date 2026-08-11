/* Camera.hpp — a thing that looks at the world. ALL of them.
 *
 * SINGLE RESPONSIBILITY: say where a viewpoint is, how it projects, what it
 * draws, and — if it has one of its own — where its pixels land.
 *
 * ==================== ONE TYPE, AND WHY THAT MATTERS =======================
 *
 * THE PLAYER'S VIEW AND A SECURITY CAMERA ARE THE SAME THING. Both sit
 * somewhere, look somewhere, have a lens, and draw some subset of the world.
 * The only difference is where the result goes: one to the screen, one to a
 * texture. That is a property of the camera, not a different kind of object.
 *
 * This file exists because the first version of this system got that wrong. It
 * had a `Camera3D` for the player and a `SceneCapture` for everything else, so
 * a developer wanting a second viewpoint had to learn a second vocabulary,
 * discover that "layers" existed on one and not the other, and find out that
 * the thing called a camera could not be captured and the thing that captured
 * was not called a camera. Two names for one idea is a tax on everyone who
 * arrives later.
 *
 *     Camera main = Camera::perspective(60.0f);
 *     main.at({ 34, 24, -6 }).lookingAt({ 11, 1, 12 });
 *
 *     Camera cctv = Camera::perspective(70.0f);
 *     cctv.at({ 0, 10, 0 }).lookingAt({ 0, 0, 12 })
 *         .withLayers(worldOnly())          // a game-side preset — see ViewLayers.hpp
 *         .renderingToTexture(512, 512)
 *         .refreshingEvery(0.1f);
 *
 * Same type, same methods. The second one has an output and the first does not.
 * Wanting occlusion or decals on the second is a layer switch, not another
 * call — asking for either is what allocates the depth prepass they need. See
 * hasScreenSpaceEffects below.
 *
 * ========================= HOW A CAMERA IS DRAWN ===========================
 *
 * A camera does not know how to draw the world, and must not — what a scene
 * contains is the game's. There are two ways its pixels get made:
 *
 *   TO A TEXTURE. `capture(deltaSeconds, draw)` runs a scene pass into this
 *   camera's own target and resolves it. Needs `renderingToTexture` first.
 *   `texture()` and `drawTo()` read the result.
 *
 *   TO THE SCREEN. The frame's main pass renders it directly; the camera is
 *   just the viewpoint and the layer set. It needs no output at all.
 *
 * A CameraSet drives several of the first kind in one call, and handles the
 * staggering and naming that a collection needs — see camera/CameraSet.hpp.
 *
 * ========================== MOVE-ONLY, DELIBERATELY ========================
 *
 * A camera with a 512x512 target and its own depth prepass is several megabytes
 * of GPU memory. Copying one silently is never what anybody meant, so it is
 * move-only and the compiler says so.
 *
 * That costs single-expression chaining off a factory — `Camera c =
 * Camera::perspective(60).at(...)` will not compile, because `at` returns a
 * reference. Write the two lines; it reads better anyway, and the alternative
 * is rvalue-qualified overloads that nobody can maintain.
 *
 * ====================== THE fovy TRAP, CLOSED FOR GOOD =====================
 *
 * raylib's Camera3D has ONE field called fovy meaning a vertical ANGLE under
 * perspective and a visible HEIGHT in world units under orthographic — same
 * name, same type, unrelated units, nothing checks. This class does not have
 * that field. It has a projection chosen by a named constructor and a lens
 * value that goes with it, so the ambiguity cannot be expressed. `toRaylib()`
 * fills raylib's field correctly on the way out.
 */
#pragma once

#include "cromwell/camera/Projection.hpp"
#include "cromwell/gpu/target/CaptureSchedule.hpp"
#include "cromwell/gpu/target/HdrTarget.hpp"
#include "cromwell/gpu/target/ScenePassBuffers.hpp"
#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec2.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/overlay/ViewLayers.hpp"
#include "cromwell/post/ToneMapPass.hpp"

#include "raylib.h"

#include <functional>
#include <memory>

namespace cromwell {

class Viewport;

class Camera {
public:
    /* ---- making one ------------------------------------------------------
     *
     * NAMED, because a lens has to be chosen and the two take different units.
     * There is no default-constructed Camera for the same reason: a viewpoint
     * with no projection is not a viewpoint. */

    /* `fovDegrees` is the VERTICAL field of view. */
    static Camera perspective(float fovDegrees = 60.0f);

    /* `worldHeight` is how much world fits top to bottom, in metres. Parallel
     * lines stay parallel — what a map or a plan wants. */
    static Camera orthographic(float worldHeight);

    ~Camera();
    Camera(Camera&&) noexcept;
    Camera& operator=(Camera&&) noexcept;
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    /* An independent copy of the DESCRIPTION — position, lens, layers — with no
     * output and no schedule. For "another camera like this one", and for
     * handing a viewpoint somewhere that must not own the target. */
    Camera clone() const;

    /* ---- where it is ----------------------------------------------------- */

    Camera& at(Vec3 position);
    Camera& lookingAt(Vec3 target, Vec3 up = Vec3::up());

    /* Orientation rather than a look-at target, for a camera driven by a rig or
     * an animation. Forward is +Z, matching Quat's convention and lookRotation.
     * The look-at target is derived a metre ahead, so the two forms stay
     * interchangeable. */
    Camera& facing(Quat orientation);

    /* Straight down at `centre`, framing `worldSize` — the map rig, in one
     * call. Under perspective the altitude is DERIVED so both projections frame
     * the same span; under orthographic it just has to clear the tallest thing.
     * `mapUp` is the world direction that is up in the picture. */
    Camera& overlooking(Vec3 centre, float worldSize, float altitude = 100.0f,
                        Vec3 mapUp = Vec3{ 0.0f, 0.0f, -1.0f });

    Vec3 position() const { return position_; }
    Vec3 target() const { return target_; }
    Vec3 up() const { return up_; }

    Vec3 forward() const { return (target_ - position_).normalised(); }
    Quat orientation() const { return lookRotation(forward(), up_); }

    /* ---- how it projects -------------------------------------------------- */

    Projection projection() const { return projection_; }
    bool isOrthographic() const { return projection_ == Projection::Orthographic; }

    /* The lens, in whichever unit the projection uses — degrees or metres. Kept
     * as one accessor because that IS the quantity; what it means is answered
     * by `projection()`, which cannot be forgotten the way raylib's fovy can. */
    float lens() const { return lens_; }
    Camera& setLens(float degreesOrWorldHeight);

    /* Switches projection while keeping the SAME VIEW. What transfers is the
     * framing, not the lens value — the number that framed the scene under one
     * projection is meaningless under the other, so a naive flip lands on a
     * random zoom. See Projection.hpp. */
    Camera& switchTo(Projection projection, float fovDegrees = 60.0f);

    /* ---- what it draws ---------------------------------------------------- */

    /* WHAT IT DRAWS AND HOW — one struct, three groups, presets to start from.
     * See overlay/ViewLayers.hpp.
     *
     * THERE ARE NO PER-FEATURE CONVENIENCE METHODS HERE, deliberately. There
     * used to be three — withSsao, withShadows, withDecals — arbitrarily chosen
     * from a dozen switches, so a reader could not tell why those three had
     * shortcuts and the rest did not, and every new switch invited another.
     * `withLayers(ViewLayers::worldOnly())` then poking the one field that
     * differs is shorter to read, uniform across every switch, and has one
     * obvious place to look things up. */
    Camera& withLayers(const ViewLayers& layers);

    /* Mutable, so a single switch can be changed without restating the rest:
     *
     *     camera.layers().annotations.debug = true;
     *
     * Changing a screen-space feature through here allocates or frees this
     * camera's depth prepass, exactly as withLayers would — asking is what
     * makes it work. */
    ViewLayers& layers();
    const ViewLayers& layers() const { return layers_; }

    /* ---- rendering to its own texture ------------------------------------- */

    /* Gives this camera an offscreen target of its own. Without it the camera
     * is a viewpoint and nothing else, which is exactly what the main view
     * wants. REQUIRES A GL CONTEXT. */
    Camera& renderingToTexture(int width, int height);

    /* How often `capture()` actually draws. A camera with its own target is a
     * whole extra scene pass, so this is the knob that decides whether that
     * matters — five times a second is imperceptible on a map and costs a
     * twelfth of every frame. See CaptureSchedule. */
    Camera& refreshingEvery(float seconds);
    Camera& refreshingEveryFrame();
    Camera& refreshingOnDemand();

    /* This camera's exposure, for its own tone map — per camera because a dim
     * security feed and a sunlit main view are different photographs. Only
     * meaningful while `layers().features.toneMap` is on; the raw path
     * resolves radiance untouched. */
    Camera& withExposure(float exposure) { tonemap_.setExposure(exposure); return *this; }
    float   exposure() const { return tonemap_.exposure(); }

    bool rendersToTexture() const { return scene_ != nullptr && scene_->valid(); }

    /* Whether this camera has a depth prepass of its own, and therefore whether
     * its screen-space features can work.
     *
     * NOT SOMETHING YOU SET. It follows from the layers: ask for ambient
     * occlusion or decals on a camera that renders to a texture and the buffers
     * are allocated, drop both and they are freed. That is one concept instead
     * of two, and it removes the failure where a feature was switched on and
     * silently did nothing because a second call was forgotten.
     *
     * IT IS NOT FREE, and that is the honest cost of asking: a depth prepass is
     * a second geometry pass over the scene, plus three more targets at this
     * camera's resolution. A map does not need either feature and should not
     * ask; a security feed does. */
    bool hasScreenSpaceEffects() const { return buffers_ != nullptr && buffers_->valid(); }

    ScenePassBuffers*       buffers() { return buffers_.get(); }
    const ScenePassBuffers* buffers() const { return buffers_.get(); }

    CaptureSchedule&       schedule() { return schedule_; }
    const CaptureSchedule& schedule() const { return schedule_; }

    /* ---- taking the picture ----------------------------------------------- */

    /* WHICH PART OF THE PASS IS RUNNING, and it exists because of a hard raylib
     * constraint that is invisible until it bites.
     *
     * raylib's BeginTextureMode DOES NOT NEST. EndTextureMode binds framebuffer
     * ZERO — not whatever was bound before it — so a pass that opens its own
     * render target from inside another one does not come back: everything
     * drawn after it goes to the backbuffer, silently. A capture whose depth
     * prepass ran inside its colour target therefore produced a black texture
     * and a frame of geometry painted over the main view for one frame.
     *
     * So the kinds of pass are separated in time rather than nested:
     *
     *   OFFSCREEN  everything with a target of its own — the depth prepass, the
     *              decal DBuffer, the occlusion blur. Runs with NOTHING bound,
     *              so each is free to open and close its own.
     *   MAIN       the lit scene, in linear radiance. Runs INSIDE this camera's
     *              colour target, and must not open another one.
     *   DISPLAY    over the FINISHED picture — inside the resolved target,
     *              after the tone map, in display colour at output resolution.
     *              The slot for anything a game draws over its main view after
     *              ITS tone map (this game's movement ribbons), so a capture
     *              can carry the same passes the screen does. Must not open a
     *              render target either.
     *
     * The callback is invoked once per phase and branches on it. A camera with
     * nothing to do in a phase simply does nothing in it. */
    enum class ScenePhase {
        Offscreen,
        Main,
        Display,
    };

    /* Draws the world for this camera into a target of the given pixel size.
     * In the Main phase the callback opens its own BeginMode3D, exactly as a
     * real scene pass does — a sky or any pre-3D work has to happen inside the
     * target and before the mode starts, and a wrapper that opened it would
     * make that unsayable.
     *
     * THE CAMERA ARRIVES MUTABLE, because a scene pass legitimately writes into
     * it: the depth prepass renders into its buffers(), the occlusion pass sets
     * its enable state. This is also CameraSet's callback type, unchanged — one
     * signature, so a draw function written against either works with both. */
    using DrawScene =
        std::function<void(Camera& camera, ScenePhase phase, float width, float height)>;

    /* Renders if the schedule says so; returns whether it did. Call once a
     * frame. `zoneName` names the profiler row — distinct per camera, or a
     * profile cannot say which one cost the frame. */
    bool capture(float deltaSeconds, const DrawScene& draw,
                 const char* zoneName = "camera");

    /* Renders NOW, whatever the schedule — a single frame on demand. For a
     * photograph: a saved-game thumbnail, a loading backdrop, a shader that
     * needs the world from somewhere else exactly once. */
    bool captureNow(const DrawScene& draw, const char* zoneName = "camera");

    /* ---- reading the result ------------------------------------------------ */

    /* The resolved texture. Y-FLIPPED, as every render texture is — prefer
     * drawTo, which handles it. Blank when this camera has no output. */
    Texture2D texture() const;
    int textureWidth() const;
    int textureHeight() const;

    /* Draws it right way up into a screen rectangle. Inside BeginDrawing,
     * outside any 3D mode. */
    void drawTo(Rectangle destination, Color tint = WHITE) const;

    /* The picture as CPU pixels, right way up — for a saved-game thumbnail, a
     * screenshot, anything leaving the GPU. The caller owns the Image and
     * frees it with UnloadImage; zero-sized when this camera has no output.
     *
     * A READBACK STALLS THE PIPELINE — the GPU must finish the frame before
     * the copy can start — so this is for a photograph taken on an event, not
     * anything called per frame. Pair it with captureNow(): render once, read
     * once. */
    Image snapshot() const;

    /* ---- reading the world through it -------------------------------------- */

    /* The projection helper for this camera as drawn at a screen rectangle.
     * Feed it a cursor position and it gives back a world ray — which is what
     * makes a camera's picture CLICKABLE wherever it is shown, a minimap
     * included. The rectangle must have the picture's aspect ratio. */
    Viewport viewportAt(Vec2 originPx, Vec2 sizePx) const;

    /* The raylib camera the render passes take. THE ONE BOUNDARY: everything
     * above is cromwell's vocabulary, and this is where it becomes raylib's,
     * the same arrangement Vec3 has with Vector3. */
    Camera3D toRaylib() const;

private:
    Camera(Projection projection, float lens);

    Vec3 position_{ 0.0f, 0.0f, 0.0f };
    Vec3 target_{ 0.0f, 0.0f, 1.0f };
    Vec3 up_ = Vec3::up();

    Projection projection_ = Projection::Perspective;
    float      lens_ = 60.0f;

    ViewLayers layers_{};

    /* Absent for a camera that renders to the screen, which is most of them. */
    std::unique_ptr<HdrTarget>        scene_;
    std::unique_ptr<ScenePassBuffers> buffers_;
    RenderTexture2D                   resolved_ = { 0 };
    ToneMapPass                       tonemap_;
    CaptureSchedule                   schedule_ = CaptureSchedule::everyFrame();

    /* Allocates or frees this camera's depth prepass to match its layers. Called
     * wherever either side can change — see the note on layers(). */
    void syncScreenSpaceBuffers();

    void destroyOutput();
};

}  // namespace cromwell
