#version 450 core
/* imgui.vs.glsl — a Dear ImGui vertex, from screen pixels to clip space.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ============ WHY A THIRD UI VERTEX SHADER, BESIDE ui AND ui_text =========
 *
 * Because it is fed by a different producer with a fixed vertex layout that is
 * not ours to change. `ImDrawVert` is position, UV and a packed colour — the
 * same twenty bytes ui_text uses, by coincidence rather than by agreement — and
 * ImGui writes it. A shader shared with the engine's own painter would be one
 * that cannot be changed without checking what ImGui does, which is the wrong
 * way round for a dev tool sitting on top of an engine.
 *
 * THE ARITHMETIC IS ui_text.vs.glsl's, DELIBERATELY IDENTICAL. Both take pixels
 * with y DOWN and both must land on the same pixel as the engine's own UI when
 * the two are on screen together.
 *
 * IT IS THE GAME'S SHADER IN THE ENGINE'S TREE, and that is worth stating: the
 * dev panel is the game's (see the CMake note on why cromwell does not link
 * imgui), but ShaderLibrary resolves one shader root and there is no game-side
 * one. When a game-owned shader directory exists this moves; nothing else about
 * it changes.
 */

layout(location = 0) in vec2 inPosition;   /* screen pixels, y DOWN */
layout(location = 1) in vec2 inTexCoord;   /* atlas texels, normalised */
layout(location = 2) in vec4 inColour;     /* straight, normalised from bytes */

/* THE TARGET'S SIZE IN PIXELS. Push constants rather than a block, matching the
 * other two UI shaders — and pushed again after every bindPipeline, because on
 * GL these are a uniform at location 0 of the CURRENT program and a pipeline
 * switch abandons them. That trap is written up in rhi/MIGRATION.md §5; the
 * symptom is a vertex stage dividing by zero and nothing drawn at all. */
layout(location = 0) uniform vec4 uPushConstants[8];

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColour;

void main()
{
    vec2 surface = uPushConstants[0].xy;

    vec2 ndc = (inPosition / surface) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vTexCoord = inTexCoord;
    vColour   = inColour;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
