#version 450 core
/* ui.fs.glsl — the vertex colour, straight out.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * THE MESH IS UNTEXTURED, ALWAYS. Every shape in the kit is exact vertex
 * geometry with a feathered edge — see ui/shape/Shapes.hpp for why that beats a
 * rounded-box shader at these sizes — so there is no UV, no atlas and nothing
 * to sample. Text is a separate command kind for exactly that reason.
 *
 * ================== THIS IS DISPLAY COLOUR, NOT RADIANCE ==================
 *
 * The whole UI runs AFTER the tone map, so its colours are already the bytes a
 * designer picked and must reach the screen unchanged. Nothing here decodes
 * sRGB, applies exposure or touches a curve — a UI that went through the tone
 * map would have its greys shifted and its brand colours quietly wrong, which
 * is the sort of thing that gets reported as "the theme looks washed out" and
 * chased through the theme.
 */

layout(location = 0) in vec4 vColour;

layout(location = 0) out vec4 outColour;

void main()
{
    /* STRAIGHT ALPHA, matching the blend state the pipeline sets. The kit
     * authors colours as (rgb, coverage) and every feathered edge in it ramps
     * the alpha alone, so premultiplying here would darken every antialiased
     * edge towards black. */
    outColour = vColour;
}
