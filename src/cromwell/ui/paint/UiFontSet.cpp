#include "cromwell/ui/paint/UiFontSet.hpp"

#include "cromwell/diag/Logger.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

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
 * Four phases, at quarter-pixel steps. Godot's "one quarter of a pixel" mode,
 * which its own docs call the best-quality setting; it also offers halves, and
 * turns the whole thing off at large sizes where a quarter of a pixel stops
 * being visible. The cost is four atlases per weight and size instead of one -
 * a few hundred KB for 95 glyphs, against the ~3 MB Godot measured for its
 * editor with full Chinese localisation.
 *
 * WHY LIGHT HINTING AND NOT FULL. They are not independent choices. Full
 * hinting snaps the outline on BOTH axes, which pulls every glyph back onto
 * the pixel grid horizontally - and a glyph forced onto the grid cannot be
 * positioned at a quarter of a pixel, so the four phases would be four copies
 * of the same bitmap. Light snaps the Y axis only and leaves X free, which is
 * exactly what makes horizontal subpixel positioning mean anything. Godot
 * pairs them the same way.
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

/* raylib's default set: printable ASCII, 32 through 126. */
constexpr int kFirstCodepoint = 32;
constexpr int kCodepointCount = 95;

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
 * built once and drawn thousands of times, and because it keeps the UI text
 * path on the default shader - no second pipeline state for one curve. The
 * cost is that it is tuned for a dark UI; a light-on-white UI would want the
 * inverse, and would want it as a bake parameter rather than a constant. */
constexpr float kCoverageGamma = 1.45f;

/* 256-entry table, built once per bake rather than a pow() per texel. */
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

/* Rasterises `path` at `sizePx`, with every outline shifted right by
 * `phase`/kPhaseCount of a pixel before rendering. Returns a Font with texture
 * id 0 on failure, which is what every caller here already tests for. */
