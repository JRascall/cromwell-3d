#version 450 core
/* outline.fs.glsl — a silhouette round whatever the custom stencil tagged.
 *
 * See assets/shaders/CONVENTIONS.md. THE FIRST CONSUMER THE CUSTOM DEPTH BUFFER
 * HAS EVER HAD, on either renderer. CustomDepthStencil.hpp has said since it was
 * written that "a silhouette is then one full-screen shader: read the value, ask
 * whether this depth is behind the G-buffer's, draw accordingly" — and until now
 * the buffer's only consumers were two rows in the texture inspector.
 *
 * ==================== WHY IT RUNS AFTER THE TONE MAP =====================
 *
 * AND IT IS THE OPPOSITE OF THE DECISION BLOOM MADE, deliberately.
 *
 * Bloom composites into the HDR scene target BEFORE the resolve, because it is
 * LIGHT: it has to be exposed with the frame and tone-mapped with it, or it is
 * not light at all. An outline is not light. It is INTERFACE — the same
 * category as the HUD and the movement rings — and a designer picking a colour
 * for it means THAT colour, not whatever the exposure curve makes of it. A
 * selection ring that changed shade when the sun went down would be read as a
 * bug in the interface, and correctly.
 *
 * So this is display-colour ink over a finished picture, which is exactly what
 * MIGRATION.md §4.6 criticises GlowPass for being — and the criticism does not
 * transfer, because GlowPass is a GLOW. The test is not "before or after the
 * tone map", it is "is this radiance, or is this a widget".
 *
 * ======================= THE EDGE, AND WHY IT IS THE ID ==================
 *
 * Edge-detected on the ID rather than on depth. A depth edge finds every
 * silhouette in the scene and needs a threshold that is wrong at some distance;
 * an id edge is exact, needs no threshold, and tells apart two soldiers
 * standing against one another — which a depth edge cannot do at all, and which
 * is the ordinary case in a tactics game.
 *
 * ============== HOW A ONE-BIT TEST PRODUCES A SMOOTH EDGE ================
 *
 * THE TEST BELOW IS STILL HARD YES-OR-NO. It is not softened, thresholded or
 * filtered, and it must not be: the stencil channel holds an ID, and any filter
 * across two neighbouring ids returns a number that is NEITHER — a third
 * soldier who does not exist. Every tap here is a texelFetch for that reason,
 * which is point sampling by definition and cannot be undone by a sampler bound
 * at the wrong slot.
 *
 * SO THE COVERAGE COMES FROM COUNTING, NOT FROM FILTERING. The custom stencil is
 * supersampled with the rest of the scene, so each output pixel is backed by
 * uSampling.x squared texels. The whole edge test runs once per texel and the
 * results are averaged. Four texels give five levels of alpha, and five levels
 * is the difference between a staircase and an edge.
 *
 * ======= WHY THE BUFFER IS FINER THAN THE SCENE, AND THE GRID ROTATED ======
 *
 * FOUR TEXELS IS FIVE LEVELS ONLY ON A DIAGONAL. A 2x2 block is an ORDERED grid:
 * its four texels sit at just TWO distinct x positions and two distinct y. A
 * near-vertical edge is decided by x alone, so it can read 0, one half, or one —
 * three levels — and three levels on high-contrast ink is a visible staircase.
 * Near-horizontal edges have the same problem in y. Only a diagonal, using both
 * axes, sees all five. In a building made of boxes, almost nothing is diagonal.
 *
 * NO FILTER FIXES THIS, and the attempt is instructive. Averaging a WIDER
 * footprint — a 4x4 block of the same buffer — is a two-pixel box filter over a
 * two-pixel line, which turns its near-rectangular profile into a four-pixel
 * triangle. A glow, not an outline. The missing information is sample POSITIONS,
 * and no amount of smoothing invents them.
 *
 * SO THE BUFFER IS RASTERISED FINER THAN THE SCENE, on its own dial — see
 * ScenePipeline::withOutlineSupersample. At 4x there are four distinct positions
 * per axis, and four is the point at which a ROTATED grid becomes possible:
 * one tap per column, each in a different row, giving as many distinct x as y.
 * That is precisely what MSAA's sample positions buy and what an ordered grid
 * throws away, and it is why hardware picked rotated patterns decades ago.
 *
 * THE SAMPLE COUNT DOES NOT FOLLOW THE BUFFER. Reading all sixteen texels of a
 * 4x4 block costs four times as much and still yields five levels, because the
 * extra taps land in columns already counted. `samples` taps in a rotated
 * permutation give `samples + 1` levels for the price of the taps alone — so
 * raising the dial buys positions, not work. Memory is what it spends, which is
 * the trade the dial exists to expose.
 *
 * THIS IS UNREAL'S EDITOR OUTLINE, with supersampling where they use MSAA. They
 * allocate a private 4x multisampled target for editor primitives in a deferred
 * renderer that has MSAA nowhere else, then run exactly this loop —
 * `Sum / MSAA_SAMPLE_COUNT` in PostProcessSelectionOutline.usf. The two are the
 * same idea; ours needs no Texture2DMS and no per-sample positions because a
 * supersample's "samples" are ordinary fully-shaded texels on an ordinary grid.
 * Their game-side custom-depth outline, which is architecturally what this pass
 * is, gets neither treatment and aliases — see
 * study/topics/rendering/outline_antialiasing.md.
 *
 * AND IT IS WHY THE DEPTH BIAS IS NEARLY GONE. It used to be 5e-4, sized to hide
 * a quarter-pixel offset between a 1x custom depth and a 2x scene depth. Both
 * buffers are now the same grid, so the two fetches below are the same texel and
 * the tagged object's depth is EQUAL in them, not merely close. What is left is
 * ULP insurance against a driver that compiles two vertex programs differently;
 * the pipeline already bets the whole image on that not happening, since the lit
 * pass tests CompareFunc::Equal against the prepass depth.
 */
