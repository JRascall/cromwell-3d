#version 450 core
/* prepass.fs.glsl — world normal in RGB, roughness in alpha.
 *
 * Converted from ../prepass.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * THE ALPHA CHANNEL IS ROUGHNESS, NOT COVERAGE, and that is the fact the whole
 * pass is arranged around: blending into this buffer would mix two surfaces'
 * material parameters per pixel and produce a value belonging to neither. The
 * pipeline that draws it has blending off, and it must stay off.
 *
 * WHAT READS IT: the occlusion pass orients its sampling hemisphere from these
 * normals, and the decal projector unprojects the depth beside them. Both are
 * reconstructed from ONE viewpoint at one resolution, which is why a camera
 * without its own prepass cannot have either.
 */

layout(location = 0) in vec3 vNormal;

layout(location = 0) out vec4 outNormalRoughness;

/* PER MATERIAL, at binding 2 — the frequency table in CONVENTIONS.md. It is a
 * whole block for one float today; that is deliberate rather than wasteful,
 * because the lit pass's material parameters land in this same block and a
 * push constant would have to be undone the moment they do. */
layout(std140, binding = 2) uniform MaterialBlock {
    vec4 uMaterialFactors;   /* x = roughness. See the padding note in CONVENTIONS.md */
};

void main()
{
    vec3 normal = normalize(vNormal);

    /* UNDERSIDES SEEN THROUGH A CUTAWAY RASTERISE BACK-FACING, and a hemisphere
     * built around an inward normal samples through the surface it sits on —
     * occlusion that darkens the wrong side of a floor. Flipping here is what
     * makes a cut-open building shade correctly. */
    if (!gl_FrontFacing) normal = -normal;

    outNormalRoughness = vec4(normal * 0.5 + 0.5, clamp(uMaterialFactors.x, 0.0, 1.0));
}
