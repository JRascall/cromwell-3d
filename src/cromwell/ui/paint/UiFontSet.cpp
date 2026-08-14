#include "cromwell/ui/paint/UiFontSet.hpp"

#include "cromwell/diag/Logger.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace cromwell::ui {

namespace {

/* ---- a raylib Font over an already-baked atlas ---------------------------
 *
 * THE UPLOAD, AND NOTHING ELSE. Every decision about the type — hinting, the
 * phase shift, the shelf pack, the coverage curve — was made in GlyphAtlas and
 * is not revisited here. This function's whole job is to put those bytes into
 * a texture raylib's DrawTextEx can sample and to restate the metrics in
 * raylib's structs.
 *
 * Returns a Font with texture id 0 on failure, which is what the caller here
 * already tests for. */
Font uploadRaylibFont(const GlyphAtlas& atlas)
{
    if (!atlas.valid()) return Font{};

    /* GRAY_ALPHA WITH THE GREY PINNED AT 255 and the coverage in alpha, which
     * is raylib's own font convention (see GenImageFontAtlas). It matters:
     * putting the coverage in the colour channels as well would multiply it in
     * twice under a straight-alpha blend and darken every edge.
     *
     * The atlas stores one channel because that is what the coverage IS; this
     * expansion is a fact about raylib's blending, which is why it lives at
     * this consumer rather than in the bake. The device painter uploads the
     * same bytes as R8 and never pays for the second channel. */
    const std::vector<std::uint8_t>& coverage = atlas.coverage();
    std::vector<unsigned char> pixels(coverage.size() * 2, 255);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        pixels[i * 2 + 1] = coverage[i];
    }

    const Image image{ pixels.data(), atlas.width(), atlas.height(), 1,
                       PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA };

    Font font{};
    font.baseSize     = atlas.sizePx();
    font.glyphCount   = GlyphAtlas::kCodepointCount;
    font.glyphPadding = atlas.padding();
    font.texture      = LoadTextureFromImage(image);   /* copies; pixels can die */
    if (font.texture.id == 0) return Font{};

    /* POINT, and it has to be. Every glyph is rasterised at the exact size it
     * is drawn at and placed on a whole texel - the fractional part of its
     * position is baked into the coverage by the phase, not applied at sample
     * time. So each texel maps to one screen pixel and a filter has nothing
     * legitimate to interpolate; bilinear would merely reintroduce, at sample
     * time, the blur the phases exist to remove. */
    SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);

    /* MemAlloc, not new: UnloadFont frees these with raylib's allocator and the
     * two have to be the same one. The glyph images are left null on purpose -
     * the pixels live in the atlas now, and UnloadFontData free()s a null
     * pointer harmlessly. */
    font.recs = static_cast<Rectangle*>(
        MemAlloc(static_cast<unsigned int>(sizeof(Rectangle) * GlyphAtlas::kCodepointCount)));
    font.glyphs = static_cast<GlyphInfo*>(
        MemAlloc(static_cast<unsigned int>(sizeof(GlyphInfo) * GlyphAtlas::kCodepointCount)));

    for (int i = 0; i < GlyphAtlas::kCodepointCount; ++i) {
        const GlyphAtlas::Glyph& glyph = atlas.glyph(i);

        font.recs[i] = Rectangle{ static_cast<float>(glyph.x), static_cast<float>(glyph.y),
                                  static_cast<float>(glyph.width),
                                  static_cast<float>(glyph.height) };

        font.glyphs[i] = GlyphInfo{};
        font.glyphs[i].value    = GlyphAtlas::kFirstCodepoint + i;
        font.glyphs[i].advanceX = glyph.advanceX;
        font.glyphs[i].offsetX  = glyph.offsetX;
        font.glyphs[i].offsetY  = glyph.offsetY;
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
     * there is room for four of them; the size is bounded by kMaxSizePx so ten
     * bits is ample and the weight sits above both. */
    return (static_cast<std::uint32_t>(weightIndex) << 12)
         | (static_cast<std::uint32_t>(sizePx) << 2)
         | static_cast<std::uint32_t>(phase & (kPhaseCount - 1));
}

bool UiFontSet::resolve(FontWeight weight, float sizePx, int phase,
                        int& outIndex, int& outSize, int& outPhase) const
{
    /* Weight falls back before size does: a missing Bold should be drawn in
     * Regular at the right size, never in Bold at the wrong one. */
    int index = weightIndex(weight);
    if (!loaded_[index]) index = 0;

    outIndex = index;
    outSize  = atlasSizeFor(sizePx);
    outPhase = phase & (kPhaseCount - 1);
    return loaded_[index];
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
    ++generation_;

    /* Rasterise one size now, purely to find out whether the file is a font.
     * 16 px because it is the kit's commonest style size, so the atlas this
     * validation produces is one the UI is about to want anyway rather than
     * work thrown away. */
    if (atlasFor(weight, 16.0f, 0) == nullptr) {
        LOGGER->warn("ui: font failed to rasterise: {}", path);
        paths_[index].clear();
        loaded_[index] = false;
        return false;
    }
    return true;
}

