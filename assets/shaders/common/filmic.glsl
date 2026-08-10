/* common/filmic.glsl - linear radiance to display colour.
 *
 * THE DISPLAY TRANSFORM, in one place. It was inline in tonemap.fs.glsl while
 * that was the only stage that resolved anything; the model preview is the
 * second, and two copies of a tone curve is exactly the sort of duplication
 * that ends with a UI thumbnail that does not match the object it depicts.
 *
 * The curve is Hable's Uncharted 2 filmic operator, which is the same preset
 * ("Hable (U2/HLVR)") Valve's newer Source 2 maps tonemap with. Its toe keeps
 * shadow separation instead of clipping it, and its shoulder rolls highlights
 * off rather than clamping them, which is what lets an overbright sun read as
 * bright instead of as a white hole.
 *
 * Nothing here samples a texture or reads a uniform, so it sits beside
 * common/colour.glsl at the bottom of the include tree.
 */
#ifndef XCOM_COMMON_FILMIC
#define XCOM_COMMON_FILMIC

vec3 hable(vec3 x)
{
    const float a = 0.15, b = 0.50, c = 0.10;
    const float d = 0.20, e = 0.02, f = 0.30;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

/* Exposure, the curve, and the sRGB encode - the whole trip from unbounded
 * radiance to a byte a monitor can show. RGB only: alpha means different
 * things to the two callers and neither wants it touched here. */
vec3 filmicDisplay(vec3 radiance, float exposure)
{
    /* Normalised by the curve's value at the white point, so a surface at
     * linear 11.2 maps to display white and nothing above it is lost. */
    const float kLinearWhite = 11.2;
    vec3 mapped = hable(radiance * exposure) / hable(vec3(kLinearWhite));

    return pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2));
}

#endif
