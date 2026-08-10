/* DecalBuffer.hpp — the DBuffer: what the decals decided, before any lighting.
 *
 * SINGLE RESPONSIBILITY: own the three-attachment MRT the decal pass writes
 * and the lit pass reads, and hand the planes out by name.
 *
 * WHY A BUFFER AT ALL, AND NOT JUST DRAWING THE DECAL. A decal that is drawn
 * as its own lit quad has to relight itself, which means it needs the receiver's
 * shadow term, its probe, its lightmap texel and its ambient occlusion — every
 * input the surface underneath already computed. Blending the decal's MATERIAL
 * into the surface instead and letting the surface light once is Unreal's
 * DBuffer and Source 2's projected decals both, and it is the only arrangement
 * where a decal on a wall takes that wall's shadow for free.
 *
 * IT IS NOT A G-BUFFER, and the distinction is the same one GBuffer.hpp draws
 * for itself. Nothing here describes the world; it describes an OVERRIDE of the
 * world that pbr.fs.glsl applies to its own material inputs before it lights
 * them. The lighting is still forward, still per material, still in one pass.
 *
 * THE PLANES
 *   albedo()    rgb = base colour, premultiplied      a = 1 - coverage
 *   normal()    rgb = world normal * 0.5 + 0.5, prem. a = 1 - coverage
 *   surface()   r = metalness  g = roughness  b = emissive mask, prem.
 *                                                    a = 1 - coverage
 *
 * COVERAGE IS STORED INVERTED because that is what makes one blend equation do
 * the whole job. Every plane is cleared to (0, 0, 0, 1) — no ink, base fully
 * intact — and each decal blends with
 *
 *     rgb:   src            + dst * src.a          (over, premultiplied)
 *     alpha: 0              + dst * src.a          (transmittance multiplies)
 *
 * so N overlapping decals leave dst.a = prod(1 - coverage_i), exactly the
 * fraction of the base material still showing through, and dst.rgb already
 * holds the decals' own contribution. The lit shader's whole decode is then
 * `decalRgb + baseValue * dbufferAlpha` — see common/dbuffer.glsl.
 *
 * WHY THREE PLANES AND NOT FOUR. Alpha is the blend weight in every one of
 * them, so a plane carries three channels of payload, and albedo(3) +
 * normal(3) + metal/rough(2) is eight — three planes with one channel spare.
 * That spare channel is the emissive MASK rather than an emissive colour: a
 * decal's glow takes its hue from its own albedo, the way $selfillum does in
 * Source, which buys a self-lit decal for one channel instead of a fourth
 * attachment and a fourth texture unit. A decal that must glow a different
 * colour from its base is the thing that would justify growing this.
 *
 * RESOLUTION IS THE PREPASS'S, NOT THE SCENE'S, and that is deliberate rather
 * than an oversight. The decal pass reconstructs world position from the
 * prepass depth texture, so a DBuffer finer than the depth it unprojects would
 * be inventing precision it does not have. The lit pass runs supersampled and
 * samples this bilinearly, which lands decal detail at display resolution —
 * where it is seen. The supersampling exists for the hard geometric edges of
 * untextured boxes (see ToneMapPass), not for texture detail.
 *
 * DEGRADES, NEVER FAILS. With no targets allocated the planes hand back a 1x1
 * (0, 0, 0, 255) texture, which decodes to "no decal touched this pixel" in all
 * three — the lit shader keeps its own material and never learns the system is
 * missing.
 *
 * RAII: the destructor releases the three textures and the FBO.
 */
#pragma once

#include "raylib.h"

namespace xcom {

class DecalBuffer {
public:
    DecalBuffer() = default;
    ~DecalBuffer();

    DecalBuffer(const DecalBuffer&) = delete;
    DecalBuffer& operator=(const DecalBuffer&) = delete;

    /* Must be the size of the depth target the decal pass unprojects. */
    void resize(int width, int height);

    bool valid() const { return framebuffer_ != 0; }

    /* The cleared state of every plane: no ink, base fully intact. Public
     * because the pass has to clear to exactly this and there must be one
     * definition of what "empty" is. */
    static constexpr Color kEmpty{ 0, 0, 0, 255 };

    Texture2D albedo()  const;
    Texture2D normal()  const;
    Texture2D surface() const;

    int width()  const { return width_; }
    int height() const { return height_; }

    /* Binds the framebuffer, enables all three draw buffers and sets the
     * viewport for the block's lifetime; restores a single draw buffer and the
     * default framebuffer on the way out.
     *
     * BeginTextureMode ALONE IS NOT ENOUGH. It binds the FBO and fixes the
     * viewport but never touches glDrawBuffers, so without the
     * rlActiveDrawBuffers(3) below only attachment 0 would receive anything and
     * the normal and surface planes would stay at their cleared value — a decal
     * that tints but neither bumps nor roughens, with no error anywhere. */
    class Scope {
    public:
        explicit Scope(const DecalBuffer& buffer);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        bool active_ = false;
    };

private:
    void destroy();

    unsigned int framebuffer_ = 0;

    /* Held as full Texture2D rather than ids because that is what every
     * consumer wants to be handed. */
    Texture2D albedo_{};
    Texture2D normal_{};
    Texture2D surface_{};

    /* The 1x1 stand-in handed out when there are no targets. Allocated once,
     * lazily, and shared by all three planes — they decode identically from
     * (0, 0, 0, 255). */
    static Texture2D emptyTexture();

    int width_ = 0;
    int height_ = 0;
};

}  // namespace xcom