void UiFontSet::unload()
{
    for (auto& entry : fonts_) {
        UnloadFont(entry.second);
    }
    fonts_.clear();
    atlases_.clear();
    ++generation_;

    for (int index = 0; index < kWeightCount; ++index) {
        paths_[index].clear();
        loaded_[index] = false;
    }
}

const GlyphAtlas* UiFontSet::atlasFor(FontWeight weight, float sizePx, int phase) const
{
    int index = 0;
    int size = 0;
    int wrapped = 0;
    if (!resolve(weight, sizePx, phase, index, size, wrapped)) return nullptr;

    const std::uint32_t key = cacheKey(index, size, wrapped);

    const auto existing = atlases_.find(key);
    if (existing != atlases_.end()) return &existing->second;

    GlyphAtlas atlas = GlyphAtlas::bake(paths_[index], size, wrapped, kPhaseCount);

    /* A FAILURE AT THIS SIZE IS NOT CACHED, so the next size still tries. The
     * usual cause is a size the face has no hinting program for rather than a
     * broken file, and one bad size should not condemn the weight. */
    if (!atlas.valid()) return nullptr;

    return &atlases_.emplace(key, std::move(atlas)).first->second;
}

const Font& UiFontSet::fontFor(FontWeight weight, float sizePx, int phase) const
{
    const GlyphAtlas* atlas = atlasFor(weight, sizePx, phase);
    if (atlas != nullptr) {
        int index = 0;
        int size = 0;
        int wrapped = 0;
        resolve(weight, sizePx, phase, index, size, wrapped);
        const std::uint32_t key = cacheKey(index, size, wrapped);

        const auto existing = fonts_.find(key);
        if (existing != fonts_.end()) return existing->second;

        const Font font = uploadRaylibFont(*atlas);
        if (font.texture.id != 0) {
            return fonts_.emplace(key, font).first->second;
        }
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

    /* PHASE 0. Advances are identical across phases by construction — the phase
     * shifts where a glyph is drawn, never how far the pen moves — so a
     * measurement that picked one would be picking arbitrarily. */
    const GlyphAtlas* atlas = atlasFor(style.weight, style.sizePx, 0);

    if (atlas == nullptr) {
        /* NO ATLAS AT ALL, which means no weight loaded. Measured against
         * raylib's built-in font so a fontless UI still lays out plausibly
         * rather than stacking every label at zero width — the same
         * degradation fontFor makes, kept deliberately in step with it. */
        const std::string terminated(text);
        const Vector2 measured = MeasureTextEx(fontFor(style.weight, style.sizePx, 0),
                                               terminated.c_str(),
                                               rasterSize(style.sizePx), style.letterSpacingPx);
        return { measured.x, height };
    }

    /* SUMMED FROM THE ATLAS'S OWN METRICS, which is what both painters advance
     * the pen by. It reproduces raylib's MeasureTextEx for everything the kit
     * produces, and it is here rather than there so that measuring does not
     * need a font uploaded — a console build has no MeasureTextEx and the
     * layout above this must still come out at the same numbers.
     *
     * NO SCALE FACTOR, because there is never one to apply: the atlas is
     * rasterised at atlasSizeFor(sizePx) and drawn at rasterSize(sizePx),
     * which are the same integer by construction. A factor here would be a
     * multiply by one that a reader has to verify.
     *
     * THE WIDEST LINE WINS and the spacing term follows the widest line's
     * character count, which is MeasureTextEx's rule. Multi-line runs are rare
     * — TextMetrics::wrap splits before a run is built — but a caption with a
     * newline in it should not measure as though the two halves were one. */
    float widest = 0.0f;
    float lineWidth = 0.0f;
    int   widestChars = 0;
    int   lineChars = 0;

    const auto endLine = [&] {
        widest = std::max(widest, lineWidth);
        widestChars = std::max(widestChars, lineChars);
        lineWidth = 0.0f;
        lineChars = 0;
    };

    for (const char character : text) {
        if (character == '\n') {
            endLine();
            continue;
        }

        const GlyphAtlas::Glyph& glyph =
            atlas->glyph(GlyphAtlas::indexOf(static_cast<unsigned char>(character)));

        /* The advance, or the bitmap's extent when a face gives none — raylib's
         * fallback, kept because a glyph with a zero advance would otherwise
         * measure as occupying nothing while still drawing pixels. */
        lineWidth += glyph.advanceX > 0
            ? static_cast<float>(glyph.advanceX)
            : static_cast<float>(glyph.width + glyph.offsetX);
        ++lineChars;
    }
    endLine();

    const float spacing = widestChars > 1
        ? static_cast<float>(widestChars - 1) * style.letterSpacingPx : 0.0f;

    return { widest + spacing, height };
}

float UiFontSet::lineHeight(const TextStyle& style) const
{
    return style.sizePx * kLineHeightFactor;
}

}  // namespace cromwell::ui
