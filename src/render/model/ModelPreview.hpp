/* ModelPreview.hpp — one model, lit and framed, on nothing at all.
 *
 * SINGLE RESPONSIBILITY: own an off-screen studio — a target, a camera fitted
 * to whatever is being shown, and a light rig — and hand out the result as a
 * texture with a transparent background.
 *
 * THIS IS NOT A UI WIDGET. It renders and it produces a Texture2D; where that
 * texture lands is entirely the caller's business. An ImGui panel, a HUD
 * rectangle and a screenshot are all the same customer as far as this class is
 * concerned, which is what keeps it usable before any of them exist.
 *
 * WHY IT IS A SECOND PASS AND NOT A CORNER OF THE FIRST. The scene camera is
 * looking at a tactical board; an inventory model has to be centred, filled to
 * the panel, lit consistently regardless of the time of day the battle is
 * happening at, and readable against no background. None of that is a viewport
 * of the world — it is a different world with one object in it. So it gets its
 * own framebuffer and its own sun.
 *
 * IT IS DRAWN ONLY WHEN IT CHANGES. A preview holds still for hundreds of
 * frames at a time: the model, the angle and the light rig are all things a
 * player changes deliberately. Every mutator marks the target dirty and
 * render() is a no-op until one does, so a panel that is merely OPEN costs a
 * textured quad per frame rather than a scene pass.
 *
 * ---- WHERE IT HAS TO BE CALLED FROM ------------------------------------
 *
 * render() PUSHES ITS OWN LIGHTING INTO THE SHARED PbrShader and does not put
 * back what was there. There is nothing to put back — the shader has no notion
 * of saved state, and faking one would mean shadowing every uniform it owns.
 *
 * That is safe for exactly one reason, and the reason is worth stating because
 * it is the thing a future change could break: the scene pass re-establishes
 * the ENTIRE environment every frame before it draws anything (updateEnvironment,
 * setSceneSize, setShadowsEnabled, bindFrameTextures, setEnvironmentProbes). So
 * call render() BEFORE the scene pass — the top of the frame is the natural
 * place — and the scene overwrites whatever the studio left behind.
 *
 * Calling it AFTER the scene has been set up but BEFORE it has been drawn will
 * light the battlefield with the studio rig. Calling it after the frame is
 * resolved works too, but then the UI showing the texture is a frame behind.
 *
 * ---- WHAT THE STUDIO DELIBERATELY DOES NOT HAVE ------------------------
 *
 *   shadows       there is no ground for one to fall on, and ShadowMap is a
 *                 fixed 4096 square — eighty megabytes to darken one side of a
 *                 rifle. The sun still shades; it just does not occlude.
 *   occlusion     SSAO is screen space, addressed against the scene's size.
 *                 Inside a 256-pixel panel those coordinates mean nothing, so
 *                 the pass binds 1x1 white — "nothing occluded", the only
 *                 honest answer to a question that cannot be asked here.
 *   a probe       reflections are parallax-corrected against the board's
 *                 bounding box. An object in a UI panel is nowhere near it,
 *                 and reflecting the world's cubemap there paints a wall onto
 *                 a pistol. The analytic sky is both cheaper and correct.
 *
 * What is left is a key light and a sky — which is what a model viewer has
 * always been, and reads as studio lighting rather than as a missing feature.
 */
#pragma once

#include "raylib.h"

#include "render/gpu/HdrTarget.hpp"
#include "render/lighting/SunLight.hpp"
#include "render/post/ToneMapPass.hpp"

#include <functional>

namespace xcom {

class MaterialLibrary;
class ModelAsset;
class PbrShader;

class ModelPreview {
public:
    /* Same trick and the same factor as the scene: render at twice the panel
     * on each axis and let the resolve's bilinear tap average each 2x2 block.
     * It matters MORE here than it does there. A preview is a cut-out — its
     * silhouette is a hard edge against nothing, with no background detail to
     * hide the stair-stepping, and the window's MSAA does not reach an FBO. */
    static constexpr int kSupersampleFactor = 2;

    /* A square, because a preview panel usually is and a model fitted to a
     * square reads the same whichever way the panel is later stretched. */
    static constexpr int kDefaultSize = 256;

