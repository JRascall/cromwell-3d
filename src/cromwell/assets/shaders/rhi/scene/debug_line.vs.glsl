#version 450 core
/* rhi/scene/debug_line.vs.glsl — a world-space debug segment.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * The whole vertex stage is one matrix multiply, because a debug line is
 * already in world space by the time DebugDraw has it — there is no model
 * transform, no normal, and nothing to light. It rides the PASS block the lit
 * pass already uploaded rather than carrying its own: the debug pass draws into
 * the same target, from the same camera, in the same frame.
 */
#include "rhi/include/scene_block.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;    /* LINEAR, normalised from bytes */

layout(location = 0) out vec4 vColour;

void main()
{
    vColour = inColour;
    gl_Position = uViewProjection * vec4(inPosition, 1.0);
}
