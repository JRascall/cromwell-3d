#version 450 core
/* ui_blur.vs.glsl — the frosted region's outline, from screen pixels to clip.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * SAME VERTEX AS THE GLYPH QUADS — position, UV, colour — and it shares their
 * buffer for exactly that reason: a rounded-rect fan and a run of letters are
 * the same twenty bytes per corner, and two buffers holding one layout is two
 * things to grow, upload and keep in step. What differs is the shader, which is
 * what a pipeline is for.
 *
 * The UV addresses the CAPTURED REGION rather than the screen. The capture
 * texture only ever grows, so the region in use is a sub-rectangle of it and the
 * painter scales the coordinates accordingly — see DeviceUiPainter::appendBlur.
 */

layout(location = 0) in vec2 inPosition;   /* screen pixels, y DOWN */
layout(location = 1) in vec2 inTexCoord;   /* into the captured region */
layout(location = 2) in vec4 inColour;     /* a tint, normally white */

/* [0].xy surface size, [0].z the mip level to read. Declared identically in the
 * fragment stage, which is how rhi/object.glsl already shares it across a
 * program — the linker merges the two declarations. */
layout(location = 0) uniform vec4 uPushConstants[8];

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColour;

void main()
{
    vec2 surface = uPushConstants[0].xy;

    /* Identical to ui.vs.glsl and ui_text.vs.glsl, and it must stay identical:
     * a frosted panel and the plate drawn over it have to land on the same
     * pixel. */
    vec2 ndc = (inPosition / surface) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vTexCoord = inTexCoord;
    vColour   = inColour;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