layout(binding = 0) uniform sampler2D uCustomStencil;   /* r = id, a = coverage */
layout(binding = 1) uniform sampler2D uCustomDepth;
layout(binding = 2) uniform sampler2D uSceneDepth;

layout(std140, binding = 1) uniform OutlineBlock {
    /* x = the id to outline, 0-255; 0 means nothing is selected.
     * y = thickness in OUTPUT pixels — what a designer means by "two wide".
     * zw = the STENCIL's size in texels, which is the supersampled size and NOT
     *      the size of the target being drawn into. */
    vec4 uOutline;

    /* x = stencil texels per output pixel on each axis — the outline's own
     *     supersample, which chooses the sample pattern below.
     * y = stencil texels per SCENE-DEPTH texel on each axis; 1 when the two
     *     factors match. See depthAt() for why this exists.
     * zw = the SCENE DEPTH's size in texels. */
    vec4 uSampling;

    /* The colour where the object is visible. */
    vec4 uVisibleColour;

    /* And where it is hidden behind something. A DIFFERENT COLOUR RATHER THAN
     * NOTHING: knowing that the selected soldier is behind that wall is most of
     * what a player wants from a selection outline, and it is what an x-ray
     * silhouette IS. Alpha zero switches it off without a second uniform. */
    vec4 uOccludedColour;
};

layout(location = 0) out vec4 outColour;

/* The last addressable texel. texelFetch outside the texture is UNDEFINED in
 * GL — not clamped, not zero — so every fetch below is clamped into range. This
 * is what makes an outline touching the edge of the screen stop there rather
 * than read rubbish. */
ivec2 lastTexel()
{
    return ivec2(max(uOutline.zw, vec2(1.0))) - ivec2(1);
}

/* Whether this texel holds the id being outlined. COVERAGE IS TESTED FIRST,
 * because a cleared texel reads id 0 and 0 is a legal id. */
