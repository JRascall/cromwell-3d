#version 330
/* ribbon.fs.glsl - movement-coverage ribbon, pixel stage.
 *
 * Port of UI_3D.Tile.MovementBorder, recovered node-for-node from the WotC SDK.
 * The material and every constant in it are written out in readable form in
 * ../../study/games/strategy/xcom2_movement_border.hlsl - read that first; this file is the
 * same maths with XCOM's z-up unreal units swapped for our y-up tiles.
 *
 * The material is MLM_Unlit: the emissive IS the output, and all the behaviour
 * lives in the opacity. It is also bDisableDepthTest, so the DestDepth compare
 * below is not an embellishment on top of the depth test - it IS the depth
 * test, made soft on purpose.
 */
in vec2  vUV;
in float vWorldY;

out vec4 finalColor;

uniform sampler2D depthTex;      /* the scene drawn once without ribbons */
uniform vec2      uResolution;
uniform float     uNear;
uniform float     uFar;

uniform float uTime;
uniform vec4  uColor;            /* rgb = VectorParameter "Color", a = master  */
uniform float uEmissive;         /* 1.0 in the lit pass, >1 in the glow pass   */
uniform float uBorderRelevance;  /* ScalarParameter, 0 = scrolling, 1 = static */
uniform float uHideHeight;       /* ScalarParameter "HideHeight"               */
uniform float uHideFade;         /* Constant_39: 48uu                          */
uniform float uDepthRate;        /* Constant_41: 0.05 / uu                     */
uniform float uDepthFloor;       /* ConstantClamp_11.Min: 0.5                  */
uniform float uPanSpeed;         /* Panner_0.SpeedY: 0.5                       */

/* Eye-space distance from a depth-buffer sample, so the recovered rates - which
 * are per unreal unit of real distance - stay meaningful. */
