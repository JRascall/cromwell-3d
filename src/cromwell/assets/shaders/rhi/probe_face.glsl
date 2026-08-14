/* rhi/probe_face.glsl — which world direction a cube-face texel looks along.
 *
 * The inverse of GL's cube-face selection table. A pass rendering INTO one face
 * of a cubemap covers it with a triangle and needs, per fragment, the direction
 * that texel will be fetched by when something samples the cube later. That is
 * the whole job.
 *
 * ================== THE TABLE IS THE SPEC'S, READ BACKWARDS ================
 *
 * GL defines the forward mapping: for a direction, pick the major axis, then
 * derive (sc, tc) from the other two and scale into (s, t). This undoes it —
 * given (s, t) on a known face, recover the direction. Four of the six faces
 * negate their vertical component, which looks like a typo six times over and
 * is not: a cubemap's faces are defined in a LEFT-handed space, and this is
 * where that shows up if it has not already.
 *
 * DO NOT "SIMPLIFY" THE SIGNS. They are checked by the device self-test's cube
 * orientation stage, which renders a marker at a known world direction and
 * samples it back — the one test in the suite that exists because reasoning
 * about this table produced a confident wrong answer twice.
 */
#ifndef XCOM_RHI_PROBE_FACE
#define XCOM_RHI_PROBE_FACE

/* WHAT ONE PREFILTER DISPATCH IS FOR. Binding 1 is the PASS slot in
 * CONVENTIONS.md's frequency table, which is exactly right: every one of these
 * changes per (probe, face, level).
 *
 * PACKED INTO TWO vec4s because std140 pads every member to sixteen bytes, so
 * six loose scalars would occupy ninety-six. */
layout(std140, binding = 1) uniform ProbeFaceBlock {
    /* x roughness for this level, y sample count, z which probe (array layer),
     * w which face, 0..5 in GL's order. */
    vec4 uProbePrefilter;

    /* x the face's size in texels at THIS level, yzw spare. Passed rather than
     * taken from textureSize(): that would report the SOURCE level's size, and
     * this pass writes the one below it. */
    vec4 uProbeFace;
};

vec3 probeFaceDirection()
{
    /* gl_FragCoord is at pixel centres, so this lands on texel centres in 0..1
     * and never on an edge — which matters because an edge sample on a clamped
     * cube face reads the neighbouring face's border rather than its own. */
    vec2 uv = gl_FragCoord.xy / max(uProbeFace.x, 1.0);

    /* Into -1..1, which is what the spec's sc/tc are expressed in. */
    float s = uv.x * 2.0 - 1.0;
    float t = uv.y * 2.0 - 1.0;

    int face = int(uProbePrefilter.w + 0.5);

    if (face == 0) return vec3( 1.0,   -t,   -s);   /* +X */
    if (face == 1) return vec3(-1.0,   -t,    s);   /* -X */
    if (face == 2) return vec3(   s,  1.0,    t);   /* +Y */
    if (face == 3) return vec3(   s, -1.0,   -t);   /* -Y */
    if (face == 4) return vec3(   s,   -t,  1.0);   /* +Z */
    return                    vec3(  -s,   -t, -1.0);   /* -Z */
}

#endif