bool tagged(ivec2 texel, float wanted)
{
    vec4 value = texelFetch(uCustomStencil, clamp(texel, ivec2(0), lastTexel()), 0);
    if (value.a < 0.5) return false;
    return abs(value.r * 255.0 - wanted) < 0.5;
}

/* ---- the occlusion test, DELIBERATELY DONE ON THE SCENE'S GRID -----------
 *
 * THE ONE PLACE A QUALITY DIAL COULD REINTRODUCE THE OLD BUG, so it is the one
 * place that refuses to work at the stencil's resolution.
 *
 * The custom stencil may be rasterised finer than sceneDepth_ — that is the
 * entire point of the dial — but sceneDepth_ is fixed at the scene's factor.
 * Fetching the two at the same position would therefore read points up to half
 * a scene texel apart, which is exactly the offset that made the old kBias
 * necessary and exactly what flips an outline between its two colours as the
 * camera moves. Finer coverage must not be bought with a fuzzier depth test.
 *
 * SO THE COMPARISON DROPS TO THE COARSER GRID, where the two agree exactly. The
 * stencil is an integer multiple of the scene depth (guaranteed by
 * withOutlineSupersample flooring the factor), so a stencil texel lies WHOLLY
 * inside one scene texel and the mapping is an integer divide — nothing that can
 * round differently from one frame to the next.
 *
 * AND THE OBJECT'S DEPTH IS REDUCED OVER THAT FOOTPRINT rather than point
 * sampled, so both sides of the comparison describe the same area of screen. The
 * reduction is a MIN — the object's nearest surface there — because that is what
 * "is any of this object in front" means. Four corners suffice: over a planar
 * surface the extremes of a square are at its corners, and a depth buffer is
 * planes almost everywhere.
 *
 * WHERE IT IS AMBIGUOUS IT ERRS TOWARDS VISIBLE. A silhouette crossing the
 * footprint gives a min nearer than the scene's sample, so the outline draws in
 * its main colour rather than its x-ray one. A wrong colour in the safe
 * direction, and — being derived from an integer mapping — a STABLE one, which
 * is the property that actually matters. A flicker is far worse than a bias.
 *
 * AT PARITY THIS IS A NO-OP: the block is one texel, the min is that texel, and
 * the arithmetic reduces to the exact comparison it replaced. */
vec4 occlusionAt(ivec2 texel)
{
    int   block    = int(max(uSampling.y, 1.0));
    ivec2 sceneMax = ivec2(max(uSampling.zw, vec2(1.0))) - ivec2(1);

    ivec2 sceneTexel = texel / block;
    float sceneDepth = texelFetch(uSceneDepth, clamp(sceneTexel, ivec2(0), sceneMax), 0).r;

    /* The corners of the custom-depth block covering that scene texel. */
    ivec2 origin = sceneTexel * block;
    int   far    = block - 1;

    float objectDepth = texelFetch(uCustomDepth, clamp(origin, ivec2(0), lastTexel()), 0).r;
    objectDepth = min(objectDepth,
        texelFetch(uCustomDepth, clamp(origin + ivec2(far, 0), ivec2(0), lastTexel()), 0).r);
    objectDepth = min(objectDepth,
        texelFetch(uCustomDepth, clamp(origin + ivec2(0, far), ivec2(0), lastTexel()), 0).r);
    objectDepth = min(objectDepth,
        texelFetch(uCustomDepth, clamp(origin + ivec2(far, far), ivec2(0), lastTexel()), 0).r);

    /* ULP INSURANCE, NOT A SPATIAL CORRECTION. It was 5e-4 when the two buffers
     * disagreed by a quarter of a pixel; they now describe the same footprint,
     * so all that is left is the chance of a driver compiling two vertex
     * programs differently. The pipeline already bets the whole image on that
     * not happening — the lit pass tests CompareFunc::Equal against the prepass
     * depth — so this is belt over braces. */
    const float kBias = 1.0e-6;
    bool occluded = objectDepth > sceneDepth + kBias;

    vec4 colour = occluded ? uOccludedColour : uVisibleColour;
    return vec4(colour.rgb * colour.a, colour.a);
}

