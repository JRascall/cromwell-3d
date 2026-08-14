#version 450 core
/* ui_blur.fs.glsl — the backdrop, read back blurred.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ============ THE MIP LEVEL IS THE RADIUS, THE GATHER IS THE SHAPE ========
 *
 * The painter copies the screen behind the panel into a texture and generates
 * its mip chain; each level halves the resolution, so a radius of 2^n pixels is
 * level n. The level decides HOW WIDE the blur is. It does not, on its own,
 * decide what the blur LOOKS like, and that distinction is the whole content of
 * this file.
 *
 * A single textureLod at the chosen level — which is what this did first, and
 * what the raylib painter still does — is blocky, and visibly so. Two things
 * stack up:
 *
 *   1. THE CHAIN IS A BOX FILTER. glGenerateMipmap averages 2x2 blocks. A box
 *      filter has a hard edge, so a bright object leaves a square-ish smear
 *      rather than a soft falloff.
 *   2. MAGNIFYING IT BACK IS A TENT. Level 3 of a 1280-wide capture is 160
 *      texels across, stretched over 1280 pixels by the bilinear unit. Bilinear
 *      magnification is piecewise linear, and the human eye is extremely good at
 *      seeing the creases where the pieces meet — which is exactly the
 *      "stair-stepped" look. It is not aliasing and no amount of extra mip
 *      levels fixes it, because the data genuinely has only 160 samples.
 *
 * THE GATHER BELOW FIXES THE SECOND AND SOFTENS THE FIRST, for eight extra
 * texture reads and no extra targets or passes. It is the upsample kernel from
 * dual (Kawase) filtering: four taps on the axes at one texel of the sampled
 * level, four on the diagonals at half that, weighted 1 and 2. Overlapping tent
 * kernels sum to something very close to a Gaussian, which has no creases to
 * see.
 *
 * WHY NOT A REAL SEPARABLE GAUSSIAN. That is two more render targets and two
 * more draws PER PANEL, on a path that already splits the render pass once per
 * frosted region — see DeviceUiPainter. This buys most of the quality inside the
 * draw that was happening anyway. If a panel ever needs a genuinely wide,
 * genuinely Gaussian blur, the answer is the full dual-filter ladder (repeated
 * downsample then repeated upsample), and it wants its own targets.
 *
 * ============== textureLod, RATHER THAN PINNING THE SAMPLER ===============
 *
 * The raylib painter clamps the texture's LOD range around the level it wants,
 * draws, and puts the range back — because rlgl's shader has no way to be told
 * a level. Here the level is a push constant and the read states it directly.
 * That is not merely tidier: a sampler's LOD clamp is state on a shared object,
 * so two panels at different strengths in one frame would need two samplers or
 * two round trips through the driver, and forgetting to restore it leaves every
 * later read of that texture pinned to a blur.
 *
 * A FRACTIONAL LEVEL IS THE POINT. log2 of a pixel radius is rarely an integer,
 * and the sampler's trilinear filter interpolates between the two levels either
 * side — which is what makes the strength a continuous dial rather than a
 * stepped one through powers of two.
 */

layout(binding = 0) uniform sampler2D uCapture;

layout(location = 0) uniform vec4 uPushConstants[8];

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColour;

layout(location = 0) out vec4 outColour;

void main()
{
    float lod = uPushConstants[0].z;

    /* THE REGION'S CORNER OF A TEXTURE THAT ONLY GROWS. vTexCoord is 0..1 of the
     * frosted region; this scales it onto the sub-rectangle the copy actually
     * filled. It is a push constant rather than baked into the UVs because the
     * capture is sized to the LARGEST region in the frame, which is not known
     * while the earlier ones are being built. */
    vec2 uv = vTexCoord * uPushConstants[1].xy;

    /* ONE TEXEL OF THE LEVEL BEING READ, in UV. textureSize(…, 0) is the full
     * resolution, and exp2(lod) is how many of those one texel of this level
     * spans. Taken from the base level rather than from int(lod) so a fractional
     * level scales smoothly instead of jumping when it crosses an integer. */
    vec2 texel = exp2(lod) / vec2(textureSize(uCapture, 0));

    /* The dual-filter upsample kernel. Diagonals carry twice the weight of the
     * axes, and the total is twelve. */
    vec3 sum = textureLod(uCapture, uv + vec2(-texel.x,  0.0), lod).rgb;
    sum     += textureLod(uCapture, uv + vec2( texel.x,  0.0), lod).rgb;
    sum     += textureLod(uCapture, uv + vec2( 0.0, -texel.y), lod).rgb;
    sum     += textureLod(uCapture, uv + vec2( 0.0,  texel.y), lod).rgb;

    vec2 half_ = texel * 0.5;
    sum += textureLod(uCapture, uv + vec2(-half_.x, -half_.y), lod).rgb * 2.0;
    sum += textureLod(uCapture, uv + vec2( half_.x, -half_.y), lod).rgb * 2.0;
    sum += textureLod(uCapture, uv + vec2(-half_.x,  half_.y), lod).rgb * 2.0;
    sum += textureLod(uCapture, uv + vec2( half_.x,  half_.y), lod).rgb * 2.0;

    vec3 backdrop = sum / 12.0;

    /* TINTED, NOT REPLACED. White gives a plain frost; a caller wanting warm or
     * darkened glass gets it by multiplying, exactly as rhi/object.glsl tints
     * the world's own vertex colours.
     *
     * THE ALPHA IS THE TINT'S, NEVER THE CAPTURE'S. What sits in the
     * backbuffer's alpha channel is whatever the last pass happened to leave
     * there — it is not coverage and it means nothing here. Multiplying it in
     * makes a frosted panel's opacity depend on an unrelated pass, which is the
     * sort of bug that appears when some future effect starts writing alpha and
     * reads as "the frosting went transparent". */
    outColour = vec4(backdrop * vColour.rgb, vColour.a);
}
