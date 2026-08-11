/* WorldText.hpp — text that lives in the scene rather than on the glass.
 *
 * SINGLE RESPONSIBILITY: draw a string at a point in the world, crisp at any
 * distance. Nameplates, floating damage numbers, map markers, signage.
 *
 * WHY THIS IS NOT DrawBillboard WITH A FONT TEXTURE. Because that is the thing
 * that looks wrong the moment the camera moves: a glyph atlas rasterised at one
 * size blurs when magnified and shimmers when minified, and world text is
 * magnified and minified continuously. The atlas here stores DISTANCE to the
 * glyph edge instead of coverage, and the shader reconstructs a sharp edge from
 * it at whatever size the fragment happens to be — see study/topics/surfaces/distance_fields.md.
 *
 * BILLBOARDED, AND THAT IS A CHOICE THIS CLASS MAKES. Every quad faces the
 * camera, because the things wanting world text first — a name over a unit, a
 * number floating off a hit — are unreadable at a glancing angle and there is
 * nothing to gain from letting them turn edge-on. Text that should lie ON a
 * surface (floor markings, painted signage) wants an orientation instead of a
 * camera, which is a different entry point rather than a flag on this one:
 * billboarding derives its basis from the view matrix, and a surface derives it
 * from the surface, so sharing the code would mean passing one and ignoring the
 * other.
 *
 * ONE FONT PER RENDERER. A weight is a separate atlas, so drawing two weights
 * means two of these. That is deliberate — batching is per atlas, and hiding
 * two textures behind one object would silently break the batch that makes this
 * cheap.
 *
 * THE COST. Two triangles per glyph and a fragment shader that is a median, a
 * derivative and two clamps. Two hundred five-digit numbers is four thousand
 * vertices against one bound texture. The expensive half of a distance field is
 * BAKING it, which happened at build time — see tools/fonts/bake_msdf.py.
 */
#pragma once

#include "cromwell/sdf/MsdfFont.hpp"

#include "raylib.h"

#include <string>
#include <string_view>
#include <vector>

namespace cromwell::sdf {

class WorldText {
public:
    WorldText() = default;
    ~WorldText();

    WorldText(const WorldText&) = delete;
    WorldText& operator=(const WorldText&) = delete;

    /* Loads the baked atlas named by `cwfontPath` and the text shader.
     * Returns false if either is missing — the font pack is licensed and not in
     * git, so absent is a normal state and every caller is expected to skip
     * drawing rather than assert.
     *
     * REQUIRES A GL CONTEXT. */
    bool load(const std::string& cwfontPath);
    void unload();
    bool ready() const;

    /* A dark rim around every glyph, in SCREEN PIXELS.
     *
     * On by default, which is not a decoration. World text has no control over
     * what is behind it — white over a snowfield, or over a pale wall, is
     * simply unreadable — and the rim costs one extra threshold of a distance
     * the shader already sampled. Width 0 turns it off.
     *
     * Pixels rather than world units, because the text has no fixed size: a rim
     * measured in em would vanish at distance and swallow the glyph up close. */
    void setOutline(Color colour, float widthPixels);

    /* Draws `text` centred on `position`, with one em standing `emHeight` world
     * units tall. Call inside BeginMode3D.
     *
     * Centred on both axes: horizontally on the run's advance width, vertically
     * between ascender and descender, so a caller positions the label where it
     * wants the text to APPEAR rather than computing an offset from metrics it
     * would have to ask for. */
    void draw(const Camera3D& camera, std::string_view text, Vector3 position,
              float emHeight, Color colour);

    /* The loaded face, for a caller that needs to measure before it places —
     * laying out a background panel behind a label, say. */
    const MsdfFont& font() const { return font_; }

private:
    MsdfFont font_;
    Shader   shader_{};
    bool     shaderReady_ = false;

    /* Uniform locations, resolved once at load. Looking a uniform up by name is
     * a string compare inside the driver, and this runs per label per frame. */
    int pxRangeLoc_ = -1;
    int outlineColourLoc_ = -1;
    int outlineWidthLoc_ = -1;

    Color outlineColour_{ 0, 0, 0, 255 };
    float outlineWidthPx_ = 1.5f;

    /* Reused across draws so a frame of labels allocates nothing. Mutable
     * scratch rather than a local, which is the same reason SpatialHash's
     * queries take a caller-supplied vector. */
    std::vector<MsdfQuad> quads_;
};

}  // namespace cromwell::sdf