/* One texel's worth of the answer, PREMULTIPLIED.
 *
 * Premultiplied because the caller averages several of these and the visible and
 * occluded colours carry different alphas — averaging straight alpha would drag
 * the occluded colour's 0.75 into the visible one's RGB and tint the edge where
 * a silhouette crosses in front of a wall. Premultiplied is the space where a
 * weighted sum of colours means what it says; main() converts back once.
 *
 * A fragment that draws nothing returns zero, which is the identity for that
 * sum — the same reason nothing here early-returns opaque black. */
vec4 outlineAt(ivec2 texel, float wanted, int reach)
{
    /* INSIDE THE SHAPE DRAWS NOTHING. An outline is the boundary — filling the
     * interior would be a highlight, which is a different effect and one that
     * obscures the soldier it is meant to point at. */
    if (tagged(texel, wanted)) return vec4(0.0);

    /* ---- is any neighbour tagged? ---------------------------------------
     *
     * AN OCTAGON, NOT A CROSS, AND THE CROSS WAS A HOLE AT EVERY CORNER.
     *
     * This used to take the four cardinal taps only, on the reasoning that
     * diagonals "only matter at a corner sharper than the thickness, and a body
     * is a box". A box is nothing BUT corners, and a corner is precisely where
     * the cardinal cross has nothing to find: from a texel diagonally outside a
     * convex corner, the left and right taps are still above the object and the
     * up and down taps are still beside it. Nothing hits, so a square of side
     * `reach` at every convex corner could never draw. The outline arrived in
     * pieces that did not meet.
     *
     * WHY IT WENT UNNOTICED FOR SO LONG: at one sample per pixel the whole
     * silhouette was ragged, and a two-pixel notch read as more of the same.
     * Antialiasing everything around it is what made the hole legible.
     *
     * THE DIAGONALS REACH SHORTER, BY EXACTLY ROOT TWO. A tap is a translation,
     * so the band this dilation produces extends by (offset · normal) — which
     * for a diagonal offset at full `reach` would be 1.41x too far, and Epic's
     * own note that "diagonal cross is thicker than vertical/horizontal cross"
     * is that same factor seen from the other side. Pulling them in to
     * reach/sqrt(2) makes the extent equal at 0, 45 and 90 degrees: an octagon
     * inscribing the circle we actually want, within about 4% everywhere. The
     * corner comes out ROUNDED at radius `reach`, which is what a band of
     * uniform width around a shape genuinely looks like.
     *
     * CARDINALS ARE TESTED FIRST because a straight edge is the common case and
     * the chain short-circuits; the corners pay for the extra four taps and
     * nothing else does. */
    int diagonal = int(float(reach) * 0.70710678 + 0.5);

    ivec2 inward;
    if      (tagged(texel + ivec2( reach, 0), wanted)) inward = ivec2( reach, 0);
    else if (tagged(texel + ivec2(-reach, 0), wanted)) inward = ivec2(-reach, 0);
    else if (tagged(texel + ivec2(0,  reach), wanted)) inward = ivec2(0,  reach);
    else if (tagged(texel + ivec2(0, -reach), wanted)) inward = ivec2(0, -reach);
    else if (tagged(texel + ivec2( diagonal,  diagonal), wanted)) inward = ivec2( diagonal,  diagonal);
    else if (tagged(texel + ivec2(-diagonal, -diagonal), wanted)) inward = ivec2(-diagonal, -diagonal);
    else if (tagged(texel + ivec2( diagonal, -diagonal), wanted)) inward = ivec2( diagonal, -diagonal);
    else if (tagged(texel + ivec2(-diagonal,  diagonal), wanted)) inward = ivec2(-diagonal,  diagonal);
    else return vec4(0.0);

    /* ---- visible, or behind something? ----------------------------------
     *
     * SAMPLED AT THE NEAREST TAGGED NEIGHBOUR, NOT AT THIS TEXEL. This texel is
     * OUTSIDE the object, so its custom depth is the clear value and the
     * comparison would always answer "behind" — the outline would be uniformly
     * occluded-coloured and the whole distinction would be lost. Stepping in by
     * `inward` is what makes the test mean anything, and it is already the
     * direction the edge was found in. */
    return occlusionAt(clamp(texel + inward, ivec2(0), lastTexel()));
}

