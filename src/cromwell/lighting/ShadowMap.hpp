/* ShadowMap.hpp — the sun's depth buffer.
 *
 * SINGLE RESPONSIBILITY: own the depth target the world is rendered into from
 * the sun's point of view, and the trivial shader that fills it.
 *
 * ONE MAP, NO CASCADES. Cascades exist to concentrate texels where a view
 * frustum needs them, and this world is a bounded 24x24 board: a single
 * orthographic projection over the whole lattice already lands roughly sixty
 * texels per tile, which is far past what hard-edged boxes can show. See
 * SunLight::viewProjectionFor.
 *
 * IT REUSES DepthTarget, colour attachment and all. A depth-only framebuffer
 * would be the leaner thing, but OpenGL calls an FBO with no colour buffer
 * incomplete unless the draw buffer is set to NONE, and rlgl exposes no way to
 * do that. The unused colour plane is the price of staying inside raylib.
 *
 * The caster material is deliberately shared with the ribbon's depth prepass:
 * that pass wants depth and nothing else too, and running it through the lit
 * shader was paying for shadow lookups and a whole BRDF per fragment to
 * produce a number the fragment shader never reads.
 */
#pragma once

#include "raylib.h"

#include "cromwell/gpu/target/DepthTarget.hpp"
#include "cromwell/lighting/SunLight.hpp"

namespace cromwell {

class ShadowMap {
public:
    /* THE EDGE WOBBLE IS ONE TEXEL, SO THE TEXEL IS WHAT HAS TO SHRINK.
     *
     * A shadow edge running at a shallow angle to the shadow map's texel grid
     * steps sideways by a whole texel every so often — at three degrees off
     * the grid, once every twenty texels. That long, one-texel-high staircase
     * is what reads as regular scallops along what is geometrically a dead
     * straight roof line, and no amount of filtering removes it: smoothing
     * inside a texel cannot flatten a step a whole texel high.
     *
     * Two things hide it, and both need the texel to be small. This is the
     * first; the second is a PCF kernel wide enough that the wobble lives
     * inside the penumbra rather than on top of it (see pbr.fs.glsl).
     *
     * Affordable only because the colour plane is R8 — see DepthTarget. */
    static constexpr int kResolution = 4096;

    ShadowMap() = default;
    ~ShadowMap();

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    /* Loads the depth-only shader and allocates the target. False means the
     * caller should shade without shadows rather than draw nothing. */
    bool load();

    bool valid() const { return loaded_; }

    /* The material every shadow caster is drawn with — position in, depth out,
     * no lighting work at all. */
    const Material& casterMaterial() const { return material_; }

    /* For surfaces that transmit rather than block. Drawn after the opaque
     * casters, with depth writes off — see glass_shadow.fs.glsl. */
    const Material& transmitterMaterial() const { return glassMaterial_; }

    /* What the transmitting surface is made of: its grime map, and
     * (remapMin, remapMax, uvScale, cleanPaneTransmittance). Set once at
     * startup from the window material — the shadow map draws every pane with
     * one material, so this is the whole world's glass, not one pane's. */
    void setTransmitter(Texture2D translucency, Vector4 transmit);

    /* Records which parts of the map the sun reached only through glass. */
    Texture2D transmissionTexture() const { return target_.colourTexture(); }

    Texture2D depthTexture() const { return target_.depthTexture(); }
    Vector2   texelSize() const;
    Matrix    viewProjection() const { return projection_.viewProjection; }
    float     worldTexelSize() const { return projection_.worldTexelSize; }
    float     depthRange() const { return projection_.depthRange; }

    /* Renders into the shadow map for the block's lifetime. Draw casters with
     * casterMaterial() inside it; nothing else about the pass has any state
     * the caller needs to manage.
     *
     * Substitutes the light's matrix for the camera's WITHOUT BeginMode3D,
     * which would install the player camera instead. DrawMesh builds its mvp
     * as model x modelview x projection, so the light matrix goes in as the
     * modelview with an identity projection beside it. */
    class Scope {
    public:
        Scope(ShadowMap& map, const SunLight::ShadowProjection& projection);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        /* Declared first so it is CONSTRUCTED first — BeginTextureMode has to
         * happen before the clear — and destroyed last, so the matrices are
         * restored while the target is still bound. */
        DepthTarget::Scope targetScope_;
        ShadowMap&         map_;
    };

private:
    DepthTarget target_;
    Shader      shader_ = { 0 };
    Material    material_ = { 0 };
    Shader      glassShader_ = { 0 };
    Material    glassMaterial_ = { 0 };
    int         locGlassTransmit_ = -1;
    bool        loaded_ = false;

    SunLight::ShadowProjection projection_ = { { 0 }, 1.0f };
};

}  // namespace cromwell
