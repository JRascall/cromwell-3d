#include "cromwell/ui/text/GlyphAtlas.hpp"

#include "cromwell/diag/Logger.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace cromwell::ui {

namespace {

/* ---- the FreeType rasteriser, with SUBPIXEL POSITIONING -----------------
 *
 * THE PROBLEM. Text does not land on whole pixels. Letter spacing is
 * fractional in real styles - this kit uses 1.8, 2.1 and 2.4 - so the pen
 * drifts off the pixel grid from the second character of every run. A glyph
 * bitmap drawn at a fractional X is resampled by the texture filter, and
 * resampling a one-pixel stem turns it into two half-strength ones. That is
 * the soft, washed look, and it is what "blurry UI text" almost always is.
 *
 * THE TWO ANSWERS, AND WHY THIS IS THE SECOND ONE.
 *
 *   1. SNAP TO WHOLE PIXELS. Source does this - integer positions, point
 *      sampled font textures. Perfectly crisp, and it quantises letter
 *      spacing: 2.4 px of tracking becomes alternating 2s and 3s.
 *   2. SUBPIXEL POSITIONING. Rasterise each glyph several times, shifted by a
 *      fraction of a pixel, and pick the variant nearest the position wanted.
 *      The bitmap is always texel-aligned, so nothing is ever resampled, and
 *      the text still sits where the layout put it. This is what Godot does by
 *      default and what the desktop stacks do.
 *
 * The mechanism is built either way; the count of phases is UiFontSet's to
 * choose, and it currently chooses one because native hinting and subpixel
 * placement are alternatives rather than additions. The reasoning for that
 * trade lives on UiFontSet::kPhaseCount, at the point where the number is set.
 *
 * WHY NOT LCD SUBPIXEL ANTIALIASING, which was tried here first: it triples
 * horizontal resolution by treating the R, G and B stripes as separate
 * samples, and it works - but it tints every edge, and on a panel that is not
 * a standard RGB stripe (OLED, rotated) the tint is wrong rather than merely
 * present. Godot ships it as an option and does NOT default to it for exactly
 * that reason, and neither does Unreal or Source. Greyscale coverage with
 * subpixel POSITIONING gets most of the sharpness with none of the colour.
 *
 * The library handle is per-process and never released. FT_Library is a few
 * hundred bytes of module tables, this is its only user, and a static
 * destruction-order bug is a worse trade than the memory. */
FT_Library ftLibrary()
{
    static FT_Library library = [] {
        FT_Library created = nullptr;
        if (FT_Init_FreeType(&created) != 0) {
            LOGGER->warn("ui: FreeType failed to initialise; text falls back");
            return static_cast<FT_Library>(nullptr);
        }
        return created;
    }();
    return library;
}

/* ---- coverage gamma -----------------------------------------------------
 *
 * WHAT FREETYPE HANDS BACK IS NOT ALPHA. It is the fraction of the pixel the
 * glyph covers - a LINEAR quantity. Writing it straight into a straight-alpha
 * blend against gamma-encoded colours is the single most common mistake in
 * text rendering, and it is asymmetric: for LIGHT TEXT ON DARK, which is this
 * entire UI, an edge at 50% coverage lands at sRGB 0.5, which the eye reads as
 * far darker than the ~0.73 that half the light actually is. Every antialiased
 * edge comes out too dark, so stems look thin and their steps look hard -
 * which reads as pixelation rather than as anything to do with gamma.
 *
 * The fix is to pre-curve the coverage. 1.45 rather than a true 2.2 sRGB
 * inverse on purpose: full linear correction overshoots and makes light text
 * look bold and slightly bloomy, which is the "unbalanced font weights"
 * problem that stops people blending text in linear space at all. This is the
 * middle setting that keeps the weight and recovers the smoothness.
 *
 * Baked into the atlas rather than applied in a shader because the atlas is
 * built once and drawn thousands of times, and because it keeps both painters'
 * text on one code path - no second pipeline state, and no risk of the two
 * disagreeing about the curve. The cost is that it is tuned for a dark UI; a
 * light-on-white UI would want the inverse, and would want it as a bake
 * parameter rather than a constant. */
constexpr float kCoverageGamma = 1.45f;

/* 256-entry table, built once per process rather than a pow() per texel. */
const unsigned char* coverageCurve()
{
    static const std::vector<unsigned char> table = [] {
        std::vector<unsigned char> values(256);
        for (int i = 0; i < 256; ++i) {
            const float linear = static_cast<float>(i) / 255.0f;
            const float curved = std::pow(linear, 1.0f / kCoverageGamma);
            values[i] = static_cast<unsigned char>(curved * 255.0f + 0.5f);
        }
        return values;
    }();
    return table.data();
}

/* One rasterised glyph, before packing. */
struct RasterGlyph {
    std::vector<unsigned char> coverage;   /* width*height, 8-bit */
    int width = 0;
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
    int advanceX = 0;
};

}  // namespace

