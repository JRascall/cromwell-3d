#version 330

/* msdf_text.fs.glsl — world-space text from a distance field.
 *
 * The decode is not here. It is in common/sdf.glsl, because a coastline and a
 * terrain boundary answer the same question about their own fields and there
 * should be one implementation of the median, the screen-space range and the
 * coverage. This file is only what is specific to TEXT.
 *
 * WHY TEXT NEEDS ITS OWN SHADER AT ALL, given the include does the work: the
 * outline. A label floating in a scene has no control over what is behind it,
 * so white text over a snowfield or a pale wall is unreadable, and every game
 * that puts names in the world solves it the same way — a dark rim around the
 * glyph. It costs one extra threshold of the SAME sampled distance, which is
 * the standing argument for storing a boundary as distance: the outline is
 * nearly free because the distance is already in hand.
 */

#include "common/sdf.glsl"

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;   /* the MSDF atlas; raylib's name, so rlgl binds it */

/* The range the ATLAS WAS BAKED WITH, handed over by MsdfFont::pxRange rather
 * than hardcoded. A shader disagreeing with its bake does not fail loudly — it
 * renders slightly crunchy or slightly soft, which reads as a tuning problem
 * and sends you to the wrong file. See cromwell/sdf/DistanceField.hpp. */
uniform float pxRange;

/* Outline colour, and its width IN SCREEN PIXELS. Pixels rather than em,
 * because the whole point is that this text has no fixed size: an outline
 * specified in em would vanish at distance and swamp the glyph up close, which
 * is precisely the local-space-spread mistake ui/shape/Shapes.hpp documents.
 * Width 0 disables it. */
uniform vec4  outlineColour;
uniform float outlineWidthPx;

out vec4 finalColor;

void main()
{
    /* The derivative work, done once. Both thresholds below share it — doing
     * it twice would be the expensive part of an otherwise trivial shader. */
    float range = sdfScreenPxRange(texture0, fragTexCoord, pxRange);

    /* Signed distance to the glyph edge in screen pixels, positive inside.
     * Everything else is two thresholds of this one number. */
    float edgePx = sdfDistancePixels(sdfMedian(texture(texture0, fragTexCoord).rgb),
                                     range);

    /* Coverage is the distance pushed through a one-pixel ramp centred on the
     * edge. The outline is the same ramp moved outward by its width, so it is
     * a true pixel width at every camera distance. */
    float fillAlpha = clamp(edgePx + 0.5, 0.0, 1.0) * fragColor.a;
    float rimAlpha  = clamp(edgePx + outlineWidthPx + 0.5, 0.0, 1.0)
                    * outlineColour.a * step(0.0001, outlineWidthPx);

    /* Fill OVER rim, composited in one pass. Two passes would double the
     * geometry and still have to order themselves; the alpha maths is the same
     * either way and this way the quad is drawn once.
     *
     * Standard source-over: the rim contributes only where the fill does not.
     * Straight (non-premultiplied) alpha out, matching everything else this
     * renderer blends. */
    float alpha = fillAlpha + rimAlpha * (1.0 - fillAlpha);
    if (alpha <= 0.0) discard;

    vec3 colour = (fragColor.rgb * fillAlpha
                 + outlineColour.rgb * rimAlpha * (1.0 - fillAlpha)) / alpha;

    finalColor = vec4(colour, alpha);
}