    /* WHERE THE CAMERA IS, in terms of the subject rather than the world — the
     * whole point of a fitted preview is that a pistol and a dropship are
     * framed identically without anyone hand-placing a camera for each.
     *
     * Yaw and pitch are the same frame SunLight's azimuth is in, measured from
     * +X toward +Z, which is what lets the key light track the turntable by
     * simply adding an offset. */
    struct Framing {
        float yawDegrees   = 35.0f;
        float pitchDegrees = 18.0f;

        /* A LONG LENS, on purpose. Wide angles are for standing inside a
         * space; an object held up for inspection wants the compressed,
         * product-photograph look a narrow field gives, and 30 degrees is
         * short of the point where the near corner of a crate starts to
         * loom. */
        float fovDegrees = 30.0f;

        /* Multiplies the fitted distance's inverse: above 1 is closer in.
         * Zoom is separate from padding so a player's scroll wheel cannot
         * destroy the framing it started from. */
        float zoom = 1.0f;

        /* Margin around the fitted sphere. The sphere is the model's bounding
         * DIAGONAL, so it already over-covers a flat object; this is the extra
         * that keeps a silhouette off the panel edge as it turns. */
        float padding = 1.10f;

        /* Shifts what the camera aims at, in the subject's own units — for a
         * portrait that should frame the head rather than the centre of mass.
         * Scaled by the subject's radius, so it survives a change of model. */
        Vector3 focusOffset{ 0.0f, 0.0f, 0.0f };
    };

    /* THE RIG. Not the battlefield's sun: a preview must look the same at dusk
     * as at noon, or an inventory icon changes colour as the mission wears on.
     * These are studio numbers and they do not come from SunLight's defaults. */
    struct Lighting {
        /* Where the key sits relative to the CAMERA when it follows, or in
         * world azimuth when it does not. Three-quarters off the view axis:
         * head-on light flattens everything it touches, and this is the angle
         * that puts a lit face and a shaded face on the same object. */
        float keyAzimuthDegrees   = 40.0f;
        float keyElevationDegrees = 45.0f;

        /* THE KEY TRACKS THE TURNTABLE. Fixed in world space, spinning a model
         * carries it through its own shadow and half of every rotation is an
         * unlit silhouette. Following the camera means the object is lit the
         * same way from every angle the player chooses to look at it, which is
         * what every model viewer since HLMV has done. */
        bool followsCamera = true;

        /* Ambient well above the scene's. Outdoors the sky is a dim fill next
         * to the sun and shadows are meant to read as harsh; a lone object on
         * nothing has no bounce from anywhere, so the same ratio drops its
         * shaded side to black and the silhouette loses its far edge. */
        SunLight::Tuning tuning{ /* peak    */ 3.4f,
                                 /* tint    */ { 1.0f, 1.0f, 1.0f },
                                 /* ambient */ 1.15f,
                                 /* skyTint */ { 1.0f, 1.0f, 1.0f },
                                 SunLight::kAngularRadius };

        /* Its own, not ToneMapPass's — the scene's exposure is a live dev
         * tunable and a UI thumbnail must not change brightness because
         * somebody dragged a slider looking at the board. */
        float exposure = ToneMapPass::kDefaultExposure;
    };

    ModelPreview() = default;
    ~ModelPreview();

    ModelPreview(const ModelPreview&) = delete;
    ModelPreview& operator=(const ModelPreview&) = delete;

    /* Allocates both targets and loads the resolve shader. False means the
     * caller should not show a preview panel at all — unlike the scene's
     * passes there is no degraded version of this worth falling back to. */
    bool create(int width = kDefaultSize, int height = kDefaultSize);
    void destroy();

    bool valid() const { return resolved_.id != 0 && hdr_.valid(); }

    int width() const  { return resolved_.texture.width; }
    int height() const { return resolved_.texture.height; }

    /* Reallocates only when the size actually changes — a panel being dragged
     * would otherwise rebuild two framebuffers every frame of the drag. */
    bool resize(int width, int height);

    /* WHAT IS BEING LOOKED AT, in its own space. This is what the camera is
     * fitted to, so it must be set before the first render or the subject is
     * framed against a unit box and lands wherever it lands. The ModelAsset
     * overload of render() sets it from the asset's own bounds. */
    void setSubjectBounds(BoundingBox bounds);
    BoundingBox subjectBounds() const { return bounds_; }

