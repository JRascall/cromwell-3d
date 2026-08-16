#version 450 core
/* imgui.fs.glsl — a Dear ImGui fragment: the vertex colour times the atlas.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ============== ONE MULTIPLY, AND ALL FOUR CHANNELS TAKE PART ============
 *
 * Unlike ui_text.fs.glsl, which reads a single coverage channel and leaves the
 * colour alone, ImGui's atlas is RGBA and its widgets rely on that: a filled
 * rectangle samples a white texel and takes the vertex colour whole, a glyph
 * samples coverage in every channel, and an embedded image takes its colour
 * from the texture with the vertex colour as a tint. One multiply serves all
 * three, which is why every ImGui backend ever written is this line.
 *
 * ================== THIS IS DISPLAY COLOUR, NOT RADIANCE ==================
 *
 * The panel draws after the tone map, so these are the bytes ImGui's theme
 * picked and they reach the screen unchanged. Same contract as the engine's own
 * UI shaders — and the reason the dev panel is drawn where it is rather than
 * inside the lit scene, where it would be graded and exposed like geometry.
 *
 * STRAIGHT ALPHA, matching the blend state the pipeline sets. ImGui premultiplies
 * nothing; a backend that assumed otherwise gets dark fringes on every
 * antialiased edge, which reads as a theme problem rather than a blend one.
 */

layout(binding = 0) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColour;

layout(location = 0) out vec4 outColour;

void main()
{
    outColour = vColour * texture(uAtlas, vTexCoord);
}
