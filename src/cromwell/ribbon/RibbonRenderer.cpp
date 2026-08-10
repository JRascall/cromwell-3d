#include "cromwell/ribbon/RibbonRenderer.hpp"

#include "raymath.h"
#include "rlgl.h"

namespace cromwell {

void RibbonRenderer::submit(const RibbonPassSettings& settings,
                            float emissive,
                            float viewWidth, float viewHeight,
                            Texture2D sceneDepth) const
{
    shader_.setViewport(viewWidth, viewHeight);
    shader_.setCamera(settings.camera);
    shader_.setTime(static_cast<float>(GetTime()));
    shader_.setHideHeight(settings.hideHeight);
    shader_.setEmissive(emissive);
    shader_.setDepthTexture(sceneDepth);

    const Matrix identity = MatrixIdentity();

    rlDisableBackfaceCulling();   /* TwoSided */
    rlDisableDepthMask();         /* translucent: never writes depth */
    rlDisableDepthTest();         /* bDisableDepthTest — the DestDepth compare
                                   * in the shader IS the test */

    for (const StripSet::Strip& strip : meshes_.strips()) {
        if (!settings.visibleRings.contains(strip.ring)) continue;
        if (strip.storey > settings.maxStorey) continue;

        /* BorderRelevance picks which of the profile's two channels to draw: 1
         * is the line that stands still, 0 the one that scrolls in dashes. It
         * is a property of the CURSOR, not of the band — the ring the cursor
         * is inside is the relevant one and draws solid, and any other ring on
         * screen stays up as a dashed outline rather than being hidden. */
        shader_.setColour(strip.colour);
        shader_.setRelevance(strip.ring == settings.solidRing ? 1.0f : 0.0f);
        DrawMesh(strip.mesh, shader_.material(), identity);
    }

    rlEnableDepthTest();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

}  // namespace cromwell
