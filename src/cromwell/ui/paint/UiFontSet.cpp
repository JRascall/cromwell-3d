#include "cromwell/ui/paint/UiFontSet.hpp"

#include "cromwell/diag/Logger.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstddef>
#include <cstring>

namespace cromwell::ui {

namespace {

/* ---- the FreeType rasteriser -------------------------------------------
 *
 * WHY NOT raylib's LoadFontEx, which does all of this in one call. Because it
 * rasterises with stb_truetype, which does not hint. At 10 to 16 px a stem is
 * one or two pixels wide, so an unhinted outline lands it as one dark pixel or
 * two grey ones depending on where the curve happens to fall - the same letter
 * comes out a different weight in different words. FreeType's LIGHT target
 * snaps stems and baselines to the pixel grid vertically and leaves horizontal
 * metrics alone, which is what evens the weight out without making the letter
 * spacing lumpy.
 *
 * WHAT IS STILL raylib's: the atlas packing. GenImageFontAtlas takes exactly
 * the GlyphInfo array built below, packs it, and allocates the rectangles with
 * the allocator UnloadFont later frees them with. Replacing the rasteriser is
 * a real improvement; replacing a working rectangle packer would just be a
 * second one to own.
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

/* raylib's default set: printable ASCII, 32 through 126. Matched exactly rather
 * than chosen, because measure() and the painter both go through MeasureTextEx
 * and DrawTextEx, which look glyphs up by codepoint and draw the missing-glyph
 * box for anything absent. The kit's chevrons are outside it - documented on
 * SettingStepperSpec rather than papered over by widening the set, which would
 * cost atlas space on every weight AND every size. */
constexpr int kFirstCodepoint = 32;
constexpr int kCodepointCount = 95;

/* Fills `glyphs` from `face` at the size already set on it. Returns false only
 * if nothing rasterised at all; one missing glyph is left as a blank cell,
 * because a font without a tilde should not cost the UI its text. */
bool rasteriseGlyphs(FT_Face face, GlyphInfo* glyphs)
{
    /* Where the baseline sits below the top of the line box, in pixels.
     * FreeType reports bitmap_top relative to the BASELINE and raylib wants
     * offsetY relative to the TOP, so every glyph is offset by this. The
     * metrics are 26.6 fixed point, hence the shift. */
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);

    int rasterised = 0;
    for (int i = 0; i < kCodepointCount; ++i) {
        const int codepoint = kFirstCodepoint + i;
        glyphs[i] = GlyphInfo{};
        glyphs[i].value = codepoint;

        /* LIGHT is the hinting target and the whole reason FreeType is here -
         * see the note above. RENDER asks for the 8-bit coverage bitmap in the
         * same call. */
        if (FT_Load_Char(face, static_cast<FT_ULong>(codepoint),
                         FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
            continue;
        }

        const FT_GlyphSlot slot = face->glyph;
        glyphs[i].advanceX = static_cast<int>(slot->advance.x >> 6);
        glyphs[i].offsetX  = slot->bitmap_left;
        glyphs[i].offsetY  = ascender - slot->bitmap_top;
        ++rasterised;

        const int width  = static_cast<int>(slot->bitmap.width);
        const int height = static_cast<int>(slot->bitmap.rows);

        /* MemAlloc rather than new or malloc, here and below: UnloadFont frees
         * this with raylib's allocator and the two have to be the same one. */
        if (width <= 0 || height <= 0) {
            /* Space and friends carry no coverage, but the packer still wants
             * a valid image it can own and free. */
            auto* blank = static_cast<unsigned char*>(MemAlloc(1));
            blank[0] = 0;
            glyphs[i].image.data = blank;
            glyphs[i].image.width = 1;
            glyphs[i].image.height = 1;
            glyphs[i].image.mipmaps = 1;
            glyphs[i].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
            continue;
        }

        auto* pixels = static_cast<unsigned char*>(
            MemAlloc(static_cast<unsigned int>(width * height)));

        /* Row by row, because FreeType's pitch is not the width - it pads rows,
         * and it is negative for bottom-up bitmaps. */
        for (int row = 0; row < height; ++row) {
            const unsigned char* source =
                slot->bitmap.buffer + static_cast<std::ptrdiff_t>(row) * slot->bitmap.pitch;
            std::memcpy(pixels + static_cast<std::ptrdiff_t>(row) * width, source,
                        static_cast<std::size_t>(width));
        }

        glyphs[i].image.data = pixels;
        glyphs[i].image.width = width;
        glyphs[i].image.height = height;
        glyphs[i].image.mipmaps = 1;
        glyphs[i].image.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
    }
    return rasterised > 0;
}

/* Rasterises `path` at `sizePx`. Returns a Font with texture id 0 on any
 * failure, which is what every caller here already tests for. */
Font loadFontFreeType(const std::string& path, int sizePx)
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

