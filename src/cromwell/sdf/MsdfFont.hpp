/* MsdfFont.hpp — a typeface as a distance field, for text that has no size.
 *
 * SINGLE RESPONSIBILITY: own one baked MSDF atlas and answer where each glyph
 * is in it. It lays out a string into quads; it does not draw them, and it
 * knows nothing about cameras, billboards or materials.
 *
 * WHY THIS EXISTS ALONGSIDE ui/paint/UiFontSet, WHICH IS ALSO "a font".
 * They answer different questions and the difference is not stylistic:
 *
 *   UiFontSet rasterises a glyph at exactly the pixel size it will be drawn
 *   at, with FreeType hinting. That is the sharpest text possible and it
 *   requires knowing the size in advance. Screen-space UI does.
 *
 *   This does not know the size and cannot. A nameplate's on-screen height
 *   changes every frame as the camera moves, so there is no size to rasterise
 *   at — one distance field serves all of them. It gives up hinting, and below
 *   roughly 12 px on screen it is visibly worse than the other one.
 *
 * So: UI text goes through UiFontSet, world text comes here, and neither is a
 * fallback for the other. See study/distance_fields.md §9.
 *
 * THE ATLAS IS BAKED, NOT BUILT. tools/bake_msdf.py drives msdf-atlas-gen and
 * writes a .bmp beside a .cwfont table. Generating a multi-channel field needs
 * the edge-colouring pass from Chlumsky's thesis and is far too slow to do at
 * load time; drawing with the result is two triangles and a median. Those two
 * facts are constantly confused and they are what makes this cheap.
 *
 * UNITS. Everything the bake produces is in EM and stays that way: a glyph's
 * quad is its plane bounds times whatever size the caller wants, so this class
 * never sees a pixel or a world unit and cannot be wrong about either. Y is
 * DOWN, matching the rest of the engine's 2D conventions and the atlas rows.
 */
#pragma once

#include "raylib.h"

#include <string>
#include <string_view>
#include <vector>

namespace cromwell::sdf {

/* One glyph's quad, in em, plus where to find it in the atlas.
 *
 * A plain carrier: built by the loader, read by the layout, no invariant
 * spanning the fields. Same exception the UI kit's TextRun takes. */
struct MsdfGlyph {
    float advance = 0.0f;

    /* The quad relative to the pen position on the baseline, in em, Y down.
     * Zero for a glyph with no geometry — a space — which is why the loader
     * emits those rather than omitting them. */
    float planeLeft = 0.0f, planeTop = 0.0f, planeRight = 0.0f, planeBottom = 0.0f;

    /* The cell in the atlas, in TEXELS. Divided by the atlas size at layout
     * time rather than stored as UVs, so a table stays readable against the
     * image it came from when a bake looks wrong. */
    float atlasLeft = 0.0f, atlasTop = 0.0f, atlasRight = 0.0f, atlasBottom = 0.0f;

    bool hasGeometry() const { return planeRight > planeLeft; }
};

/* One laid-out glyph: where its quad goes and which part of the atlas fills
 * it. Positions are in em relative to the run's origin, so a caller scales by
 * whatever size it wants and never asks this class about world units. */
struct MsdfQuad {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

class MsdfFont {
public:
    MsdfFont() = default;
    ~MsdfFont();

    MsdfFont(const MsdfFont&) = delete;
    MsdfFont& operator=(const MsdfFont&) = delete;

    /* Loads a .cwfont and the atlas image it names, resolved against the same
     * asset roots as everything else. Returns false and leaves this unloaded
     * if either is missing — the pack is licensed and not in git, so absent is
     * a normal state and callers are expected to degrade rather than assert.
     *
     * REQUIRES A GL CONTEXT: it uploads the atlas. */
    bool load(const std::string& cwfontPath);

    void unload();
    bool loaded() const { return atlas_.id != 0; }

    /* The atlas, for binding. */
    const Texture2D& atlas() const { return atlas_; }

    /* The distance range the atlas was BAKED with, which the shader needs and
     * must not assume. Read from the file rather than from a constant, so a
     * re-bake at a different range cannot silently disagree with the shader.
     * See sdf.glsl on what happens when it does. */
    float pxRange() const { return pxRange_; }

    /* Line advance in em. Multiply by the render size. */
    float lineHeight() const { return lineHeight_; }
    float ascender() const { return ascender_; }
    float descender() const { return descender_; }

    /* Width of `text` in em, so a caller can centre a nameplate over a unit
     * without laying it out twice. */
    float measure(std::string_view text) const;

    /* Lays `text` out into quads, in em, with the origin at the start of the
     * baseline. APPENDS — the vector is not cleared — so a caller building one
     * batch from many strings does not need a temporary per string.
     *
     * ASCII ONLY, matching the bake. A byte outside the atlas is skipped
     * rather than drawn as a box: world text is labels and numbers, and a
     * tofu box floating over a unit is worse than a missing character. */
    void layout(std::string_view text, std::vector<MsdfQuad>& outQuads) const;

private:
    /* Dense over the baked range rather than a map. The set is printable
     * ASCII, so this is one subtraction and a bounds check per character
     * instead of a hash — and layout runs per string per frame for every
     * label on screen, which is the one part of this that is not cold. */
    static constexpr int kFirstCodepoint = 32;
    static constexpr int kCodepointCount = 95;

    const MsdfGlyph* glyphFor(char character) const;

    Texture2D atlas_{};
    MsdfGlyph glyphs_[kCodepointCount]{};

    float pxRange_ = 0.0f;
    float lineHeight_ = 0.0f;
    float ascender_ = 0.0f;
    float descender_ = 0.0f;
    float atlasWidth_ = 0.0f;
    float atlasHeight_ = 0.0f;
};

}  // namespace cromwell::sdf
