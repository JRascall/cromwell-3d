/* sdf.glsl — decoding a distance field, for every consumer of one.
 *
 * SINGLE RESPONSIBILITY: turn a texel sampled from a distance field into
 * coverage, correctly antialiased, at any magnification. Nothing here knows
 * what the field depicts.
 *
 * WHY THIS IS SHARED RATHER THAN LIVING IN THE TEXT SHADER. A distance field
 * is a general way to store a boundary, and text is only the first thing that
 * wants one. A coastline where land meets sea, a terrain material boundary, a
 * decal mask with a soft edge — all of them store "how far to the edge, and
 * which side am I on", and all of them decode it identically. What differs is
 * how the field was GENERATED (glyph outlines and a land mask have nothing in
 * common) and what the coverage is used FOR. The decode is the part that is
 * genuinely the same, so it is the part that is shared.
 *
 * THE ENCODING CONTRACT, which every producer must match and which
 * cromwell/sdf/DistanceField.hpp states once for the C++ side:
 *
 *   - The edge is at 0.5. Below is outside, above is inside. Not 0.0, because
 *     the field is stored in an unsigned 8-bit texture and half the range has
 *     to be spendable on each side.
 *   - `pxRange` is how many TEXELS of the field the 0..1 range spans. It is a
 *     property of the BAKE, not of the draw, so it travels with the atlas in
 *     its metrics file and is never typed in twice. 2 to 4 suits text; a field
 *     feeding an effect that samples far from the boundary — a glow, a coastal
 *     shallows ramp — wants more, because past pxRange the field clamps and
 *     carries no gradient at all.
 *   - Single channel: the distance is in .r. Three channel: see sdfMedian.
 *
 * ANTIALIASING IS SCREEN-SPACE, AND THAT IS THE WHOLE POINT. The softening
 * band is derived per fragment from how fast the field changes across the
 * screen, so it is one pixel wide whether the shape is eight pixels tall or
 * fills the display. A spread fixed in the shape's own space instead is
 * exactly the failure documented in ui/shape/Shapes.hpp — a thin feature and
 * a huge one get the same absolute softening, and the thin one turns to mush.
 * That is why the UI kit draws its rounded boxes as real geometry, and it is
 * also why anything here that skipped the derivative would repeat the mistake.
 */

#ifndef CW_SDF_GLSL
#define CW_SDF_GLSL

/* The median of three channels — the MULTI-channel decode.
 *
 * A single distance field stores one number per texel, so at a corner, where
 * two edges meet, the true field has a crease. Bilinear interpolation cannot
 * represent a crease and smooths it, which rounds every sharp corner off; on
 * text that shaves the points of E, T, L and 7 and reads as melted.
 *
 * An MSDF stores three distances, each carrying a different subset of the
 * shape's edges. Along a smooth edge all three agree and the median is simply
 * the distance. At a corner the median of three linear functions reproduces
 * the crease exactly, because that is what the intersection of two half-planes
 * is. Corners come back sharp for the cost of two more channels.
 *
 * Branchless, and the standard formulation rather than a clever one. */
float sdfMedian(vec3 channels)
{
    return max(min(channels.r, channels.g),
               min(max(channels.r, channels.g), channels.b));
}

/* How many SCREEN pixels the field's 0..1 range covers at this fragment.
 *
 * This is the conversion that makes the whole technique resolution
 * independent, so it is worth reading rather than trusting:
 *
 *   unitRange     — pxRange expressed in UV, i.e. what fraction of the texture
 *                   the meaningful band occupies.
 *   screenTexSize — the inverse of the UV derivative, i.e. how many screen
 *                   pixels one full unit of UV is stretched across right here.
 *
 * Their dot product is that band measured in screen pixels. Text far away
 * shrinks it, text up close grows it, and the coverage below divides by it, so
 * the soft edge stays one pixel at every distance.
 *
 * DERIVED FROM fwidth(uv), NOT fwidth(distance). The obvious shortcut is to
 * take the derivative of the decoded value directly and skip pxRange entirely.
 * It very nearly works, and it fails in two places that matter: the median
 * jumps where which-channel-is-median changes, putting a bright speck at some
 * corners, and in the clamped region beyond pxRange the field is flat, so the
 * derivative is zero and the divide explodes. The UV derivative is well
 * behaved everywhere because UVs are linear across a triangle.
 *
 * Floored at 1, so a field minified past the point of carrying detail fades
 * rather than aliasing into noise. */
float sdfScreenPxRange(sampler2D field, vec2 uv, float pxRange)
{
    vec2 unitRange = vec2(pxRange) / vec2(textureSize(field, 0));
    vec2 screenTexSize = vec2(1.0) / max(fwidth(uv), vec2(1e-8));
    return max(0.5 * dot(unitRange, screenTexSize), 1.0);
}

/* Coverage from a decoded field value. `screenPxRange` comes from the function
 * above; it is passed in rather than recomputed so a shader sampling the same
 * field several times — an outline pass and a fill pass — pays for it once. */
float sdfCoverage(float distance, float screenPxRange)
{
    return clamp(screenPxRange * (distance - 0.5) + 0.5, 0.0, 1.0);
}

/* Signed distance in SCREEN PIXELS, positive inside.
 *
 * For effects that want the distance itself rather than a hard edge: an
 * outline n pixels thick, a glow falling off over n, a coastline whose
 * shallows ramp inward. Separate from sdfCoverage because a caller wanting a
 * band at distance n should not have to un-threshold a coverage value to
 * recover it. */
float sdfDistancePixels(float distance, float screenPxRange)
{
    return screenPxRange * (distance - 0.5);
}

/* ---- the two common cases, in one call each ---------------------------- */

/* Multi-channel: text, and anything else baked from vector outlines where
 * corners have to survive. */
float sdfCoverageMsdf(sampler2D field, vec2 uv, float pxRange)
{
    float range = sdfScreenPxRange(field, uv, pxRange);
    return sdfCoverage(sdfMedian(texture(field, uv).rgb), range);
}

/* Single-channel: fields generated from a raster mask — a coastline, a terrain
 * boundary — where there are no corners worth preserving and one channel is a
 * third of the memory. */
float sdfCoverageSingle(sampler2D field, vec2 uv, float pxRange)
{
    float range = sdfScreenPxRange(field, uv, pxRange);
    return sdfCoverage(texture(field, uv).r, range);
}

#endif  /* CW_SDF_GLSL */