    auto* glyphs = static_cast<GlyphInfo*>(
        MemAlloc(static_cast<unsigned int>(sizeof(GlyphInfo) * kCodepointCount)));
    const bool ok = rasteriseGlyphs(face, glyphs);
    FT_Done_Face(face);

    if (!ok) {
        UnloadFontData(glyphs, kCodepointCount);
        return Font{};
    }

    Font font{};
    font.baseSize = sizePx;
    font.glyphCount = kCodepointCount;
    font.glyphPadding = 1;
    font.glyphs = glyphs;

    /* Skyline packing rather than raylib's default rows: glyph heights vary a
     * lot at one size - a comma against a capital - and rows waste the
     * difference. Cold code, so the extra packing work is free. */
    const Image atlas = GenImageFontAtlas(font.glyphs, &font.recs, font.glyphCount,
                                          font.baseSize, font.glyphPadding, 1);
    font.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);

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

int UiFontSet::atlasSizeFor(float sizePx)
{
    const int rounded = static_cast<int>(sizePx + 0.5f);
    if (rounded < kMinSizePx) return kMinSizePx;
    if (rounded > kMaxSizePx) return kMaxSizePx;
    return rounded;
}

std::uint32_t UiFontSet::cacheKey(int weightIndex, int sizePx)
{
    return (static_cast<std::uint32_t>(weightIndex) << 16)
         | static_cast<std::uint32_t>(sizePx);
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
    fontFor(weight, 16.0f);
    if (atlases_.find(cacheKey(index, atlasSizeFor(16.0f))) == atlases_.end()) {
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

const Font& UiFontSet::fontFor(FontWeight weight, float sizePx) const
{
    /* Weight falls back before size does: a missing Bold should be drawn in
     * Regular at the right size, never in Bold at the wrong one. */
    int index = weightIndex(weight);
    if (!loaded_[index]) index = 0;

    if (loaded_[index]) {
        const int size = atlasSizeFor(sizePx);
        const std::uint32_t key = cacheKey(index, size);

        const auto existing = atlases_.find(key);
        if (existing != atlases_.end()) return existing->second;

        const Font font = loadFontFreeType(paths_[index], size);
        if (font.texture.id != 0 && font.glyphCount != 0) {
            /* Bilinear even though the atlas is now drawn at its own size:
             * layout positions are floats, so a glyph often lands on a
             * fractional pixel. Point sampling would snap each one to the
             * nearest texel and make letter spacing visibly uneven within a
             * word. At 1:1 bilinear costs almost nothing and stays even. */
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
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
    const Vector2 measured = MeasureTextEx(fontFor(style.weight, style.sizePx), terminated.c_str(),
                                           style.sizePx, style.letterSpacingPx);
    return { measured.x, height };
}

float UiFontSet::lineHeight(const TextStyle& style) const
{
    return style.sizePx * kLineHeightFactor;
}

}  // namespace cromwell::ui
