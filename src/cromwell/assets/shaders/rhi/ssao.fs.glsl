#version 450 core
/* ssao.fs.glsl — screen-space ambient occlusion.
 *
 * Converted from ../ssao.fs.glsl; the body below is unchanged and its comments
 * are the record of several real bugs. See assets/shaders/CONVENTIONS.md for
 * the dialect.
 *
 * ===================== WHAT THE CONVERSION CHANGED ========================
 *
 * texture0 / colDiffuse    GONE. They existed because raylib blits this pass
 *                          as a textured quad and binds its own source and
 *                          tint whether the shader wants them or not. The
 *                          device draws a covering triangle with no vertex
 *                          buffer at all, so there is nothing to bind.
 *
 * uKernel                  vec3[24] became vec4[24]. In std140 EVERY array
 *                          element is padded to sixteen bytes, so a vec3 array
 *                          already occupied vec4 stride on the GPU while the
 *                          C++ side packed it tightly — the exact mismatch
 *                          CONVENTIONS.md warns about, and it would have read
 *                          garbage from the fourth element onward. Declaring
 *                          vec4 makes the padding visible instead of latent.
 *
 * the loose uniforms       collected into two std140 blocks by frequency.
 *
 * ==================== ONE OF ITS BUGS IS NOW FIXED UPSTREAM ================
 *
 * The long comment below about normals arriving inverted describes dead code
 * in the raylib prepass: it flips on `!gl_FrontFacing`, but that pass culls
 * back faces, so the flip never ran. The converted prepass uses CullMode::None
 * precisely so a cutaway's undersides are rasterised and that flip is live.
 *
 * The view-vector flip here is kept anyway. It is correct for every surface
 * this pass can see and cannot be defeated by geometry authored inside-out,
 * which a winding test can.
 */

layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uNormals;   /* world normals, n * 0.5 + 0.5 */

layout(location = 0) out vec4 outOcclusion;

layout(std140, binding = 1) uniform PassBlock {
    mat4 uProjection;
    mat4 uInverseProjection;
    mat4 uView;
    vec4 uResolutionAndRadius;   /* xy = resolution, z = radius, w = bias */
    vec4 uStrength;              /* x = strength. vec4 for std140 - see CONVENTIONS.md */
};

/* THE SAMPLE KERNEL, at its own binding because it is written once at startup
 * and never again — mixing it into the per-pass block would re-upload 384
 * bytes every frame to change nothing. */
const int kKernelSize = 24;
layout(std140, binding = 3) uniform KernelBlock {
    vec4 uKernel[kKernelSize];   /* xyz used; w is std140's padding, not data */
};

/* The originals, restored as names the body already uses. */
#define uResolution uResolutionAndRadius.xy
#define uRadius     uResolutionAndRadius.z
#define uBias       uResolutionAndRadius.w
#define finalColor  outOcclusion

vec3 viewPositionAt(vec2 uv, out float rawDepth)
{
    rawDepth = texture(uDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, rawDepth * 2.0 - 1.0, 1.0);
    vec4 view = uInverseProjection * clip;
    return view.xyz / view.w;
}

/* A rotation hashed rather than sampled from a noise texture. The kernel is
 * fixed, so without this every pixel samples the same 24 directions and the
 * result bands; rotating it trades those bands for high-frequency noise, which
 * is what the blur pass is there to remove.
 *
 * IT MUST TILE ON THE BLUR'S FOOTPRINT — see kRotationPeriod below. */
