/* rhi/object.glsl — where one drawn thing is, and what colour it is tinted.
 *
 * THE PER-OBJECT FREQUENCY, in push constants rather than in a uniform block at
 * binding 3. CONVENTIONS.md allows either; this picks the constants because the
 * payload is one matrix and one colour, and because it is written between
 * draws — dozens of times in a pass, for the bodies. A uniform buffer used that
 * way is either one buffer rewritten per draw (a pipeline stall on every
 * backend that means it) or an allocator, and neither is worth building for
 * eighty bytes.
 *
 * ============== WHY EVEN THE STATIC WORLD PUSHES A TRANSFORM ==============
 *
 * The lattice is emitted in world space and needs no transform at all, so an
 * obvious economy is a second pipeline without one. That was rejected: it
 * doubles every pass — shadow, prepass and lit each grow a static and a dynamic
 * variant — to save one uniform write per submission, and the two variants then
 * have to be kept in step by hand for the life of the renderer.
 *
 * So there is one path, and the static world pushes identity once before its
 * buckets. A shader permutation is a maintenance cost that never goes away; a
 * matrix multiply by identity is a few instructions the vertex stage was going
 * to spend on a matrix multiply anyway.
 *
 * ======================= WHAT IS NOT IN HERE, AND WHY =====================
 *
 * A NORMAL MATRIX. `mat3(model)` is used directly by the callers, which is
 * exact for everything currently drawn — the world is untransformed and the
 * bodies are axis-aligned boxes, where a per-axis scale followed by a normalise
 * gives back the same unit axis the inverse transpose would.
 *
 * It stops being exact the moment something is both ROTATED and non-uniformly
 * scaled, which no current object is. When one arrives it needs the real
 * inverse transpose, and that is three more vec4s — 128 bytes exactly, with the
 * tint's slot gone. That is the point at which this moves to an ObjectBlock at
 * binding 3, and the reason to write the limit down rather than let it be
 * discovered as a lighting bug on the first rotated prop.
 */
#ifndef XCOM_RHI_OBJECT
#define XCOM_RHI_OBJECT

/* THE RESERVED LOCATION. ICommandEncoder::pushConstants writes here on every
 * backend — real push constants on Vulkan and Metal, a uniform array at
 * location 0 on GL. Eight vec4s is the 128 bytes every backend guarantees;
 * five are used. */
layout(location = 0) uniform vec4 uPushConstants[8];

/* COLUMNS, IN ORDER. GLSL's mat4(vec4,vec4,vec4,vec4) takes columns, and
 * cromwell::Mat4 stores columns — m[c * 4 + r]. The two agree, so the sixteen
 * floats travel across unpermuted and there is no transpose anywhere in this
 * path. See Mat4.hpp, which chose that layout for exactly this reason. */
mat4 objectTransform()
{
    return mat4(uPushConstants[0], uPushConstants[1],
                uPushConstants[2], uPushConstants[3]);
}

/* MULTIPLIES the vertex colour rather than replacing it. The world's vertices
 * carry the emitter's per-surface colour and want white here; a body's cube is
 * white and takes its colour entirely from this. One expression serves both,
 * with no flag saying which kind of thing is being drawn. */
vec4 objectTint()
{
    return uPushConstants[4];
}

#endif