Font loadFontFreeType(const std::string& path, int sizePx, int phase)
{
    FT_Library library = ftLibrary();
    if (library == nullptr) return Font{};

    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0) return Font{};

    /* Width 0 means "square pixels, take it from the height". */
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(sizePx)) != 0) {
        FT_Done_Face(face);
        return Font{};
    }

    /* The shift, in 26.6 fixed point: a whole pixel is 64, so a quarter is 16. */
    const FT_Pos shift = static_cast<FT_Pos>(phase) * (64 / UiFontSet::kPhaseCount);

    /* Where the baseline sits below the top of the line box. FreeType reports
     * bitmap_top relative to the BASELINE and the painter positions from the
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

    if (rasterised == 0) return Font{};

    /* ---- shelf packing ----------------------------------------------------
     * Rows of glyphs, each as tall as its tallest member. Not the tightest
     * packing there is and it does not need to be: 95 glyphs of one size, run
     * once. GenImageFontAtlas would do this too, but it takes ownership of the
     * glyph images and reformats them, and keeping the pixels here is what lets
     * the same code path serve any coverage layout. */
    const int padding = 1;
    int atlasWidth = 128;
    while (atlasWidth < (sizePx + padding * 2) * 12) atlasWidth *= 2;

    int penX = padding;
    int penY = padding;
    int rowHeight = 0;
    std::vector<Rectangle> placed(kCodepointCount);
    for (int i = 0; i < kCodepointCount; ++i) {
        const RasterGlyph& glyph = raster[i];
        if (glyph.width <= 0) {
            placed[i] = Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
            continue;
        }
        if (penX + glyph.width + padding > atlasWidth) {
            penX = padding;
            penY += rowHeight + padding;
            rowHeight = 0;
        }
        placed[i] = Rectangle{ static_cast<float>(penX), static_cast<float>(penY),
                               static_cast<float>(glyph.width),
                               static_cast<float>(glyph.height) };
        penX += glyph.width + padding;
        rowHeight = std::max(rowHeight, glyph.height);
    }

    int atlasHeight = 16;
    while (atlasHeight < penY + rowHeight + padding) atlasHeight *= 2;

    /* GRAY_ALPHA with the grey pinned at 255 and the coverage in alpha, which
     * is raylib's own font convention (see GenImageFontAtlas). It matters:
     * putting the coverage in the colour channels as well would multiply it in
     * twice under a straight-alpha blend and darken every edge. */
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(atlasWidth) * atlasHeight * 2, 0);
    for (std::size_t i = 0; i < pixels.size(); i += 2) pixels[i] = 255;

    const unsigned char* curve = coverageCurve();
    for (int i = 0; i < kCodepointCount; ++i) {
        const RasterGlyph& glyph = raster[i];
        if (glyph.width <= 0) continue;
        const int originX = static_cast<int>(placed[i].x);
        const int originY = static_cast<int>(placed[i].y);
        for (int row = 0; row < glyph.height; ++row) {
            for (int col = 0; col < glyph.width; ++col) {
                const std::size_t destination =
                    ((static_cast<std::size_t>(originY + row) * atlasWidth)
                     + originX + col) * 2;
                pixels[destination + 1] = curve[
                    glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col]];
            }
        }
    }

    const Image atlas{ pixels.data(), atlasWidth, atlasHeight, 1,
                       PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA };

    Font font{};
    font.baseSize = sizePx;
    font.glyphCount = kCodepointCount;
    font.glyphPadding = padding;
    font.texture = LoadTextureFromImage(atlas);   /* copies; pixels can die */
    if (font.texture.id == 0) return Font{};

    /* MemAlloc, not new: UnloadFont frees these with raylib's allocator and the
     * two have to be the same one. The glyph images are left null on purpose -
     * the pixels live in the atlas now, and UnloadFontData free()s a null
     * pointer harmlessly. */
    font.recs = static_cast<Rectangle*>(
        MemAlloc(static_cast<unsigned int>(sizeof(Rectangle) * kCodepointCount)));
    font.glyphs = static_cast<GlyphInfo*>(
        MemAlloc(static_cast<unsigned int>(sizeof(GlyphInfo) * kCodepointCount)));
    for (int i = 0; i < kCodepointCount; ++i) {
        font.recs[i] = placed[i];
        font.glyphs[i] = GlyphInfo{};
        font.glyphs[i].value = kFirstCodepoint + i;
        font.glyphs[i].advanceX = raster[i].advanceX;
        font.glyphs[i].offsetX = raster[i].offsetX;
        font.glyphs[i].offsetY = raster[i].offsetY;
    }
    return font;
}

}  // namespace

int UiFontSet::weightIndex(FontWeight weight)
{
    switch (weight) {
    case FontWeight::Regular:   return 0;
    case FontWeight::Medium:    return 1;
    case FontWeight::SemiBold:  return 2;
    case FontWeight::Bold:      return 3;
    case FontWeight::ExtraBold: return 4;
    }
    return 0;
}

UiFontSet::~UiFontSet()
{
    unload();
}

float UiFontSet::rasterSize(float sizePx)
{
    return static_cast<float>(atlasSizeFor(sizePx));
}

int UiFontSet::atlasSizeFor(float sizePx)
{
    const int rounded = static_cast<int>(sizePx + 0.5f);
    if (rounded < kMinSizePx) return kMinSizePx;
    if (rounded > kMaxSizePx) return kMaxSizePx;
    return rounded;
}

std::uint32_t UiFontSet::cacheKey(int weightIndex, int sizePx, int phase)
{
    /* Weight, size and phase in one integer. Two bits for the phase because
     * there are four of them; the size is bounded by kMaxSizePx so ten bits is
     * ample and the weight sits above both. */
    return (static_cast<std::uint32_t>(weightIndex) << 12)
         | (static_cast<std::uint32_t>(sizePx) << 2)
         | static_cast<std::uint32_t>(phase & (kPhaseCount - 1));
}

