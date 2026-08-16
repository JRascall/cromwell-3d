/* DeviceTexturePreviews.hpp — every intermediate the device renderer produces,
 * turned into something a panel can show.
 *
 * SINGLE RESPONSIBILITY: blit one device texture into a display-colour RGBA8
 * texture of its own, applying whatever interpretation makes that buffer
 * READABLE. It owns the targets and nothing else; it does not know what a dev
 * panel is, what ImGui is, or what any particular buffer means.
 *
 * ===================== WHY A BLIT AND NOT THE TEXTURE ITSELF ==============
 *
 * Because half the renderer's intermediates cannot be looked at directly, and
 * the ways they fail are all silent:
 *
 *   - THE SHADOW MAP AND THE PREPASS DEPTH ARE D32F. Sampled raw they arrive
 *     in the red channel with green and blue at zero, so the picture is a red
 *     wash — and worse, a perspective depth buffer spends almost its whole
 *     range in the first few metres, so everything past the near plane reads as
 *     flat white. "Every pixel is white" is exactly what a shadow map that was
 *     never rendered into looks like, which is the one thing the panel exists
 *     to tell apart.
 *   - SCENE COLOUR IS RGBA16F LINEAR RADIANCE, where the sun is genuinely
 *     several times one. Clamped to a display it is a blown-out white shape,
 *     which reads as an exposure bug in a buffer whose exposure has not been
 *     applied yet.
 *   - THE OCCLUSION PLANE IS ONE CHANNEL, and the G-buffer's ALPHA is
 *     roughness rather than coverage — a fourth channel nobody would guess at.
 *   - THE PROBES ARE A CUBEMAP ARRAY. There is no way to put one on a 2D panel
 *     without deciding a layout, and a cubemap array you cannot look at is
 *     worse to debug than a single cubemap, because now there is also the
 *     question of WHICH LAYER is wrong.
 *
 * So each preview names how its source should be READ, and that name is the
 * diagnostic value. The raylib renderer's `overlay/TexturePreviews` makes the
 * same argument and this is its device half; the two deliberately offer the
 * same interpretations so a preview can be compared across renderers.
 *
 * ====================== WHAT IT REFUSES TO DO =============================
 *
 * IT NEVER INVENTS. A mode makes a buffer visible; it does not make one up. A
 * slot whose source handle is invalid produces an invalid handle and the caller
 * says "not allocated" — because a diagnostic that draws a plausible picture of
 * a buffer that does not exist is worse than no panel at all. Same rule as the
 * zeros the dev panel shows for unconverted subsystems; see rhi/MIGRATION.md
 * §4.5.
 *
 * ======================= WHY IT IS THE ENGINE'S ===========================
 *
 * Every buffer it reads is `ScenePipeline`'s, and the reasons each one is hard
 * to look at are properties of rendering rather than of this game — a second
 * project on cromwell would write this again, unchanged, which is the test
 * §0 of the migration sets for where a seam belongs. It names no graphics API,
 * no UI toolkit and no game type; the panel that shows the result is the
 * game's, and that split is exactly the one DeviceImGuiRenderer describes.
 *
 * ================== COST, AND WHY THE CALLER MUST GATE IT =================
 *
 * One fullscreen blit into a small target per preview, per frame it is asked
 * for. That is nothing next to a frame — but it is not nothing next to zero,
 * and NOTHING HERE KNOWS WHETHER ANYBODY IS LOOKING. The caller is the only
 * one who does, and it should not ask while the panel is shut.
 */
#pragma once

#include "cromwell/rhi/Handles.hpp"

#include <cstdint>

namespace cromwell::rhi { class IRenderDevice; }

namespace cromwell {

class DeviceTexturePreviews {
public:
    /* HOW A BUFFER SHOULD BE READ, which is the whole diagnostic content of a
     * preview. Named after what the SOURCE is, not after the arithmetic —
     * `Roughness` rather than `Alpha` would be a lie the moment a second buffer
     * puts something else in its fourth channel, and `Alpha` is what the raylib
     * previews already call it. */
    enum class Mode {
        /* RGB straight out, alpha forced opaque. For 8-bit colour planes: the
         * encoded normals, the transmission plane. */
        Colour,

        /* The red channel as grey. One-channel planes — the occlusion buffer. */
        Red,

        /* The fourth channel as grey. The G-buffer's roughness. */
        Alpha,

        /* DEPTH, LINEARISED AGAINST THE PROJECTION AND BANDED. See
         * setDepthRange, and the long note in preview.fs.glsl on why a contrast
         * curve was tried first and measured as useless. Each band is an equal
         * slice of DISTANCE, which is what the raylib previews' note already
         * promises a reader. */
        Depth,

        /* LINEAR HDR RADIANCE, RANGE-COMPRESSED so a sun-lit surface and a
         * shadowed one are both visible. Not the frame's tone curve and not its
         * exposure: this is a look at the BUFFER, and borrowing the resolve's
         * exposure would make the preview change when a slider moved without
         * anything in the buffer changing. */
        Hdr,
    };

    /* One per intermediate worth looking at, plus room. Slots are the caller's
     * to allocate and are stable across frames, so a target is created once
     * rather than every time the panel is opened. */
    static constexpr int kMaxSlots = 16;

    explicit DeviceTexturePreviews(rhi::IRenderDevice& device);
    ~DeviceTexturePreviews();