void main()
{
    float wanted = uOutline.x;

    /* NOTHING SELECTED IS NOT AN EARLY RETURN OF BLACK. This pass BLENDS, so
     * its identity is a fully transparent fragment; anything opaque here would
     * paint the whole screen. */
    if (wanted < 0.5) { outColour = vec4(0.0); return; }

    /* THE BLOCK OF STENCIL TEXELS BEHIND THIS OUTPUT PIXEL. gl_FragCoord is in
     * output pixels because that is what this pass rasterises into; the stencil
     * is `samples` times finer on each axis, so pixel (x,y) owns the square of
     * texels starting at (x,y) * samples. */
    int samples = int(max(uSampling.x, 1.0));
    ivec2 base  = ivec2(gl_FragCoord.xy) * samples;

    /* THICKNESS ARRIVES IN OUTPUT PIXELS AND IS SPENT IN STENCIL TEXELS. Scaling
     * it here is what keeps the outline the same width on screen whatever the
     * supersample factor is — and a thickness in UV, which is what this used to
     * be, would have been thinner on a large monitor. */
    int reach = int(max(uOutline.y, 1.0) * float(samples) + 0.5);

    /* ---- the sample pattern ----------------------------------------------
     *
     * ONE PER ROW AND ONE PER COLUMN — a rotated grid, and the reason the buffer
     * was made finer in the first place.
     *
     * The failure a 2x2 block has is not that four samples is too few, it is
     * that they sit at only TWO distinct x positions and two distinct y. A
     * near-vertical edge is decided by x alone, so it can read three levels and
     * no more, however many samples are averaged. Sampling all sixteen texels of
     * a 4x4 block would cost four times as much and STILL give five levels,
     * because the extra samples land in columns already counted.
     *
     * So the pattern takes exactly one texel from each column, at a different
     * row each time: `samples` taps spanning `samples` distinct x AND `samples`
     * distinct y, which is `samples + 1` coverage levels on near-vertical and
     * near-horizontal edges alike. This is what MSAA's rotated sample positions
     * buy and what an ordered grid throws away — see the header.
     *
     * THE STRIDE MUST BE COPRIME WITH THE WIDTH or the walk revisits a column
     * before covering them all. Half the width plus one is coprime for every
     * power of two above two, which is the whole range this dial allows.
     *
     * AT PARITY THERE IS NOTHING TO ROTATE — two columns is two columns — so the
     * plain block is sampled instead, exactly as before. That setting is the
     * cheap one and is expected to look like the cheap one. */
    vec4 sum   = vec4(0.0);
    int  taken = 0;

    if (samples <= 2) {
        for (int y = 0; y < samples; ++y)
            for (int x = 0; x < samples; ++x) {
                sum += outlineAt(base + ivec2(x, y), wanted, reach);
                taken++;
            }
    } else {
        int stride = samples / 2 + 1;
        for (int i = 0; i < samples; ++i) {
            ivec2 at = ivec2((i * stride) % samples, i);
            sum += outlineAt(base + at, wanted, reach);
            taken++;
        }
    }

    sum /= float(taken);

    /* BACK TO STRAIGHT ALPHA, because the pipeline blends SrcAlpha /
     * OneMinusSrcAlpha. The guard is not defensive dressing: alpha is zero over
     * almost the entire screen and dividing there would produce NaNs that the
     * blend would happily smear across the frame. */
    outColour = sum.a > 0.0 ? vec4(sum.rgb / sum.a, sum.a) : vec4(0.0);
}