float hash12(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / uResolution;

    float rawDepth;
    vec3 position = viewPositionAt(uv, rawDepth);

    /* Nothing was drawn here - it is sky. Fully open, and an early out for
     * most of the frame at a tactical camera. */
    if (rawDepth >= 1.0) {
        finalColor = vec4(1.0);
        return;
    }

    vec3 worldNormal = texture(uNormals, uv).xyz * 2.0 - 1.0;
    vec3 normal = normalize(mat3(uView) * worldNormal);

    /* A VISIBLE SURFACE FACES THE VIEWER — assert it rather than assume it.
     *
     * Measured on this scene, large areas of plainly visible wall arrive here
     * with N.V NEGATIVE: the G-buffer holds a normal pointing away from the
     * camera. prepass.fs.glsl tries to prevent that with
     * `if (!gl_FrontFacing) normal = -normal`, but backface culling is enabled
     * during the prepass, so back faces are never rasterised, gl_FrontFacing is
     * always true, and that flip is dead code. A box wound inside-out then has
     * its outward face culled, leaves its inward face on screen looking
     * perfectly solid, and stores the inward normal.
     *
     * The consequence here is total. The hemisphere is built around this
     * normal, so an inverted one aims every tap straight INTO the surface,
     * where they find whatever lies behind it and report it as occlusion. That
     * is what "the ambient occlusion is showing geometry through a wall" turns
     * out to be: not a range, bias or blur failure, but the sampling hemisphere
     * pointing the wrong way on those panels.
     *
     * Flipping against the view vector is correct for every surface the AO pass
     * can ever see, and unlike a winding-based test it cannot be defeated by
     * geometry authored inside-out. The emitter should still be fixed; this
     * makes the pass immune to it either way. */
    if (dot(normal, normalize(-position)) < 0.0) normal = -normal;

    /* THE ROTATION REPEATS EVERY FOUR PIXELS, and that is not a detail.
     *
     * The blur that follows is a 4x4 box, justified as "exactly the size of the
     * rotation's correlation window". That justification only holds if the
     * rotation actually HAS a four-pixel period. Hashed on the raw pixel
     * coordinate it has none: every pixel gets an unrelated angle, the noise is
     * full-frequency, and a 4x4 average cannot cancel it — it only softens it,
     * leaving a permanent grainy field over every surface. On a flat wall,
     * where there is no real occlusion to look at, that residue IS the image.
     *
     * The reference implementations all tile a 4x4 noise texture across the
     * screen for exactly this reason: within any 4x4 block every rotation
     * appears once, so the box filter averages one whole period and the noise
     * integrates away. Quantising the hash input reproduces that without
     * needing a texture. */
    const float kRotationPeriod = 4.0;

    float angle = hash12(mod(gl_FragCoord.xy, kRotationPeriod)) * 6.2831853;
    vec3 rotation = vec3(cos(angle), sin(angle), 0.0);

    /* Gram-Schmidt against the normal, so the basis is orthonormal whatever
     * the random direction was. */
    vec3 tangent   = normalize(rotation - normal * dot(rotation, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 basis     = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; i++) {
        vec3 samplePosition = position + (basis * uKernel[i].xyz) * uRadius;

        /* Back to screen space to find what is actually drawn there. */
        vec4 offset = uProjection * vec4(samplePosition, 1.0);
        offset.xyz /= offset.w;
        vec2 sampleUv = offset.xy * 0.5 + 0.5;
        if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThan(sampleUv, vec2(1.0)))) continue;

        /* THE WHOLE POSITION, NOT JUST ITS DEPTH — see the rejection below. */
        float sampleRawDepth;
        vec3  scenePosition = viewPositionAt(sampleUv, sampleRawDepth);
        if (sampleRawDepth >= 1.0) continue;

        /* View space looks down -z, so a LARGER z is nearer the eye: the scene
         * being in front of the sample point means the sample is buried.
         *
         * THE BIAS IS SLOPE-SCALED, exactly as a shadow map's is, and for the
         * identical reason. A FIXED bias assumes the depth difference between
         * a tap and the surface it lands on is small when they are the same
         * surface — true head-on, false edge-on. On a wall seen at a grazing
         * angle a tap displaced sideways within the plane changes depth by
         * nearly its full offset, so "is the scene in front of this tap"
         * becomes a coin toss decided by precision, and roughly half the taps
         * come back buried on a surface that occludes nothing. That false
         * occlusion is not uniform: it is modulated by the tile-sized boxes the
         * wall is built from, so it prints as RECTANGLES on a flat facade.
         *
         * Dividing by N.V grows the bias as the surface turns away — 0.025
         * head-on, roughly six times that at eighty degrees — which is the
         * margin needed for the comparison to stay meaningful. */
        float nDotV = max(dot(normal, normalize(-position)), 0.05);
        float slopeBias = uBias / nDotV;

        bool buried = scenePosition.z >= samplePosition.z + slopeBias;

        /* THE RANGE REJECTION, measured as a DISTANCE IN SPACE rather than a
         * difference in depth. Both halves of that sentence were bugs.
         *
         * It must reach ZERO. The original guard was
         * `smoothstep(0, 1, uRadius / delta)`, a RECIPROCAL that decays without
         * ever arriving: at twice the radius it still passed half the
         * occlusion, at four times a sixth, and it never stopped.
         *
         * And it must measure the right thing. Comparing only DEPTH says
         * nothing about how far apart two points actually are. A wall seen at a
         * grazing angle from close up spans a huge screen area at nearly
         * constant depth, so a sample's projected position can travel most of
         * the way across the frame and land on geometry that is metres away
         * laterally but at a near-identical z. A depth-only test waves that
         * through, the occluder is nearer than the sample point, and it counts
         * as burying it — which is how a staircase four tiles behind a solid
         * wall comes to be drawn on the face of it, in recognisable steps.
         * Grazing angles and a close camera are exactly when this bites, which
         * is why the artefact appears from some viewpoints and not others.
         *
         * The radius is the sampling SPHERE. What decides whether an occluder
         * is inside it is its distance from the shading point in three
         * dimensions, so that is what is measured. Full weight within half a
         * radius, nothing beyond one — contact darkening lives well inside the
         * sphere and is untouched. */
        vec3  toOccluder       = scenePosition - position;
        float occluderDistance = length(toOccluder);
        float withinRange = 1.0 - smoothstep(uRadius * 0.5, uRadius, occluderDistance);

        /* THE COSINE TERM, and without it a flat wall occludes ITSELF.
         *
         * Ambient occlusion asks how much of the hemisphere ABOVE a surface is
         * blocked, so an occluder's contribution is weighted by how far it
         * lies along the normal — the same cosine the rendering equation has.
         * A binary "is it buried" test has no such weighting, and an occluder
         * lying IN the surface's own tangent plane counts as fully as one
         * directly overhead.
         *
         * That is disastrous on a surface seen edge-on. A tangential sample on
         * a flat wall projects to a screen position still on that same wall,
         * so the depth found there differs from the sample's by less than the
         * bias, and which side of the comparison it lands on is decided by
         * floating-point noise. Half the taps come back "buried" and the wall
         * darkens itself into a grey field of noise — and any geometry BEHIND
         * it, being further away, fails the buried test and stops contributing,
         * so it prints its own silhouette into that field as CLEAN bands. A
         * staircase behind a wall therefore appears on the wall in reverse:
         * not shaded onto it, but stencilled out of its self-occlusion.
         *
         * Weighting by dot(normal, direction) sends tangent-plane occluders to
         * zero, which is where the whole artefact lives. */
        float facing = max(dot(normal, toOccluder / max(occluderDistance, 1e-4)), 0.0);

        /* A TAP THAT LANDED ON ITSELF TELLS US NOTHING, and left in it lies.
         *
         * When the projected sample comes back to essentially the point being
         * shaded, `toOccluder` is a near-zero vector whose DIRECTION is pure
         * floating-point noise — normalising it yields an arbitrary unit
         * vector, so the cosine weight above is arbitrary too, and on a flat
         * surface roughly half of those arbitrary directions point away from
         * the plane and register as occluders. Measured on a flat wall, taps
         * were reporting large off-plane angles almost everywhere for exactly
         * this reason.
         *
         * The threshold is the bias: the bias already declares that a depth
         * difference smaller than this is surface detail rather than an
         * occluder, and the same statement in three dimensions is that an
         * occluder this close is the surface itself. */
        if (occluderDistance < uBias) continue;

        occlusion += buried ? withinRange * facing : 0.0;
    }

    float ao = 1.0 - (occlusion / float(kKernelSize)) * uStrength.x;
    finalColor = vec4(vec3(clamp(ao, 0.0, 1.0)), 1.0);
}
