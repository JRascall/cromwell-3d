#version 450 core
/* bloom_prefilter.fs.glsl — the scene's bright half, at half resolution.
 *
 * See assets/shaders/CONVENTIONS.md. First of the three bloom stages; the
 * chain and the reasoning are in ScenePipeline::drawBloom.
 *
 * ================== WHY A THRESHOLD AT ALL, AND WHY A SOFT ONE ============
 *
 * Physically there is no threshold: a real lens scatters light from EVERY
 * source, and the bright ones simply scatter more. A pass that blurred the
 * whole frame and added a few percent back would be the honest model, and it
 * is what film-grade bloom does.
 *
 * We threshold anyway, for a reason that is about authoring rather than about
 * optics: without one, raising the bloom to where an emissive strip reads as a
 * light also puts a haze over every lit wall in the scene, and the two cannot
 * be tuned apart. The threshold is the knob that separates "how much do
 * LIGHTS glow" from "how milky is the image".
 *
 * A HARD CUT IS THE VERSION THAT LOOKS BROKEN. A surface drifting across the
 * threshold — a wall panning through a highlight, a unit walking under a lamp —
 * pops into glowing all at once, and in motion that reads as flickering rather
 * than as a threshold. The soft knee is a quadratic that ramps contribution in
 * over a band below the cut, so the transition is continuous. This is Jimenez's
 * curve from the Call of Duty presentation and Unity's default.
 *
 * ============ AND THE KARIS AVERAGE, WHICH IS NOT OPTIONAL ================
 *
 * A single very bright pixel — a specular glint, a probe hitting the sun —
 * survives every downsample as a shrinking dot and then blows back up through
 * the upsample as a square-ish blob that FLICKERS as the camera moves, because
 * which texel it lands in changes. That is the classic bloom firefly.
 *
 * Weighting each tap by 1/(1+luma) before averaging bounds any single tap's
 * contribution, which removes the flicker at the cost of slightly dimming
 * genuinely large bright areas. Karis' trick, and it belongs in the FIRST
 * downsample only — applying it further down the chain would dim the bloom
 * itself rather than the outliers.
 */
layout(binding = 0) uniform sampler2D uSource;

layout(std140, binding = 1) uniform BloomBlock {
    /* x = threshold, y = knee, z = intensity, w = filter radius */
    vec4 uBloom;

    /* xy = one texel of the SOURCE in UV, zw = the source's size in pixels */
    vec4 uSourceTexel;

    /* xy = the TARGET's size in pixels, which is how gl_FragCoord becomes a UV
     * without a vertex stage that carries one. zw spare. */
    vec4 uTargetSize;
};

layout(location = 0) out vec4 outColour;

/* THE SOFT KNEE, as a scale factor on the colour rather than a clamp on it.
 *
 * Returning a WEIGHT and multiplying, instead of subtracting the threshold,
 * keeps hue: subtracting a scalar from an unbalanced colour shifts it toward
 * whichever channel had the most headroom, so a warm lamp just above the cut
 * comes out noticeably more orange than the lamp itself. */
float kneeWeight(vec3 colour)
{
    float brightness = max(colour.r, max(colour.g, colour.b));

    float threshold = max(uBloom.x, 0.0);
    float knee      = max(uBloom.y, 0.0001);

    /* The quadratic ramp across the knee band, then the linear region above it.
     * `soft` is zero at threshold-knee and equals (brightness-threshold) at
     * threshold+knee, so the two pieces meet with matching value and slope —
     * which is the whole point of the curve and what a max() of the two alone
     * would not give. */
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);

    float contribution = max(soft, brightness - threshold);

    /* Divided by brightness to become a scale rather than an amount, guarded
     * for a black pixel where there is nothing to scale. */
    return contribution / max(brightness, 0.0001);
}

/* One bilinear tap, weighted so no single bright texel can dominate. */
vec4 karisTap(vec2 uv)
{
    vec3 colour = texture(uSource, uv).rgb;

    /* RELATIVE LUMINANCE, not the max channel: this is a perceptual weight, and
     * a saturated blue at max-channel 1.0 is far dimmer than a white at the
     * same figure. Rec. 709 coefficients, spelt out here rather than added to
     * common/colour.glsl — that header is shared with the raylib renderer and
     * is for things both paths use, which this is not. */
    float weight = 1.0 / (1.0 + dot(colour, vec3(0.2126, 0.7152, 0.0722)));
    return vec4(colour * weight, weight);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / max(uTargetSize.xy, vec2(1.0));

    /* FOUR TAPS AT THE CORNERS OF THE SOURCE'S 2x2 BLOCK. Each bilinear fetch
     * sits exactly between four source texels, so four fetches average sixteen
     * — a 4x4 box for the price of four reads, which is what makes a 2x
     * downsample this cheap and this stable. */
    vec2 offset = uSourceTexel.xy;

    vec4 sum = karisTap(uv + offset * vec2(-1.0, -1.0))
             + karisTap(uv + offset * vec2( 1.0, -1.0))
             + karisTap(uv + offset * vec2(-1.0,  1.0))
             + karisTap(uv + offset * vec2( 1.0,  1.0));

    /* Renormalised by the weights actually used, so a uniformly bright region
     * keeps its brightness and only OUTLIERS are pulled down. Dividing by four
     * instead would dim the whole image by however much the weights summed to. */
    vec3 colour = sum.rgb / max(sum.a, 0.0001);

    /* THE THRESHOLD LAST, on the averaged value. Applying it per tap inside
     * karisTap would threshold sixteen source texels independently and let a
     * single one above the cut carry the block, which is the firefly the Karis
     * weighting was there to prevent — the two have to be in this order. */
    outColour = vec4(colour * kneeWeight(colour), 1.0);
}