bool UiFontSet::loadWeight(FontWeight weight, const std::string& path)
{
    if (!FileExists(path.c_str())) {
        LOGGER->warn("ui: font not found, falling back: {}", path);
        return false;
    }

    const int index = weightIndex(weight);
    paths_[index] = path;
    loaded_[index] = true;

    /* Rasterise one size now, purely to find out whether the file is a font.
     * 16 px because it is the kit's commonest style size, so the atlas this
     * validation produces is one the UI is about to want anyway rather than
     * work thrown away.
     *
     * Success is tested by asking whether the atlas ENTERED THE CACHE, not by
     * looking at the returned font: fontFor falls back to raylib's built-in on
     * failure, and that font has a perfectly valid texture id, so inspecting
     * the return value would call every broken file a success. */
    fontFor(weight, 16.0f, 0);
    if (atlases_.find(cacheKey(index, atlasSizeFor(16.0f), 0)) == atlases_.end()) {
        LOGGER->warn("ui: font failed to rasterise: {}", path);
        paths_[index].clear();
        loaded_[index] = false;
        return false;
    }
    return true;
}

void UiFontSet::unload()
{
    for (auto& entry : atlases_) {
        UnloadFont(entry.second);
    }
    atlases_.clear();

    for (int index = 0; index < kWeightCount; ++index) {
        paths_[index].clear();
        loaded_[index] = false;
    }
}

const Font& UiFontSet::fontFor(FontWeight weight, float sizePx, int phase) const
{
    /* Weight falls back before size does: a missing Bold should be drawn in
     * Regular at the right size, never in Bold at the wrong one. */
    int index = weightIndex(weight);
    if (!loaded_[index]) index = 0;

    if (loaded_[index]) {
        const int size = atlasSizeFor(sizePx);
        const int wrapped = phase & (kPhaseCount - 1);
        const std::uint32_t key = cacheKey(index, size, wrapped);

        const auto existing = atlases_.find(key);
        if (existing != atlases_.end()) return existing->second;

        const Font font = loadFontFreeType(paths_[index], size, wrapped);
        if (font.texture.id != 0 && font.glyphCount != 0) {
            /* POINT, and it has to be. Every glyph is rasterised at the exact
             * size it is drawn at and placed on a whole texel - the fractional
             * part of its position is baked into the coverage by the phase, not
             * applied at sample time. So each texel maps to one screen pixel
             * and a filter has nothing legitimate to interpolate; bilinear
             * would merely reintroduce, at sample time, the blur the phases
             * exist to remove. */
            SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
            return atlases_.emplace(key, font).first->second;
        }

        /* Rasterisation failed at this size only — fall through to the built-in
         * rather than caching a broken atlas, so the next size still tries. */
        if (font.texture.id != 0) UnloadFont(font);
    }

    /* raylib's built-in. Static because GetFontDefault returns by value and the
     * interface hands back a reference; one copy for the process is fine, it is
     * a handle to a texture raylib owns. */
    static const Font fallback = GetFontDefault();
    return fallback;
}

Vec2 UiFontSet::measure(std::string_view text, const TextStyle& style) const
{
    const float height = lineHeight(style);
    if (text.empty()) {
        /* Zero width, full line height: an empty label keeps its row rather
         * than collapsing it. */
        return { 0.0f, height };
    }

    /* raylib's API is null-terminated, and a string_view need not be. Cold code
     * — see the note on the UI's cost in UiContext.hpp — so the copy is not
     * worth a scratch-buffer dance. */
    const std::string terminated(text);
    /* Measured at the size it will be DRAWN at, which is the rasterised one.
     * Measuring at the style's fractional size and drawing at the integer one
     * would put the layout and the glyphs a fraction of a pixel out of step,
     * and a centred label would sit very slightly off centre. */
    const Vector2 measured = MeasureTextEx(fontFor(style.weight, style.sizePx, 0), terminated.c_str(),
                                           rasterSize(style.sizePx), style.letterSpacingPx);
    return { measured.x, height };
}

float UiFontSet::lineHeight(const TextStyle& style) const
{
    return style.sizePx * kLineHeightFactor;
}

}  // namespace cromwell::ui
