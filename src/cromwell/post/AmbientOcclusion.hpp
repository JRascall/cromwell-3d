/* AmbientOcclusion.hpp — the SSAO chain.
 *
 * SINGLE RESPONSIBILITY: own the occlusion targets, the sampling kernel and
 * the two passes that fill them, and hand out the result as a texture.
 *
 * WHAT IT ANSWERS. The shadow map says what the SUN cannot reach; this says
 * what the SKY cannot reach. Untextured boxes need the second badly: without
 * it every inside corner takes the whole hemisphere as though it were open
 * ground, and the geometry reads as flat shapes laid on the floor rather than
 * standing on it. It is the screen-space stand-in for the contact darkening a
 * baked light-probe solve would resolve properly.
 *
 * It reads the scene prepass — depth from the depth attachment, world normals
 * from the colour plane that framebuffer had to carry anyway (see
 * DepthTarget.hpp). So the whole effect costs two fullscreen passes and no
 * extra geometry submission.
 *
 * DEGRADES, NEVER FAILS. If a shader or a target is missing, texture() hands
 * back rlgl's 1x1 white and the lit shader multiplies its ambient by one.
 */
#pragma once

#include "raylib.h"

#include <array>

namespace cromwell {

class AmbientOcclusion {
public:
    /* Enough directions that the blur has something to average, few enough
     * that the whole pass stays a rounding error at this scene's complexity. */
    static constexpr int kKernelSize = 24;

    AmbientOcclusion() = default;
    ~AmbientOcclusion();

    AmbientOcclusion(const AmbientOcclusion&) = delete;
    AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;

    bool load();
    void resize(int width, int height);

    bool available() const { return shadersLoaded_ && targetsValid_; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }
    bool active() const { return available() && enabled_; }

    /* The three numbers the occlusion shader reads. */
    struct Tuning {
        /* World units are tiles, so the default is just under half a tile —
         * roughly two thirds of a metre at XCOM's 96uu scale. Large enough to
         * darken where a wall meets a floor, small enough that a crate does
         * not shade the whole tile it stands on. */
        float radius = 0.45f;

        /* In view-space units, which here are world units — the view matrix is
         * a rigid transform, so no scale creeps in.
         *
         * IT HAS TO BE SMALL RELATIVE TO THE KERNEL. The kernel packs most of
         * its taps close to the origin, and any tap nearer than the bias is
         * rejected before it can occlude anything; set to 0.022 against an
         * innermost tap at 0.034 this silently discarded most of the samples
         * and the whole effect came out as a faint outline. Against the
         * current kernel the nearest tap is at 0.11, an order of magnitude of
         * headroom. Raising this is the fastest way to turn SSAO off by
         * accident.
         *
         * IT ALSO SETS THE SELF-TAP FLOOR. ssao.fs.glsl rejects any tap whose
         * occluder is closer than this in three dimensions, on the grounds that
         * a tap which came back to the point being shaded has a direction made
         * of floating-point noise. Moving this number moves that floor too.
         *
         * Briefly raised to 0.025 while a flat facade was coming out patterned
         * with rectangles. That turned out to be the prepass compositing the
         * G-buffer instead of writing it, not anything the bias could fix, so
         * this is back where it was tuned. */
        float bias = 0.008f;

        /* Deliberately short of 1. Full strength drives inside corners to
         * black, which on a tactical board hides the cover the player is
         * reading; this is contact darkening, not a second shadow. */
        float strength = 1.0f;
    };

    Tuning&       tuning()       { return tuning_; }
    const Tuning& tuning() const { return tuning_; }

    /* Runs occlusion then blur. Call after the scene prepass and before the
     * lit pass, outside any target scope. `depth` and `normals` are the
     * prepass attachments, and must be the size passed to resize(). */
    void render(const Camera3D& camera, Texture2D depth, Texture2D normals);

    /* The blurred occlusion, or 1x1 white when inactive. */
    Texture2D texture() const;

private:
    void buildKernel();
    void destroyTargets();

    Shader occlusionShader_ = { 0 };
    Shader blurShader_ = { 0 };

    RenderTexture2D raw_ = { 0 };
    RenderTexture2D blurred_ = { 0 };

    int  width_ = 0;
    int  height_ = 0;
    bool shadersLoaded_ = false;
    bool targetsValid_ = false;
    bool enabled_ = true;
    Tuning tuning_;

    /* Flat xyz triples, the layout SetShaderValueV wants for a vec3 array. */
    std::array<float, kKernelSize * 3> kernel_{};

    int locDepth_ = -1;
    int locNormals_ = -1;
    int locProjection_ = -1;
    int locInverseProjection_ = -1;
    int locView_ = -1;
    int locResolution_ = -1;
    int locRadius_ = -1;
    int locBias_ = -1;
    int locStrength_ = -1;
    int locKernel_ = -1;
    int locBlurResolution_ = -1;

    /* The blur is BILATERAL, so it needs the same depth buffer and the same
     * unprojection the occlusion pass used — see ssao_blur.fs.glsl. */
    int locBlurDepth_ = -1;
    int locBlurInverseProjection_ = -1;
};

}  // namespace cromwell