    DeviceTexturePreviews(const DeviceTexturePreviews&) = delete;
    DeviceTexturePreviews& operator=(const DeviceTexturePreviews&) = delete;

    /* Loads the two shaders and builds the pipelines. False means no preview
     * can be produced; it has logged which stage failed, and — like every other
     * dev tool on this path — the caller carries on drawing the frame. */
    bool initialise();
    bool ready() const { return ready_; }

    /* Destroys every target, the pipelines and the shaders. Safe twice. */
    void release();

    /* ---- WHAT `Mode::Depth` HAS TO KNOW TO MEAN ANYTHING ------------------
     *
     * The projection a depth buffer was written with, and how far the display
     * ramp should span in world units.
     *
     * WITHOUT IT THE PREVIEW IS A PICTURE OF THE ENCODING RATHER THAN OF THE
     * SCENE. A perspective depth buffer is hyperbolic — with a 0.1 near plane
     * and a 1000 far plane, the whole visible world sits above 0.99 — so
     * anything that displays the stored value, however it is stretched, shows
     * the near clip plane's precision curve and not where the geometry is.
     * Inverting the projection turns it back into a distance, which is the only
     * form on which "each band is an equal slice of distance" is true.
     *
     * `span` RATHER THAN JUST USING `far`. The far plane is a thousand units
     * out and nothing on a tactical board is; ramping over it would put the
     * entire scene in the first two percent of the display. The caller passes
     * something scene-sized — the raylib previews use the world diagonal times
     * 1.5 — and the same number here keeps the two panels comparable.
     *
     * A ZERO OR NEGATIVE `far` MEANS THE BUFFER IS ALREADY LINEAR, which an
     * orthographic shadow map is. That is the default, so a caller that says
     * nothing gets the interpretation that needs no information. */
    DeviceTexturePreviews& withDepthRange(float nearPlane, float farPlane, float span);

    /* ONE PREVIEW, INTO THIS SLOT'S OWN TARGET, handed back for sampling.
     *
     * The target is created on first use and recreated if the size changes, so
     * a slot wants a STABLE size. `width` and `height` are the preview's own
     * resolution rather than anything the panel displays at — the panel scales
     * what it is given, and a size that followed a slider would rebuild a
     * render target every frame the slider was dragged.
     *
     * Returns an invalid handle when the source is invalid, when the slot is
     * out of range, or when the target could not be made. The caller must treat
     * that as "not allocated" and say so rather than drawing something. */
    rhi::TextureHandle render(int slot, rhi::TextureHandle source, Mode mode,
                              uint32_t width, uint32_t height);

    /* SIX FACES OF ONE PROBE, SIDE BY SIDE, in GL's face order: +X -X +Y -Y +Z
     * -Z. `probe` is the CUBE index, not the array slice — the shader multiplies
     * by six itself, because a caller that had to would eventually pass a slice
     * and get a preview one face out of step with its label.
     *
     * A STRIP RATHER THAN A CROSS. A cross wastes half its area and puts the
     * faces in an arrangement that only helps if you already know which way the
     * probe is facing; a strip is six labelled squares and fits a panel column.
     * It is the layout the raylib probe preview already uses, so the two can be
     * compared side by side. */
    rhi::TextureHandle renderCube(int slot, rhi::TextureHandle cubeArray, int probe,
                                  uint32_t faceSize);

private:
    /* Makes or remakes this slot's target. False leaves the slot empty. */
    bool ensureTarget(int slot, uint32_t width, uint32_t height);

    /* The common half of both renders: set the block, open the pass, draw. */
    void blit(int slot, rhi::PipelineHandle pipeline, rhi::TextureHandle source,
              float mode, float parameter);

    rhi::IRenderDevice& device_;
    bool ready_ = false;

    /* TWO SHADERS RATHER THAN ONE WITH A DUMMY BINDING. A fragment stage
     * declaring both a sampler2D and a samplerCubeArray needs BOTH bound on
     * every draw — a pipeline's bindings are the same every frame — so the 2D
     * previews would have to bind a stand-in cube array and the cube preview a
     * stand-in 2D. That is two resources to own and two more ways to be wrong
     * about a preview, to save one pipeline object. */
    rhi::ShaderHandle   shader_;
    rhi::PipelineHandle pipeline_;
    rhi::ShaderHandle   cubeShader_;
    rhi::PipelineHandle cubePipeline_;

    /* PINNED TO LEVEL ZERO, and it is the same two floats the ImGui backend
     * needed for the same reason: createSampler asks for a mipmapped
     * minification filter, and a texture whose chain does not reach it is
     * INCOMPLETE in GL and samples as zero. Every source here has one level.
     * See rhi/MIGRATION.md §4.10 and the sampler note in DeviceImGuiRenderer. */
    rhi::SamplerHandle  sampler_;

    rhi::BufferHandle   block_;

    /* Defaults to "already linear", which is what a shadow map is and what a
     * caller that never calls withDepthRange should get. */
    float nearPlane_ = 0.1f;
    float farPlane_ = 0.0f;
    float depthSpan_ = 1.0f;

    struct Slot {
        rhi::TextureHandle texture;
        uint32_t           width = 0;
        uint32_t           height = 0;
    };
    Slot slots_[kMaxSlots];
};

}  // namespace cromwell
