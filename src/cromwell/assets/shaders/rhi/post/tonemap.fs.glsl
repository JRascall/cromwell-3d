#version 450 core
/* tonemap.fs.glsl — linear HDR radiance in, display pixels out.
 *
 * Converted from ../tonemap.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * THE ONLY STAGE THAT KNOWS WHAT A SCREEN IS. Everything upstream works in
 * unbounded linear radiance where the sun is genuinely forty times brighter
 * than a lit wall; this is where that range is compressed into the zero-to-one
 * a monitor can show, and where the sRGB transfer curve is applied.
 *
 * The curve itself is common/filmic.glsl, shared with the raylib path's resolve
 * — so the two renderers cannot drift on the one thing an A/B comparison is
 * most sensitive to.
 *
 * WHAT THE CONVERSION CHANGED: texture0 and colDiffuse are gone, along with the
 * textured quad raylib blitted them with. This is a covering triangle with no
 * vertex buffer, sampling one explicitly-bound texture.
 */
#include "common/filmic.glsl"

layout(binding = 0) uniform sampler2D uScene;

/* THE OCCLUSION PLANE, for the diagnostic view only — see uExposureAndFlags.z. */
layout(binding = 1) uniform sampler2D uOcclusion;

layout(std140, binding = 1) uniform ResolveBlock {
    vec4 uExposureAndFlags;   /* x = exposure, y != 0 means apply the curve */
    vec4 uOutputTexel;        /* xy = one BACKBUFFER pixel in UV. zw spare    */
};

layout(location = 0) out vec4 outColour;

void main()
{
    /* THIS IS WHERE THE SUPERSAMPLE IS RESOLVED, and it is one texture read.
     *
     * The scene target is exactly twice the backbuffer on each axis, so the
     * centre of an output pixel — gl_FragCoord.xy is already at a half-pixel
     * offset — lands exactly on the corner shared by four source texels. A
     * bilinear tap there returns all four averaged, with equal weights, for the
     * price of one fetch. See kSupersample in ScenePipeline.cpp.
     *
     * texelFetch WOULD BE WRONG HERE, and silently: it ignores the sampler, so
     * it would read the single texel at the output pixel's index — one sample
     * in four, the top-left of each block, and the other three discarded. The
     * image would look correct and be aliased exactly as if nothing had been
     * supersampled, which is the version of this bug that survives review.
     *
     * The raylib path samples with a negative source height to flip raylib's
     * bottom-up FBOs. Nothing to flip here — the device renders into a texture
     * whose origin convention this pass and the lit pass agree on. */
    vec3 radiance = texture(uScene, gl_FragCoord.xy * uOutputTexel.xy).rgb;

    /* ---- the diagnostic views ------------------------------------------
     *
     * SHOWN HERE RATHER THAN IN THE LIT PASS, because a view that REPLACES the
     * picture wants to bypass the tone curve as well: occlusion is a 0..1
     * coverage, not radiance, and pushing it through an exposure and a filmic
     * shoulder would show a washed-out version of the thing being inspected.
     *
     * 5 IS THE OCCLUSION PLANE, matching ViewSettings::debugView so one key
     * cycles the same views on both renderers. It is the only way to actually
     * see what SSAO is doing — in an ordinary frame it multiplies the ambient
     * term only, which at a 0.42 intensity is a subtle darkening in corners
     * rather than anything you can point at. */
    int debugView = int(uExposureAndFlags.z + 0.5);
    if (debugView == 5) {
        outColour = vec4(texture(uOcclusion, gl_FragCoord.xy * uOutputTexel.xy).rrr, 1.0);
        return;
    }

    /* THE SWITCH TRAVELS INTO THE RESOLVE rather than branching outside it, so
     * there is no second code path for a future feature to forget: raw is the
     * same blit without the curve. */
    vec3 display = uExposureAndFlags.y != 0.0
                 ? filmicDisplay(radiance, uExposureAndFlags.x)
                 : radiance * uExposureAndFlags.x;

    /* Opaque: this is the backbuffer, and the scene's alpha carries coverage
     * for other passes' benefit rather than anything the window wants. */
    outColour = vec4(display, 1.0);
}
