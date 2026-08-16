#version 450 core
/* custom_stencil.fs.glsl — tag one object, for a later effect to find.
 *
 * Converted from ../../custom_stencil.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * A PORT OF UNREAL'S CUSTOM DEPTH / CUSTOM STENCIL. There, a primitive carries
 * bRenderCustomDepth and a CustomDepthStencilValue — an integer 0-255 — and the
 * pass writes both into a buffer that materials and post-process read back as
 * SceneTexture:CustomDepth and CustomStencil. It is deliberately general:
 * selection outlines, x-ray silhouettes, masking a post effect to particular
 * actors, depth-of-field and blur masks are all this buffer with different
 * consumers. That generality is the reason to copy it rather than build a
 * "selection outline" feature.
 *
 * ============= WHY THE VALUE IS IN A COLOUR CHANNEL ON BOTH PATHS =========
 *
 * On the raylib renderer it is FORCED: rlgl attaches depth with no stencil bits
 * and wraps no stencil functions, so a real stencil would mean raw GL from
 * outside raylib.
 *
 * HERE IT IS A CHOICE, and worth stating because this RHI does have D24S8. A
 * hardware stencil would give eight bits of value and nothing else; this gives
 * the value AND a coverage channel, which is what makes an id of ZERO
 * distinguishable from "nothing was drawn here". A hardware stencil could only
 * make that distinction by reserving a value and losing it. It also costs no
 * stencil pipeline state, which this RHI does not have — see MIGRATION.md §4.10
 * on that interface being half-built.
 *
 * DEPTH GOES TO THE DEPTH ATTACHMENT as usual, and that is the half that makes
 * the buffer worth having: a consumer can ask whether the tagged object is
 * BEHIND what is on screen, and draw an occluded silhouette differently from a
 * visible one.
 */
#include "rhi/include/object.glsl"

layout(location = 0) out vec4 outStencil;

void main()
{
    /* ENCODED TO 0..1 IN AN 8-BIT CHANNEL, so the value survives exactly — 256
     * codes for 256 integers, read back as round(r * 255). The division is the
     * only arithmetic in this shader.
     *
     * ALPHA IS COVERAGE, NOT OPACITY. It is 1 wherever anything was drawn, so
     * that a stencil value of zero is a real id rather than "the buffer was
     * cleared here". */
    outStencil = vec4(objectStencil() / 255.0, 0.0, 0.0, 1.0);
}
