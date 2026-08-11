#include "cromwell/camera/Camera.hpp"

#include "cromwell/camera/Viewport.hpp"
#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"
#include "cromwell/math/RaylibInterop.hpp"

#include <algorithm>

namespace cromwell {

Camera::Camera(Projection projection, float lens)
    : projection_(projection), lens_(lens) {}

Camera Camera::perspective(float fovDegrees)
{
    return Camera{ Projection::Perspective, std::max(fovDegrees, 1.0f) };
}

Camera Camera::orthographic(float worldHeight)
{
    return Camera{ Projection::Orthographic, std::max(worldHeight, 1.0e-3f) };
}

Camera::~Camera() { destroyOutput(); }

Camera::Camera(Camera&& other) noexcept
    : position_(other.position_), target_(other.target_), up_(other.up_),
      projection_(other.projection_), lens_(other.lens_), layers_(other.layers_),
      scene_(std::move(other.scene_)), buffers_(std::move(other.buffers_)),
      resolved_(other.resolved_), schedule_(other.schedule_)
{
    /* The moved-from camera must not free the target we just took. Its own
     * destructor still runs.
     *
     * tonemap_ DOES NOT TRAVEL — ToneMapPass owns a shader and is not movable —
     * so this camera starts with an unloaded one and captureNow() reloads it at
     * first use. The moved-from camera keeps its shader and frees it normally. */
    other.resolved_ = RenderTexture2D{ 0 };
}

Camera& Camera::operator=(Camera&& other) noexcept
{
    if (this == &other) return *this;

    destroyOutput();

    position_ = other.position_;
    target_ = other.target_;
    up_ = other.up_;
    projection_ = other.projection_;
    lens_ = other.lens_;
    layers_ = other.layers_;
    scene_ = std::move(other.scene_);
    buffers_ = std::move(other.buffers_);
    resolved_ = other.resolved_;
    schedule_ = other.schedule_;

    other.resolved_ = RenderTexture2D{ 0 };
    return *this;
}

Camera Camera::clone() const
{
    /* THE DESCRIPTION ONLY. A clone that shared or duplicated the target would
     * be either a double free or several megabytes nobody asked for; "another
     * camera like this one" means the viewpoint and the layers. */
    Camera copy{ projection_, lens_ };
    copy.position_ = position_;
    copy.target_ = target_;
    copy.up_ = up_;
    copy.layers_ = layers_;
    return copy;
}

/* ---- placement --------------------------------------------------------- */

Camera& Camera::at(Vec3 position)
{
    /* The look direction is kept, not the target: `at` moves the camera, and a
     * camera that swung round to keep staring at a fixed point when it was
     * merely repositioned would be a dolly nobody asked for. */
    const Vec3 offset = target_ - position_;
    position_ = position;
    target_ = position + offset;
    return *this;
}

Camera& Camera::lookingAt(Vec3 target, Vec3 up)
{
    target_ = target;

    /* A LOOK-AT WHOSE UP IS PARALLEL TO ITS VIEW IS DEGENERATE — MatrixLookAt
     * crosses two parallel vectors and every pixel comes out NaN. Straight down
     * is not exotic; it is what every map camera does. Fixed here rather than
     * left to each caller, because the symptom is a black screen with nothing
     * in the log. */
    const Vec3 view = (target_ - position_).normalised();
    Vec3 chosen = up.normalised();
    if (view.lengthSquared() > 0.5f && std::abs(dot(view, chosen)) > 0.999f) {
        chosen = anyPerpendicular(view);
    }
    up_ = chosen;
    return *this;
}

Camera& Camera::facing(Quat orientation)
{
    /* A metre ahead, so `target()` stays meaningful and the two ways of aiming
     * a camera remain interchangeable. Distance to the target does not affect a
     * view matrix. */
    return lookingAt(position_ + forwardOf(orientation), upOf(orientation));
}

Camera& Camera::overlooking(Vec3 centre, float worldSize, float altitude, Vec3 mapUp)
{
    const Camera3D rig = topDownCamera(centre, worldSize, projection_,
                                       projection_ == Projection::Perspective ? lens_ : 60.0f,
                                       altitude, mapUp);
    position_ = fromRaylib(rig.position);
    target_ = fromRaylib(rig.target);
    up_ = fromRaylib(rig.up);

    /* Under orthographic the framing IS the lens, so overlooking sets it. Under
     * perspective the framing is the altitude, which topDownCamera derived, and
     * the lens is left alone. */
    if (projection_ == Projection::Orthographic) lens_ = std::max(worldSize, 1.0e-3f);
    return *this;
}

/* ---- lens -------------------------------------------------------------- */

Camera& Camera::setLens(float degreesOrWorldHeight)
{
    lens_ = std::max(degreesOrWorldHeight, 1.0e-3f);
    return *this;
}

Camera& Camera::switchTo(Projection projection, float fovDegrees)
{
    if (projection == projection_) return *this;

    const Camera3D converted = matchFraming(toRaylib(), projection, fovDegrees);
    position_ = fromRaylib(converted.position);
    target_ = fromRaylib(converted.target);
    up_ = fromRaylib(converted.up);
    projection_ = projection;
    lens_ = converted.fovy;
    return *this;
}

/* ---- content ----------------------------------------------------------- */

Camera& Camera::withLayers(const ViewLayers& layers)
{
    layers_ = layers;
    syncScreenSpaceBuffers();
    return *this;
}

ViewLayers& Camera::layers()
{
    /* A CALLER HOLDING THIS REFERENCE CAN CHANGE A SCREEN-SPACE FEATURE, and the
     * buffers have to follow or the flag would be set and do nothing — the exact
     * failure that removing the separate `withScreenSpaceEffects` switch was
     * meant to end.
     *
     * The sync that matters happens in captureNow(), which every capture passes
     * through AFTER any writes have landed. Syncing here as well costs nothing
     * — allocation is idempotent — and keeps a caller that reads buffers()
     * straight after a withLayers-style edit seeing the settled state. */
    syncScreenSpaceBuffers();
    return layers_;
}

/* ---- output ------------------------------------------------------------ */

void Camera::destroyOutput()
{
    if (resolved_.id != 0) {
        UnloadRenderTexture(resolved_);
        resolved_ = RenderTexture2D{ 0 };
    }
    scene_.reset();
    buffers_.reset();
}

Camera& Camera::renderingToTexture(int width, int height)
{
    const int outWidth = std::max(width, 1);
    const int outHeight = std::max(height, 1);

    if (rendersToTexture() && resolved_.texture.width == outWidth
        && resolved_.texture.height == outHeight) {
        return *this;  /* already the right size — safe to call every frame */
    }

    destroyOutput();

    /* Supersampled behind the resolve, same factor as the main scene and for a
     * stronger reason: a camera's output is usually SMALL, and a small target is
     * exactly where hard geometry edges alias worst. */
    scene_ = std::make_unique<HdrTarget>();
    if (!scene_->create(outWidth * ToneMapPass::kSupersampleFactor,
                        outHeight * ToneMapPass::kSupersampleFactor, /*withDepth=*/true)) {
        LOGGER->warn("camera: could not create a {}x{} HDR target", outWidth, outHeight);
        destroyOutput();
        return *this;
    }

    resolved_ = LoadRenderTexture(outWidth, outHeight);
    if (resolved_.id == 0) {
        LOGGER->warn("camera: could not create a {}x{} resolve target", outWidth, outHeight);
        destroyOutput();
        return *this;
    }
    SetTextureFilter(resolved_.texture, TEXTURE_FILTER_BILINEAR);

    if (!tonemap_.valid()) tonemap_.load();

    /* The buffers follow the layers, so a resize re-derives them rather than
     * losing them — a window drag must not silently turn a security feed flat. */
    syncScreenSpaceBuffers();

    /* The new target holds nothing, so whatever the schedule thought it had
     * captured is gone. */
    schedule_.invalidate();
    return *this;
}

Camera& Camera::refreshingEvery(float seconds)
{
    schedule_ = CaptureSchedule::interval(seconds, schedule_.phase());
    return *this;
}

Camera& Camera::refreshingEveryFrame()
{
    schedule_ = CaptureSchedule::everyFrame();
    return *this;
}

Camera& Camera::refreshingOnDemand()
{
    schedule_ = CaptureSchedule::onDemand();
    return *this;
}

void Camera::syncScreenSpaceBuffers()
{
    /* DERIVED FROM THE LAYERS, never set directly. Asking for occlusion or
     * decals is what allocates the prepass they are reconstructed from; dropping
     * both frees it. One concept instead of two, and no way to switch a feature
     * on and silently not get it.
     *
     * Only for a camera with its own target: one rendering to the screen uses
     * the frame's buffers, which are already the right size and viewpoint. */
    const bool wanted = layers_.needsDepthPrepass() && rendersToTexture();

    if (!wanted) {
        buffers_.reset();
        return;
    }

    if (buffers_ == nullptr) buffers_ = std::make_unique<ScenePassBuffers>();

    /* Sized to the HDR buffer, NOT the resolve. The prepass has to agree
     * pixel-for-pixel with the pass that samples it — occlusion is looked up at
     * the fragment's own coordinates, so a half-scale prepass smears it. */
    buffers_->resize(static_cast<int>(scene_->width()), static_cast<int>(scene_->height()));
}

/* ---- capture ----------------------------------------------------------- */

bool Camera::capture(float deltaSeconds, const DrawScene& draw, const char* zoneName)
{
    if (!rendersToTexture() || !draw) return false;
    if (!schedule_.tick(deltaSeconds)) return false;
    return captureNow(draw, zoneName);
}

bool Camera::captureNow(const DrawScene& draw, const char* zoneName)
{
    if (!rendersToTexture() || !draw) return false;

    /* THE BUFFERS FOLLOW THE LAYERS, SETTLED HERE, before anything draws. A
     * caller that wrote a screen-space switch through layers() has changed what
     * this capture needs, and this is the one place every capture passes
     * through — so syncing here is what makes "asking is what makes it work"
     * true rather than merely documented. Idempotent, so the common case costs
     * two boolean tests. */
    syncScreenSpaceBuffers();

    /* LOADED AT USE, NOT ONLY AT SETUP. The tonemap cannot travel through a
     * move — ToneMapPass owns a shader and is deliberately not movable — so a
     * camera that was moved after renderingToTexture() arrives here with a
     * target and no tonemap. Reloading lazily closes that hole; the alternative
     * was a capture that silently resolved to nothing. */
    if (!tonemap_.valid()) tonemap_.load();

    /* Named by the caller on BOTH timelines: several cameras in a frame is the
     * expected case, and rows that all say "camera" cannot tell you which of
     * them was expensive. */
    const char* const name = zoneName != nullptr ? zoneName : "camera";
    CW_PROFILE_ZONE_N(name);
    CW_GPU_ZONE(name);

    /* THE OFFSCREEN PASSES FIRST, WITH NOTHING BOUND. Each of them opens a
     * render target of its own, and raylib's texture mode does not nest — see
     * ScenePhase. Running them here rather than inside the colour target below
     * is the whole reason that enum exists. */
    draw(*this, ScenePhase::Offscreen, scene_->width(), scene_->height());

    {
        HdrTarget::Scope scope(*scene_);

        /* BLANK, not a background colour: a camera's picture is frequently
         * composited over a HUD panel, and a transparent surround is the only
         * version that works both there and over an opaque frame. */
        ClearBackground(BLANK);

        /* No BeginMode3D here — the callback opens its own. See the header. */
        draw(*this, ScenePhase::Main, scene_->width(), scene_->height());
    }

    BeginTextureMode(resolved_);
    ClearBackground(BLANK);

    /* ONE RESOLVE, ALWAYS. The tone map is a feature switch, not a branch —
     * the pass absorbs it and blits raw when the curve is off, so this call
     * site cannot drift out of step with a second path. See ToneMapPass. */
    tonemap_.draw(*scene_, static_cast<float>(resolved_.texture.width),
                  static_cast<float>(resolved_.texture.height), layers_.features.toneMap);

    /* THE DISPLAY PHASE, still inside the resolved target: whatever the game
     * draws over its finished frame gets the same slot here, at the OUTPUT
     * resolution rather than the supersampled one — these passes are drawn in
     * display colour and must not be tone-mapped like radiance. */
    draw(*this, ScenePhase::Display, static_cast<float>(resolved_.texture.width),
         static_cast<float>(resolved_.texture.height));

    EndTextureMode();
    return true;
}

/* ---- result ------------------------------------------------------------ */

Texture2D Camera::texture() const { return resolved_.texture; }
int Camera::textureWidth() const { return resolved_.texture.width; }
int Camera::textureHeight() const { return resolved_.texture.height; }

void Camera::drawTo(Rectangle destination, Color tint) const
{
    if (!rendersToTexture()) return;

    /* THE NEGATIVE HEIGHT IS THE FLIP. A render texture's rows are stored
     * bottom-up, so a straight blit shows the world mirrored — obvious with a
     * sky and invisible on a symmetrical map until somebody walks north and the
     * marker goes south. */
    const Rectangle source{ 0.0f, 0.0f, static_cast<float>(resolved_.texture.width),
                            -static_cast<float>(resolved_.texture.height) };
    DrawTexturePro(resolved_.texture, source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
}

Image Camera::snapshot() const
{
    if (!rendersToTexture()) return Image{};

    Image image = LoadImageFromTexture(resolved_.texture);

    /* The same flip drawTo does, for the same reason: a render texture's rows
     * are stored bottom-up, and a thumbnail saved without this is upside down
     * in every file browser. */
    ImageFlipVertical(&image);
    return image;
}

Viewport Camera::viewportAt(Vec2 originPx, Vec2 sizePx) const
{
    return Viewport{ toRaylib(), originPx, sizePx };
}

Camera3D Camera::toRaylib() const
{
    return makeCamera(position_, target_, projection_, lens_, up_);
}

}  // namespace cromwell