float linearDist(float d)
{
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

/* How many sub-positions the profile is evaluated at across one pixel. Cheap:
 * the ribbon is a couple of percent of the frame and each tap is three
 * smoothsteps, no texture fetches. */
const int AA_TAPS = 8;

/* Stand-in for MovementBorder_Line, the 64x64 BC5 two-channel profile texture:
 * R is the cross-section that scrolls, G the one that stands still, both
 * clamped across the width and tiled once per 96uu along the length.
 *
 * Reconstructed rather than extracted - the game's texture is not ours to ship,
 * and a 64-texel profile is exactly the thing that is better off analytic. The
 * shape is the point: a flat core out to just under half the width, then a
 * shoulder that reaches zero at the rim so the strip has no hard edge to alias.
 * Returns (mask, core); core drives nothing but the glow pass's hot centre.
 *
 * ANTIALIASING. The shoulder being soft in UV is not the same as it being soft
 * in PIXELS, and at a normal tactical camera it is nowhere near: the quad is
 * MovementBorderWidth = 5uu across, which lands under two pixels wide, so the
 * whole 0.24 -> 0.86 shoulder is a fraction of one pixel and a single evaluation
 * is a point sample of a near-binary function. That is the staircase. The lit
 * pass half-hides it behind the backbuffer's MSAA; the glow pass has no MSAA
 * behind it and shows it plainly.
 *
 * So the authored profile is SUPERSAMPLED across its own screen-space footprint
 * rather than widened, clamped or blurred. fwidth gives that footprint in the
 * same units the profile is written in, and averaging over it is exactly the box
 * filter the rasteriser skipped. XCOM's section is then untouched wherever there
 * are pixels enough to draw it, and it degrades by dimming rather than by
 * dropping samples wherever there are not. The one place that honesty is the
 * wrong answer is a sub-pixel UI line, which is what the minimum-width term at
 * the bottom of the function exists to put back. */
vec2 lineProfile(vec2 uv)
{
    /* The quad is MovementBorderWidth wide - 5uu, straight out of the config -
     * but the quad is the CANVAS, not the line. The texture decides how much of
     * it is inked, and it is not all of it: a plateau across the middle quarter,
     * then a shoulder that reaches zero before the rim. Widening these two
     * numbers fattens the ribbon without touching its geometry. */
    float e   = uv.x * 2.0 - 1.0;                 /* -1..1 across the quad     */
    float ew  = clamp(fwidth(e),    1e-5, 4.0);   /* clamp: silhouettes spike  */
    float vw  = clamp(fwidth(uv.y), 0.0,  1.0);
    float pan = uTime * uPanSpeed;

    float body = 0.0, core = 0.0, dash = 0.0;
    for (int i = 0; i < AA_TAPS; i++) {
        float j = (float(i) + 0.5) / float(AA_TAPS) - 0.5;   /* -0.5 .. +0.5 */

        /* offset BEFORE the fold to abs(): fold first and the taps either side
         * of the centreline collapse onto each other, and the core - the one
         * part of the section that lives at e = 0 - never resolves at all */
        float a = abs(e + j * ew);
        body += 1.0 - smoothstep(0.24, 0.86, a);
        core += 1.0 - smoothstep(0.0,  0.34, a);

        /* R channel: the same section, gated into dashes along the run. V is in
         * tiles already (UVTilingDistance = 96uu = one tile), so this is one
         * dash per tile, sliding at Panner_0's 0.5 UV/s.
         *
         * The footprint here is fwidth(uv.y) and never fwidth of the fract: the
         * wrap makes that derivative explode once per tile, which would punch a
         * hole through the head of every dash. */
        float f = fract(uv.y + j * vw + pan);
        dash += smoothstep(0.03, 0.16, f) * (1.0 - smoothstep(0.62, 0.78, f));
    }
    /* MINIMUM SCREEN WIDTH. The supersample above is exact, and for a UI element
     * exact is not the same as right: at a normal camera the section really is
     * thinner than a pixel, so an honest box filter renders it correspondingly
     * dim - and dimmer again every time the camera pulls back. (Measurably: the
     * point-sampled version was ~1.5x brighter here, and every bit of that was
     * aliasing energy, ink laid down at full strength on whichever pixels the
     * line happened to land on.) A movement border has to stay legible at every
     * zoom, so once the ink no longer fills its pixel the section is normalised
     * back up to the strength a one-pixel-wide line would have. It keeps the
     * antialiased SHAPE and gets its READ back, and the scale is a smooth
     * function of the derivative, so it introduces no aliasing of its own.
     *
     * 1.10 is the body profile's own integral across the full section,
     * 2*(0.24 + (0.86-0.24)/2). The clamp floors the boost at 4x so a ribbon a
     * whole screen away fades out rather than blooming into a bright smear. */
    float inv = 1.0 / float(AA_TAPS);
    body *= inv / clamp(1.10 / ew, 0.25, 1.0);

    /* Neither of the other two gets a minimum, and for the same reason in both
     * cases: they are SHAPE, not strength, so where they stop being resolvable
     * the honest box average is also the right answer.
     *   core  selects the glow's hot centre. Normalise it and every ribbon past
     *         arm's length reports as all-centre and glows at full emissive.
     *   dash  a dash train too fine to resolve should melt into a dimmer
     *         continuous line, which is what it now does - not be boosted back
     *         into the crawling stipple it was. */
    core *= inv;
    dash *= inv;

    float scrolling = body * dash;
    float standing  = body;
    return vec2(mix(scrolling, standing, uBorderRelevance), core);
}

/* Subtract_14 is PixelDepth - DestDepth: positive means this fragment has sunk
 * behind what is already drawn. The 0.5 clamp floor buys a dead zone where the
 * ribbon is buried but still at full strength, then a short falloff - 10uu of
 * grace, 10uu of fade, gone. That grace is what lets a stepped staircase rise
 * through a ribbon lying on the plane underneath and dissolve it instead of
 * slicing it. Returns [0, 0.5]. */
float depthFade(float fragDist, float sceneDist)
{
    return 1.0 - clamp((fragDist - sceneDist) * uDepthRate, uDepthFloor, 1.0);
}

void main()
{
    vec2  screen   = gl_FragCoord.xy / uResolution;
    float fragDist = linearDist(gl_FragCoord.z);

    vec2  profile  = lineProfile(vUV);
    float lineMask = profile.x;

    /* Custom_15 "Height Fading", verbatim:
     *   return 1.0-saturate((WorldPos.z - (FadeHeight-FadeDistance))/ FadeDistance);
     * One-sided - it hides border ABOVE the storey being viewed and leaves the
     * floors below alone, so climbing reveals rather than swaps. */
    float height = clamp(1.0 - clamp((vWorldY - (uHideHeight - uHideFade)) / uHideFade,
                                     0.0, 1.0), 0.0, 1.0);

    /* depthFade against the scene, four taps rather than one. That buffer is
     * single-sampled, so where the ribbon crosses a silhouette the fade inherits
     * the depth buffer's own stair steps - the recovered rates are smooth but
     * what they are fed is not. Averaging the FADE (not the depth: averaging
     * distances across a silhouette is meaningless) over a half-pixel diagonal
     * turns that step into a ramp without touching a single constant. */
    vec2  t     = 0.75 / uResolution;
    float depth = 0.25 * (
          depthFade(fragDist, linearDist(texture(depthTex, screen + vec2( t.x,  t.y)).r))
        + depthFade(fragDist, linearDist(texture(depthTex, screen + vec2(-t.x,  t.y)).r))
        + depthFade(fragDist, linearDist(texture(depthTex, screen + vec2( t.x, -t.y)).r))
        + depthFade(fragDist, linearDist(texture(depthTex, screen + vec2(-t.x, -t.y)).r)));

    /* Min_1, not a product: the stronger reason to vanish wins. ConstantScale_9's
     * 2.0 then undoes the depth term's 0.5 ceiling, so an unoccluded fragment
     * lands back at full lineMask. */
    float a = clamp(2.0 * lineMask * min(height, depth), 0.0, 1.0) * uColor.a;
    if (a < 0.003) discard;

    /* EmissiveColor <- "Color", masked to RGB. Unlit, so that is the output.
     * uEmissive is ours, not XCOM's: their glow is the scene's bloom picking up
     * this value, and the glow pass reproduces that by re-running this shader
     * overbright. It is 1.0 everywhere else, which leaves the material exact. */
    vec3 emissive = uColor.rgb * (uEmissive > 1.0
                                  ? mix(uEmissive * 0.55, uEmissive, profile.y)
                                  : 1.0);
    finalColor = vec4(emissive, a);
}
