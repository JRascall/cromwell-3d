/* GlowPass.hpp — the stand-in for the scene bloom an unlit emissive would feed.
 *
 * SINGLE RESPONSIBILITY: own the HDR targets and the blur chain, and add the
 * halo back over the frame.
 *
 * Half res for the BLUR: a halo has no detail for the eye to miss, and it
 * quarters the work of every iteration.
 *
 * Full res for the SOURCE, which is a different question entirely. That pass
 * is geometry, and it goes into an FBO — so unlike the lit pass it has none of
 * the backbuffer's MSAA under it. The ribbon is under two pixels wide at a
 * normal camera; at half res that is one sample per two screen pixels of a
 * line drawn at emissive 4.5, which comes out a staircase that the blur then
 * smears faithfully and the composite magnifies 2x on the way back. Extracting
 * at full res and box-filtering 2:1 into the blur chain is 4x supersampling of
 * the halo for one extra pass over a few percent of the screen.
 *
 * This is also what a real bloom does: Source 2 extracts from the RESOLVED
 * scene buffer, never by re-rasterising into an unresolved one.
 */
#pragma once

#include "raylib.h"

#include "render/gpu/HdrTarget.hpp"
#include "render/ribbon/RibbonRenderer.hpp"
#include "render/ribbon/RibbonTuning.hpp"

namespace xcom {

class GlowPass {
public:
    GlowPass() = default;
    ~GlowPass();

    GlowPass(const GlowPass&) = delete;
    GlowPass& operator=(const GlowPass&) = delete;

    /* Loads the blur shader once. */
    bool loadShader();
    void resize(int screenWidth, int screenHeight);

    bool available() const { return available_; }

    /* How hard the halo is driven. Read at render time, so it can move between
     * frames; see RibbonTuning. */
    void setTuning(const RibbonTuning& tuning) { tuning_ = tuning; }

    /* Call in 2D, straight after EndMode3D(), with the same settings the main
     * ribbon draw got. Cheap no-op if the targets failed to allocate. */
    void render(const RibbonRenderer& ribbons,
                const RibbonPassSettings& settings,
                Texture2D sceneDepth);

private:
    /* Draw a render target over a whole other one, source flipped because
     * raylib's FBOs are bottom-up. */
    static void blit(Texture2D source, float destWidth, float destHeight);

    /* One separable half of the blur: source -> destination, cleared first,
     * taps `stepX/stepY` texels apart. */
    void blurPass(HdrTarget& destination, Texture2D source,
                  float stepX, float stepY, float scale) const;

    void setBlurUniforms(float dirX, float dirY, float scale) const;

    Shader    blurShader_ = { 0 };
    int       locTexelDir_ = 0;
    int       locScale_ = 0;

    HdrTarget source_;   /* full res: the ribbon re-drawn overbright   */
    HdrTarget pingA_;    /* half res, ping-ponged by the widening blur */
    HdrTarget pingB_;
    HdrTarget sum_;      /* every iteration accumulated                */
    bool      available_ = false;

    RibbonTuning tuning_;   /* defaults are the recovered constants */
};

}  // namespace xcom