    const Framing& framing() const { return framing_; }
    void setFraming(const Framing& framing);

    const Lighting& lighting() const { return lighting_; }
    void setLighting(const Lighting& lighting);

    /* Drag-to-turn, in degrees. Pitch is clamped short of the poles, where the
     * look-at up vector goes parallel to the view direction and the image
     * rolls over. */
    void orbit(float yawDegrees, float pitchDegrees);

    /* Scroll-to-zoom. Multiplies rather than adds, so a step feels the same
     * size at every distance, and clamps to a range the fit stays sane in. */
    void zoomBy(float factor);

    /* Force a redraw. Needed when something the preview cannot see changes —
     * a material retextured, a model swapped behind the same bounds. Every
     * setter here already does this for itself. */
    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }

    /* The fitted camera. Exposed because a caller that wants click-to-select
     * or drag-in-world needs the same matrices this pass used, and rebuilding
     * them by hand beside it is how the two drift apart. */
    Camera3D camera() const;

    /* THE GENERAL FORM. `drawSubject` runs inside the studio with the camera
     * and the lighting already installed, exactly as ReflectionProbeSet::capture
     * does — the caller owns what the geometry is, because this class has no
     * business knowing whether a preview is one prop, a soldier, or a squad.
     *
     * Draw in the subject's own coordinates; the camera is already fitted to
     * the bounds you declared. A no-op unless the preview is dirty.
     *
     * WHATEVER YOU DRAW MUST CARRY TANGENTS. The lit shader builds its normal
     * through a TBN, so a mesh with no tangent attribute normalises a zero
     * vector, and the NaN that produces spreads through every term: the model
     * comes out a single flat colour with no shading at all, which reads far
     * more like "the light rig is broken" than like "this mesh is missing an
     * attribute". ModelAsset::load already generates them, so the overload
     * below is safe; a hand-built or GenMesh* mesh needs GenMeshTangents
     * calling on it first.
     *
     * See the header comment for where in the frame this may be called. */
    void render(PbrShader& shader, const std::function<void()>& drawSubject);

    /* The common case: one asset at its own origin, framed from its own
     * bounds. Sets the subject bounds as a side effect, so it needs no
     * setSubjectBounds() beforehand. */
    void render(PbrShader& shader, const ModelAsset& asset,
                const MaterialLibrary& library);

    /* STRAIGHT ALPHA, ORIGIN TOP LEFT — an ordinary texture, in other words,
     * with none of the two gotchas an FBO normally carries. The resolve
     * un-premultiplies and flips, so this needs no negative source rectangle
     * and no premultiplied blend mode: DrawTexturePro, rlImGuiImage and
     * ImGui::Image all handle it correctly as-is. See model_preview.fs.glsl. */
    Texture2D texture() const { return resolved_.texture; }

    /* Scaled into `destination`, aspect preserved and centred within it — a
     * preview stretched to a panel that is not its shape is the one artifact
     * that makes a rendered model look like a bitmap. */
    void drawTo(Rectangle destination, Color tint = WHITE) const;

private:
    /* Where the camera sits and how far the clip planes go, all derived from
     * the subject's bounds. One function because the three answers have to
     * agree: a near plane fitted to a different distance than the camera uses
     * slices the front off the model. */
    struct Fit {
        Vector3 target;
        Vector3 position;
        float   nearPlane;
        float   farPlane;
    };
    Fit fit() const;

    /* Applies the rig to sun_, resolving followsCamera against the framing. */
    void applyLighting();

    /* The two framebuffers, without the shader — so a panel being dragged
     * reallocates its targets rather than recompiling a program every frame
     * of the drag. */
    bool createTargets(int width, int height);

    /* Supersampled linear HDR down to a panel-sized, straight-alpha RGBA8. */
    void resolve() const;

    HdrTarget       hdr_;         /* supersampled, linear, with depth */
    RenderTexture2D resolved_{};  /* panel-sized RGBA8, what callers see */
    Shader          resolve_{};
    int             locExposure_ = -1;

    SunLight    sun_;
    Framing     framing_;
    Lighting    lighting_;
    BoundingBox bounds_{ { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };

    bool dirty_ = true;
};

}  // namespace xcom
