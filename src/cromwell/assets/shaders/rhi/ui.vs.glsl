#version 450 core
/* ui.vs.glsl — screen pixels to clip space, and nothing else.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * THE UI WORKS IN PIXELS WITH Y DOWN, which is what every widget above it
 * measures and lays out in, and what a designer means by "eight pixels of
 * padding". This is the one place that becomes a GPU coordinate.
 *
 * NO PROJECTION MATRIX. An orthographic screen projection is four multiplies
 * and two adds — building a mat4 to carry six useful numbers would cost a
 * uniform buffer, a matrix build per frame and a reader's trip to another file
 * to find out it was an ortho all along.
 */

layout(location = 0) in vec2 inPosition;   /* screen pixels, y DOWN */
layout(location = 1) in vec4 inColour;     /* sRGB, normalised from bytes */

/* THE TARGET'S SIZE IN PIXELS, as push constants rather than a block: it is two
 * floats that change only on a resize, and the alternative is a uniform buffer
 * for eight bytes. See rhi/object.glsl on the reserved location. */
layout(location = 0) uniform vec4 uPushConstants[8];

layout(location = 0) out vec4 vColour;

void main()
{
    vec2 surface = uPushConstants[0].xy;

    /* Pixels to 0..1, then to -1..1. The Y TERM IS NEGATED, which is the whole
     * of the y-down-to-y-up conversion: clip space runs upward and the UI runs
     * downward, so a panel laid out at the top of the screen would otherwise
     * draw at the bottom. */
    vec2 ndc = (inPosition / surface) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vColour = inColour;

    /* z = 0 puts every vertex on the near plane, which is inside the 0..1 clip
     * range the engine uses — see Mat4.hpp. The pipeline has no depth test, so
     * the value only has to be in range, and painter's order does the rest. */
    gl_Position = vec4(ndc, 0.0, 1.0);
}
