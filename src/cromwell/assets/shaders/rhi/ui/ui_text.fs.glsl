#version 450 core
/* ui_text.fs.glsl — a glyph's coverage, as the alpha of the text's colour.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ================== ONE CHANNEL, AND IT IS ALREADY CURVED =================
 *
 * The atlas holds the fraction of each pixel the glyph covers, with the gamma
 * curve applied at bake time — see the long note in ui/text/GlyphAtlas.cpp for
 * why that curve exists and why it is not applied here. NOTHING IN THIS FILE
 * MAY TOUCH IT. Decoding it as though it were an sRGB value would be the
 * classic double correction, and on light-on-dark text — which is this entire
 * UI — it reads as stems that are too thin and edges that look stepped.
 *
 * ================== THIS IS DISPLAY COLOUR, NOT RADIANCE ==================
 *
 * The whole UI runs AFTER the tone map, so vColour is already the bytes a
 * designer picked and reaches the screen unchanged. Same contract as
 * ui.fs.glsl; the only difference here is what modulates the alpha.
 */

layout(binding = 0) uniform sampler2D uGlyphAtlas;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColour;

layout(location = 0) out vec4 outColour;

void main()
{
    /* STRAIGHT ALPHA, matching the blend state the pipeline sets and matching
     * the shapes drawn beside it: the colour is at full strength and the
     * coverage rides in alpha alone. Premultiplying here would darken every
     * antialiased glyph edge towards black, which on text reads as a dirty
     * fringe rather than as a blending mistake. */
    outColour = vec4(vColour.rgb, vColour.a * texture(uGlyphAtlas, vTexCoord).r);
}
