#version 330
/* custom_stencil.fs.glsl - which object is this, and how far away.
 *
 * A port of Unreal's CUSTOM DEPTH / CUSTOM STENCIL pass. There, a primitive
 * carries bRenderCustomDepth and a CustomDepthStencilValue - an integer 0-255 -
 * and rendering writes both into a separate depth-stencil buffer that materials
 * and post-process read back as SceneTexture:CustomDepth and CustomStencil.
 * Outlines, x-ray silhouettes, selection highlights, blur and depth-of-field
 * masks are all the same mechanism with different consumers.
 *
 * AN INTEGER PER OBJECT, NOT A CATEGORY PER CHANNEL. Four channels holding
 * four fixed categories is the obvious design and it is the rigid one: two
 * soldiers cannot be told apart, and every new kind of view needs a new
 * channel. One arbitrary value per object subsumes all of that - a consumer
 * asks for the value it cares about, and "all units" is a range test.
 *
 * NO REAL STENCIL, because rlgl has none: it attaches depth as
 * GL_DEPTH_COMPONENT24 with no stencil bits and wraps no stencil calls, so the
 * value goes in a colour channel instead. Same information, same uses, one
 * small buffer.
 *
 * R  the stencil value, 0-255 encoded to 0-1. MUST be sampled with NEAREST:
 *    interpolating between two object ids produces a third that belongs to
 *    nothing.
 * A  coverage. 1 where anything was drawn, so a value of 0 is a real id
 *    rather than "empty" - which matters because 0 is the natural default.
 */
out vec4 finalColor;

/* The object's stencil value, already normalised to 0-1 by the caller. */
uniform float uStencilValue;

void main()
{
    finalColor = vec4(uStencilValue, 0.0, 0.0, 1.0);
}
