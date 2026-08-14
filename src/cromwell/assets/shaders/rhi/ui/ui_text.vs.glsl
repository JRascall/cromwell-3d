#version 450 core
/* ui_text.vs.glsl — a glyph quad, from screen pixels to clip space.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * THE SAME CONVERSION AS ui.vs.glsl, and a separate shader anyway. Text is the
 * only thing in this UI that samples a texture — every shape above it is exact
 * vertex geometry with a feathered edge (see ui/shape/Shapes.hpp) — so the two
 * differ by a UV, which is an attribute rather than a branch. Folding them into
 * one shader would mean either a UV on every shape vertex, which is eight bytes
 * per corner of every panel to carry nothing, or a uniform branch taken by
 * every fragment in the frame to decide whether to sample.
 *
 * The conversion itself is duplicated deliberately rather than shared through
 * an include: it is two lines, and an include that spliced a function into both
 * would be more machinery than the arithmetic it hides.
 */

layout(location = 0) in vec2 inPosition;   /* screen pixels, y DOWN */
layout(location = 1) in vec2 inTexCoord;   /* atlas texels, normalised */
layout(location = 2) in vec4 inColour;     /* sRGB, normalised from bytes */

/* THE TARGET'S SIZE IN PIXELS, as push constants rather than a block — see the
 * note in ui.vs.glsl. PUSHED AGAIN WHENEVER THIS PIPELINE IS BOUND: on GL these
 * are a uniform at location 0 of the CURRENT PROGRAM, so a pipeline switch
 * loses them, and a lost surface size is a UI drawn at whatever the last
 * program happened to leave there. */
layout(location = 0) uniform vec4 uPushConstants[8];

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColour;

void main()
{
    vec2 surface = uPushConstants[0].xy;

    /* Pixels to 0..1, then to -1..1, with Y negated for the y-down-to-y-up
     * conversion. Identical to ui.vs.glsl, and it has to stay identical: a
     * label and the plate under it are drawn by two pipelines and must land on
     * the same pixel. */
    vec2 ndc = (inPosition / surface) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vTexCoord = inTexCoord;
    vColour   = inColour;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