int GlyphAtlas::indexOf(int codepoint)
{
    if (codepoint >= kFirstCodepoint && codepoint < kFirstCodepoint + kCodepointCount) {
        return codepoint - kFirstCodepoint;
    }
    /* '?', which is what raylib's GetGlyphIndex falls back to. */
    return '?' - kFirstCodepoint;
}

GlyphAtlas GlyphAtlas::bake(const std::string& path, int sizePx, int phase, int phaseCount)
{
    GlyphAtlas atlas;

    if (phaseCount < 1) phaseCount = 1;

    FT_Library library = ftLibrary();
    if (library == nullptr) return atlas;

    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0) return atlas;

    /* Width 0 means "square pixels, take it from the height". */
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(sizePx)) != 0) {
        FT_Done_Face(face);
        return atlas;
    }

    /* The shift, in 26.6 fixed point: a whole pixel is 64, so a quarter is 16. */
    const FT_Pos shift = static_cast<FT_Pos>(phase) * (64 / phaseCount);

    /* Where the baseline sits below the top of the line box. FreeType reports
     * bitmap_top relative to the BASELINE and the painters position from the
     * TOP, so every glyph is offset by this. Metrics are 26.6 fixed point. */
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);

    std::vector<RasterGlyph> raster(kCodepointCount);
    int rasterised = 0;
    for (int i = 0; i < kCodepointCount; ++i) {
        const FT_ULong codepoint =
            static_cast<FT_ULong>(kFirstCodepoint) + static_cast<FT_ULong>(i);

        /* LOADED WITHOUT RENDERING, so the outline can be moved first. This is
         * the whole mechanism: FT_Load_Char with FT_LOAD_RENDER would hand back
         * a finished bitmap snapped to the grid, and there would be nothing
         * left to shift. */
        /* NO FLAGS, WHICH MEANS THE FONT'S OWN HINTING. FreeType runs the
         * TrueType bytecode in fpgm/prep/cvt - instructions the type designer
         * wrote for exactly these sizes - rather than the autohinter's guess or
         * no hinting at all. Inter ships them, and this is the single largest
         * difference between crisp small text and soft small text.
         *
         * It is also what Dear ImGui uses (imgui_freetype.cpp leaves LoadFlags
         * at 0), which is the controlled comparison that settled it: same
         * FreeType, same face, same window, and its text was visibly crisper
         * than ours until this changed. */
        if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) {
            continue;
        }
        const FT_GlyphSlot slot = face->glyph;

        /* The advance is the UNSHIFTED one. The phase moves where this glyph is
         * drawn, not how far the pen travels afterwards - carrying the shift
         * into the advance would make every following character drift. */
        raster[i].advanceX = static_cast<int>(slot->advance.x >> 6);

        if (slot->format == FT_GLYPH_FORMAT_OUTLINE && shift != 0) {
            FT_Outline_Translate(&slot->outline, shift, 0);
        }
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
            continue;
        }

        /* Read AFTER rendering: the translate above moves the bounding box, so
         * bitmap_left already carries the whole-pixel part of the shift and the
         * coverage carries the fraction. */
        raster[i].offsetX = slot->bitmap_left;
        raster[i].offsetY = ascender - slot->bitmap_top;
        ++rasterised;

        const int width = static_cast<int>(slot->bitmap.width);
        const int height = static_cast<int>(slot->bitmap.rows);
        if (width <= 0 || height <= 0) continue;

        raster[i].width = width;
        raster[i].height = height;
        raster[i].coverage.assign(static_cast<std::size_t>(width) * height, 0);
        for (int row = 0; row < height; ++row) {
            /* Row by row: FreeType's pitch is not the width - it pads rows, and
             * it is negative for bottom-up bitmaps. */
            std::memcpy(raster[i].coverage.data() + static_cast<std::size_t>(row) * width,
                        slot->bitmap.buffer + static_cast<std::ptrdiff_t>(row) * slot->bitmap.pitch,
                        static_cast<std::size_t>(width));
        }
    }
    FT_Done_Face(face);

    if (rasterised == 0) return atlas;

    /* ---- shelf packing ----------------------------------------------------
     * Rows of glyphs, each as tall as its tallest member. Not the tightest
     * packing there is and it does not need to be: 95 glyphs of one size, run
     * once. GenImageFontAtlas would do this too, but it takes ownership of the
     * glyph images and reformats them, and keeping the pixels here is what lets
     * the same code path serve any consumer's layout. */
    const int padding = 1;
    int atlasWidth = 128;
    while (atlasWidth < (sizePx + padding * 2) * 12) atlasWidth *= 2;

    int penX = padding;
    int penY = padding;
    int rowHeight = 0;
    for (int i = 0; i < kCodepointCount; ++i) {
        const RasterGlyph& glyph = raster[i];

        /* The metrics land whether or not there are pixels: a space has an
         * advance and no bitmap, and dropping its advance would close up every
         * gap in every label. */
        atlas.glyphs_[i].offsetX  = glyph.offsetX;
        atlas.glyphs_[i].offsetY  = glyph.offsetY;
        atlas.glyphs_[i].advanceX = glyph.advanceX;

        if (glyph.width <= 0) continue;

        if (penX + glyph.width + padding > atlasWidth) {
            penX = padding;
            penY += rowHeight + padding;
            rowHeight = 0;
        }
        atlas.glyphs_[i].x      = penX;
        atlas.glyphs_[i].y      = penY;
        atlas.glyphs_[i].width  = glyph.width;
        atlas.glyphs_[i].height = glyph.height;

        penX += glyph.width + padding;
        rowHeight = std::max(rowHeight, glyph.height);
    }

    int atlasHeight = 16;
    while (atlasHeight < penY + rowHeight + padding) atlasHeight *= 2;

    /* ONE BYTE PER TEXEL, CURVED. Every consumer wants this as the alpha of a
     * text quad; nothing wants it in a colour channel, and putting it there as
     * well would multiply it in twice under a straight-alpha blend and darken
     * every edge. See the header on why this is coverage rather than alpha. */
    atlas.coverage_.assign(static_cast<std::size_t>(atlasWidth) * atlasHeight, 0);

    const unsigned char* curve = coverageCurve();
    for (int i = 0; i < kCodepointCount; ++i) {
        const RasterGlyph& glyph = raster[i];
        if (glyph.width <= 0) continue;

        const int originX = atlas.glyphs_[i].x;
        const int originY = atlas.glyphs_[i].y;
        for (int row = 0; row < glyph.height; ++row) {
            for (int col = 0; col < glyph.width; ++col) {
                const std::size_t destination =
                    (static_cast<std::size_t>(originY + row) * atlasWidth) + originX + col;
                atlas.coverage_[destination] = curve[
                    glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col]];
            }
        }
    }

    atlas.width_   = atlasWidth;
    atlas.height_  = atlasHeight;
    atlas.sizePx_  = sizePx;
    atlas.padding_ = padding;
    atlas.baseline_ = ascender;

    /* THE HINTED CAP TOP, read off the rasterised 'H'. Its offsetY is already
     * (ascender - bitmap_top), which is exactly the distance from the top of the
     * glyph box to the top of a capital — see the header on why this comes from
     * a bitmap rather than from the face's declared cap height.
     *
     * A face with no 'H' falls back to the ascender, which centres on the whole
     * box and is the behaviour this replaced: wrong, but not broken. */
    const int capitalH = GlyphAtlas::indexOf('H');
    atlas.capTop_ = raster[capitalH].height > 0 ? raster[capitalH].offsetY : 0;
    return atlas;
}

}  // namespace cromwell::ui
