/* common/colour.glsl - constants, colour space, and cheap noise.
 *
 * The bottom of the include tree: everything else pulls this in and it pulls
 * in nothing. Nothing here samples a texture or reads a uniform, so it is safe
 * for any stage of any shader family.
 */
#ifndef XCOM_COMMON_COLOUR
#define XCOM_COMMON_COLOUR

const float kPi = 3.14159265359;

/* THE ONLY PLACE sRGB IS DECODED. Albedo is authored in sRGB; normal maps,
 * MRAO and every mask are DATA and must be sampled raw. Decoding one of those
 * by accident is the classic way to get subtly wrong lighting that nobody can
 * find, so the conversion lives in one named function rather than as an
 * inline pow() somebody can copy into the wrong line. */
vec3 srgbToLinear(vec3 c) { return pow(c, vec3(2.2)); }

/* Per-pixel hash, for rotating sampling discs. Cheap enough to avoid carrying
 * a blue-noise texture around, and keyed to position only - keying anything
 * like this to time makes the result crawl between frames, which is worse than
 * the artifact it hides. */
float hash12(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

#endif
