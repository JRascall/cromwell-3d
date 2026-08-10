#include "render/model/ModelPreview.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"
#include "render/lighting/PbrShader.hpp"
#include "render/material/MaterialLibrary.hpp"
#include "render/model/ModelAsset.hpp"

#include <cmath>

namespace xcom {
namespace {

/* FIELD BY FIELD, NOT memcmp. Both settings structs are plain data, but
 * Lighting carries a bool among its floats and therefore padding, and
 * comparing padding bytes is how a preview ends up redrawing every frame for
 * reasons nobody can reproduce. Cheap enough written out. */
bool equal(Vector3 a, Vector3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool equal(const SunLight::Tuning& a, const SunLight::Tuning& b)
{
    return a.peak == b.peak && equal(a.tint, b.tint)
        && a.ambient == b.ambient && equal(a.skyTint, b.skyTint)
        && a.angularRadius == b.angularRadius;
}

bool equal(const ModelPreview::Framing& a, const ModelPreview::Framing& b)
{
    return a.yawDegrees == b.yawDegrees && a.pitchDegrees == b.pitchDegrees
        && a.fovDegrees == b.fovDegrees && a.zoom == b.zoom
        && a.padding == b.padding && equal(a.focusOffset, b.focusOffset);
}

bool equal(const ModelPreview::Lighting& a, const ModelPreview::Lighting& b)
{
    return a.keyAzimuthDegrees == b.keyAzimuthDegrees
        && a.keyElevationDegrees == b.keyElevationDegrees
        && a.followsCamera == b.followsCamera
        && equal(a.tuning, b.tuning) && a.exposure == b.exposure;
}

bool equal(const BoundingBox& a, const BoundingBox& b)
{
    return equal(a.min, b.min) && equal(a.max, b.max);
}

/* Short of the poles. At exactly 90 the look-at up vector is parallel to the
 * view direction and the view matrix degenerates; a couple of degrees of
 * margin also stops the image spinning on its own axis as it approaches. */
constexpr float kMaxPitchDegrees = 88.0f;

/* Wide enough for a scope reticle, tight enough that a dropship cannot be
 * pushed so far away it becomes three pixels. */
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 8.0f;

}  // namespace

ModelPreview::~ModelPreview()
{
    destroy();
}

bool ModelPreview::createTargets(int width, int height)
{
    if (width <= 0 || height <= 0) return false;

    if (!hdr_.create(width * kSupersampleFactor, height * kSupersampleFactor,
                     /*withDepth=*/true)) {
        TraceLog(LOG_WARNING, "PREVIEW: no float target - previews disabled");
        return false;
    }

    resolved_ = LoadRenderTexture(width, height);
    if (resolved_.id == 0) {
        hdr_.destroy();
        return false;
    }

    /* BILINEAR AND CLAMPED. The panel this lands in is rarely the exact size
     * it was rendered at, and point sampling a scaled preview throws away the
     * supersampling that was the expensive part. Clamp because the alpha at
     * the border is zero and a wrapped tap would fetch the opposite edge of
     * the model into the fringe. */
    SetTextureFilter(resolved_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(resolved_.texture, TEXTURE_WRAP_CLAMP);

    dirty_ = true;
    return true;
}

bool ModelPreview::create(int width, int height)
{
    destroy();

    if (!createTargets(width, height)) return false;

    resolve_ = ShaderLibrary::load(nullptr, "model_preview.fs.glsl");
    if (resolve_.id == 0) {
        destroy();
        return false;
    }
    locExposure_ = GetShaderLocation(resolve_, "uExposure");

    return true;
}

void ModelPreview::destroy()
{
    if (resolve_.id != 0) {
        UnloadShader(resolve_);
        resolve_ = Shader{ 0 };
    }
    if (resolved_.id != 0) {
        UnloadRenderTexture(resolved_);
        resolved_ = RenderTexture2D{};
    }
    hdr_.destroy();
    locExposure_ = -1;
}

bool ModelPreview::resize(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (valid() && width == this->width() && height == this->height()) return true;

    if (resolved_.id != 0) {
        UnloadRenderTexture(resolved_);
        resolved_ = RenderTexture2D{};
    }
    hdr_.destroy();

    return createTargets(width, height);
}

void ModelPreview::setSubjectBounds(BoundingBox bounds)
{
    if (equal(bounds, bounds_)) return;
    bounds_ = bounds;
    dirty_  = true;
}

void ModelPreview::setFraming(const Framing& framing)
{
    if (equal(framing, framing_)) return;
    framing_ = framing;
    framing_.pitchDegrees = Clamp(framing_.pitchDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
    framing_.zoom         = Clamp(framing_.zoom, kMinZoom, kMaxZoom);
    dirty_ = true;
}

void ModelPreview::setLighting(const Lighting& lighting)
{
    if (equal(lighting, lighting_)) return;
    lighting_ = lighting;
    dirty_    = true;
}

void ModelPreview::orbit(float yawDegrees, float pitchDegrees)
{
    if (yawDegrees == 0.0f && pitchDegrees == 0.0f) return;

    framing_.yawDegrees += yawDegrees;
    framing_.pitchDegrees =
        Clamp(framing_.pitchDegrees + pitchDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
    dirty_ = true;
}

void ModelPreview::zoomBy(float factor)
{
    if (factor <= 0.0f || factor == 1.0f) return;

    const float zoom = Clamp(framing_.zoom * factor, kMinZoom, kMaxZoom);
    if (zoom == framing_.zoom) return;

    framing_.zoom = zoom;
    dirty_ = true;
}

ModelPreview::Fit ModelPreview::fit() const
{
    const Vector3 centre = Vector3Scale(Vector3Add(bounds_.min, bounds_.max), 0.5f);

    /* THE BOUNDING SPHERE, off the box's diagonal. A sphere is the only shape
     * whose extent does not change as the turntable turns, so fitting to one
     * means the model neither clips nor breathes as it spins — which fitting
     * to the box's projected extent per angle would do. It over-covers a flat
     * object, and that is the price of a still frame. */
    float radius = Vector3Length(Vector3Subtract(bounds_.max, bounds_.min)) * 0.5f;
    if (!(radius > 0.0f)) radius = 0.5f;   /* degenerate or unset bounds */

    /* THE TIGHTER OF THE TWO HALF-ANGLES. raylib's fovy is vertical always, so
     * in a panel taller than it is wide the HORIZONTAL field is the narrow one
     * and fitting to the vertical would run the model off the sides. */
    const float halfVertical = framing_.fovDegrees * 0.5f * DEG2RAD;
    const float aspect =
        (valid() && height() > 0) ? static_cast<float>(width()) / static_cast<float>(height())
                                  : 1.0f;
    const float halfHorizontal = std::atan(std::tan(halfVertical) * aspect);
    const float halfAngle      = std::fmin(halfVertical, halfHorizontal);

    /* sin(halfAngle) = radius / distance is the sphere exactly touching the
     * cone; padding backs off from that and zoom drives in. */
    const float distance =
        (radius / std::sin(halfAngle)) * framing_.padding / std::fmax(framing_.zoom, kMinZoom);

    const float yaw        = framing_.yawDegrees * DEG2RAD;
    const float pitch      = framing_.pitchDegrees * DEG2RAD;
    const float horizontal = std::cos(pitch);

    /* Same frame as SunLight's azimuth — from +X toward +Z — which is what
     * makes "key light at yaw + 40 degrees" mean what it says. */
    const Vector3 offset{ horizontal * std::cos(yaw),
                          std::sin(pitch),
                          horizontal * std::sin(yaw) };

    Fit out;
    out.target   = Vector3Add(centre, Vector3Scale(framing_.focusOffset, radius));
    out.position = Vector3Add(out.target, Vector3Scale(offset, distance));

    /* Fitted to the subject rather than left at the global 0.01/1000. A single
     * object spans a handful of world units, and a near plane three orders of
     * magnitude closer than it needs to be spends the whole depth buffer on
     * empty space in front of the model. The slack absorbs focusOffset having
     * moved the target off the sphere's centre. */
    out.nearPlane = std::fmax(0.01f, distance - radius * 3.0f);
    out.farPlane  = distance + radius * 3.0f;
    return out;
}

Camera3D ModelPreview::camera() const
{
    const Fit placement = fit();

    Camera3D view{};
    view.position   = placement.position;
    view.target     = placement.target;
    view.up         = Vector3{ 0.0f, 1.0f, 0.0f };
    view.fovy       = framing_.fovDegrees;
    view.projection = CAMERA_PERSPECTIVE;
    return view;
}

void ModelPreview::applyLighting()
{
    /* Following means the offset is relative to where the camera stands, so
     * the key stays three-quarters off the view axis at every yaw. */
    const float azimuth = lighting_.followsCamera
                        ? framing_.yawDegrees + lighting_.keyAzimuthDegrees
                        : lighting_.keyAzimuthDegrees;

    sun_.setAzimuth(azimuth);
    sun_.setElevation(lighting_.keyElevationDegrees);
    sun_.tuning() = lighting_.tuning;
}

void ModelPreview::render(PbrShader& shader, const std::function<void()>& drawSubject)
{
    if (!valid() || !drawSubject || !dirty_) return;
    if (!shader.valid()) return;

    applyLighting();
    const Fit      placement = fit();
    const Camera3D view      = camera();

    /* ---- the studio's environment, pushed into the shared shader ----------
     * Not restored afterwards, and it does not need to be: the scene pass
     * re-establishes all of this before it draws. See the header for why that
     * pins where render() may be called from. */
    shader.updateEnvironment(sun_, view.position);
    shader.setLightmapEnabled(false);
    shader.setDebugView(0);
    shader.setSceneSize(hdr_.width(), hdr_.height());

    /* 1x1 white for every frame buffer the studio has no answer for. White is
     * "nothing occluded" and "nothing shadowed", which is the right answer
     * rather than a placeholder — there is no world here to occlude anything.
     * Shadow strength is already zero, from the no-shadow updateEnvironment. */
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1,
                           PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    shader.bindFrameTextures(white, white, white, white, white);
    shader.clearEnvironmentProbes();

    /* AND NO DECALS. Same screen-space problem, and white would not do here:
     * the DBuffer's identity is (0, 0, 0, 255), not white — white decodes as a
     * fully opaque white decal over everything, which is the worst available
     * answer. Switching the read off is both cheaper and unambiguous.
     *
     * A studio has nothing to decal in any case: the DBuffer holds the SCENE's
     * marks, so leaving it live would paint whatever is on the board across a
     * rifle floating in a UI panel. */
    shader.setDecalsEnabled(false);

    /* BeginMode3D reads the clip planes from rlgl's globals rather than from
     * the camera, so a per-pass frustum has to be swapped in around it — and
     * swapped back, because the scene's are not ours to leave changed. */
    const double previousNear = rlGetCullDistanceNear();
    const double previousFar  = rlGetCullDistanceFar();
    rlSetClipPlanes(static_cast<double>(placement.nearPlane),
                    static_cast<double>(placement.farPlane));

    {
        HdrTarget::Scope scope(hdr_);

        /* TRANSPARENT BLACK, and this is the whole feature. Alpha stays zero
         * wherever the model is not, which is what the resolve turns into a
         * cut-out and what lets the panel behind show through. */
        ClearBackground(BLANK);

        BeginMode3D(view);
        drawSubject();
        EndMode3D();
    }

    rlSetClipPlanes(previousNear, previousFar);

    resolve();
    dirty_ = false;
}

void ModelPreview::render(PbrShader& shader, const ModelAsset& asset,
                          const MaterialLibrary& library)
{
    if (!asset.valid()) return;

    /* Before the dirty check inside the other overload — a swapped model is
     * new bounds, and new bounds are what makes it dirty. */
    setSubjectBounds(asset.bounds());

    render(shader, [&asset, &library, &shader] {
        asset.drawLit(MatrixIdentity(), library, shader);
    });
}

void ModelPreview::resolve() const
{
    SetShaderValue(resolve_, locExposure_, &lighting_.exposure, SHADER_UNIFORM_FLOAT);

    /* POSITIVE source height, unlike ToneMapPass. Both targets are FBOs, so
     * this blit's own flip cancels the one the 3D pass introduced and the
     * result is an ordinary top-down texture. ToneMapPass needs the negative
     * rectangle precisely because the backbuffer is not an FBO and so does not
     * supply that second flip. */
    const Rectangle source{ 0.0f, 0.0f, hdr_.width(), hdr_.height() };
    const Rectangle destination{ 0.0f, 0.0f,
                                 static_cast<float>(width()),
                                 static_cast<float>(height()) };

    BeginTextureMode(resolved_);
    ClearBackground(BLANK);

    /* A STRAIGHT COPY, not a blend. The shader has already divided coverage
     * back out of the colour, so ordinary alpha blending would immediately
     * multiply it in again — the exact double-darkening the un-premultiply
     * exists to prevent. ONE/ZERO on both planes writes what the shader
     * computed, verbatim. */
    rlSetBlendFactorsSeparate(RL_ONE, RL_ZERO, RL_ONE, RL_ZERO, RL_FUNC_ADD, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    BeginShaderMode(resolve_);
    DrawTexturePro(hdr_.texture(), source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndBlendMode();

    EndTextureMode();
}

void ModelPreview::drawTo(Rectangle destination, Color tint) const
{
    if (!valid()) return;
    if (destination.width <= 0.0f || destination.height <= 0.0f) return;

    const float sourceAspect = static_cast<float>(width()) / static_cast<float>(height());
    const float destAspect   = destination.width / destination.height;

    /* LETTERBOXED INSIDE THE DESTINATION. A model stretched to a panel that is
     * not its shape is the one artifact that makes a rendered object read as a
     * bitmap, and it is invisible to whoever wrote the panel because they
     * usually made it square. */
    Rectangle fitted = destination;
    if (destAspect > sourceAspect) {
        fitted.width = destination.height * sourceAspect;
        fitted.x += (destination.width - fitted.width) * 0.5f;
    } else {
        fitted.height = destination.width / sourceAspect;
        fitted.y += (destination.height - fitted.height) * 0.5f;
    }

    const Rectangle source{ 0.0f, 0.0f,
                            static_cast<float>(width()),
                            static_cast<float>(height()) };
    DrawTexturePro(resolved_.texture, source, fitted, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
}

}  // namespace xcom
