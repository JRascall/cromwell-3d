/* rhi/decal_blocks.glsl — the decal pass's two blocks, declared once.
 *
 * A uniform block is matched across stages BY NAME, so a vertex shader and a
 * fragment shader that declare the same block differently do not warn — they
 * fail to link with a message some distance from the edit, or worse they both
 * link and one reads every member past the divergence at the wrong offset.
 * scene_block.glsl carries the same note and the same fix.
 *
 * TWO BLOCKS AT TWO FREQUENCIES, per assets/shaders/CONVENTIONS.md's binding
 * table: 1 is the PASS and 3 is the OBJECT. The pass block is written once for
 * the whole decal pass; the object block is one slice of a buffer holding every
 * decal, bound at an offset per draw.
 *
 * WHY THE PASS BLOCK IS THE DECAL'S OWN AND NOT scene_block's. Because this
 * pass needs almost nothing that one carries — no sun, no sky, no shadow
 * scales, no probes — and it needs one thing that one does not: the INVERSE
 * view-projection, which is what turns a depth-buffer sample back into a world
 * position. Adding an inverse to the block every geometry shader binds would be
 * sixty-four bytes uploaded per pass for the benefit of one.
 */
#ifndef XCOM_RHI_DECAL_BLOCKS
#define XCOM_RHI_DECAL_BLOCKS

layout(std140, binding = 1) uniform DecalPassBlock {
    mat4 uViewProjection;

    /* WHAT THE FRAGMENT STAGE IS REALLY FOR. Window space -> NDC -> world, so
     * the pass can recover the surface actually under each pixel of the
     * projector box. It is built on the CPU from the same matrices the prepass
     * was rendered with; a mismatch here places every decal on a surface that
     * is not there. */
    mat4 uInverseViewProjection;

    /* xy = the DBUFFER's size in pixels — which is the size of the depth
     * texture being unprojected, not the window's. zw spare. */
    vec4 uResolution;
};

layout(std140, binding = 3) uniform DecalObjectBlock {
    /* Unit cube -> world. Its three columns are the decal's tangent, bitangent
     * and normal SCALED TO ITS SIZE, and the fragment stage reads those lengths
     * — local coordinates are normalised per axis, so a distance along W is
     * only comparable with one along U after being scaled by the ratio. */
    mat4 uModel;

    /* And its inverse, which turns a recovered world position into the decal's
     * own space where the box is the unit cube and the UV is a shift.
     * CARRIED RATHER THAN COMPUTED: GLSL has inverse(mat4), and calling it per
     * fragment for a value that is constant across the whole draw is the
     * definition of work done in the wrong place. */
    mat4 uInverseModel;

    vec4 uTint;       /* multiplies the albedo map                          */
    vec4 uFactors;    /* rough, metal, normalStrength, opacity              */
    vec4 uFade;       /* angleStart, angleEnd, depthFade, emissive          */

    /* x: 1 to resolve the projection per pixel and carry the texture round
     * corners, 0 for a single fixed axis. Kept switchable rather than always on
     * because the two are genuinely different looks — a poster is a flat
     * rectangle that should stop at the edge of its wall, and a splatter is a
     * thing thrown at the world that should run over whatever it hits. yzw
     * spare. */
    vec4 uWrap;
};

#endif
