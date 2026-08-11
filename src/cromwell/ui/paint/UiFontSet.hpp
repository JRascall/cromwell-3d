/* UiFontSet.hpp — the UI's typefaces, and the answer to "how big is this".
 *
 * SINGLE RESPONSIBILITY: own one rasterised font per weight, and implement the
 * measurement interface the headless widgets lay out against.
 *
 * THIS IS THE SEAM. Everything above it — every widget in the kit — is
 * arithmetic over the numbers this returns, and none of it links raylib. This
 * file is where a weight becomes a file on disk and a string becomes a width in
 * pixels, and it is the only place in the UI that knows either.
 *
 * WEIGHTS DEGRADE, THEY DO NOT FAIL. A weight that was never given a file falls
 * back to Regular, and a font set given no files at all falls back to raylib's
 * built-in font. A UI missing its Bold should look slightly wrong; it should
 * never fail to draw, because the alternative is a menu that vanishes on a
 * machine where an asset did not ship.
 *
 * ONE ATLAS PER (WEIGHT, SIZE), RASTERISED ON DEMAND. Text is never sampled at
 * a size it was not rasterised at, because that is the only way to be crisp and
 * there is no version of "scale a glyph atlas" that is.
 *
 * This replaces a single 48 px atlas filtered bilinearly, and the reason is
 * worth keeping: the kit's styles are 10 to 16 px, so that atlas was being
 * MINIFIED three to five times. Bilinear takes four texels, so a 4x reduction
 * throws away roughly fifteen of every sixteen, with no mipmap to have averaged
 * them first — stems land on or between texel centres depending on the
 * fractional position, so a stroke's darkness changes as text moves, and 'l'
 * and 'i' come out different weights in the same word. Magnifying a small atlas
 * is merely blurry; minifying a large one aliases, which reads as text that
 * shimmers rather than text that is soft. Neither is what "crisp" means.
 *
 * THE COST IS AN ATLAS PER DISTINCT SIZE, which is bounded by how many sizes
 * the styles actually name — five or six across the whole kit, plus however
 * many a display scale multiplies them into. Each is a few hundred KB. A cache
 * miss rasterises inside a draw call, so a size appearing for the first time
 * costs a hitch on that frame and nothing afterwards.
 *
 * SIZES ARE DEVICE PIXELS BY THE TIME THEY GET HERE. This deliberately does not
 * know about DPI or a UI zoom: a scale that only reached the font would give
 * bigger text in boxes that stayed the same size. Whatever multiplies a layout
 * multiplies TextStyle::sizePx on the way in, and this rasterises whatever it
 * is handed.
 *
 * LINE HEIGHT IS THE LINE BOX, NOT THE FONT SIZE. raylib's MeasureTextEx
 * reports the point size as the height, which is not what a paragraph advances
 * by and not what a label should be centred within. Everything here reports
 * `sizePx * kLineHeightFactor` instead, consistently, so measuring and wrapping
 * and centring all agree.
 */
#pragma once

#include "cromwell/ui/core/UiText.hpp"

#include "raylib.h"

#include <cstdint>
#include <map>
#include <string>

namespace cromwell::ui {

class UiFontSet : public TextMetrics {
public:
    /* Leading. 1.25 is the usual UI value — tighter reads as cramped in a
     * paragraph, looser wastes a tip card's height. */
    static constexpr float kLineHeightFactor = 1.25f;

    /* Rasterisation is clamped to this. Not a style limit — it is a guard on
     * the cache, so a layout bug that computes a 40,000 px heading asks for a
     * texture the driver refuses rather than one it agrees to allocate. */
    static constexpr int kMinSizePx = 4;
    static constexpr int kMaxSizePx = 256;

    UiFontSet() = default;
    ~UiFontSet();

    UiFontSet(const UiFontSet&) = delete;
    UiFontSet& operator=(const UiFontSet&) = delete;

    /* Registers `path` as the file for `weight`. Returns false and leaves the
     * weight falling back if the file is missing or will not rasterise —
     * callers can log it, but nothing here treats it as fatal.
     *
     * It rasterises one atlas immediately rather than only recording the path,
     * so an unreadable font is reported HERE, at load, and not as a silently
     * missing glyph the first time some panel happens to open.
     *
     * REQUIRES A GL CONTEXT: it uploads an atlas texture. Call it after the
     * window is open. */
    bool loadWeight(FontWeight weight, const std::string& path);

    /* Releases every atlas at every size. Safe to call twice and safe to call
     * on a set that loaded nothing. */
    void unload();

    /* The atlas for this weight AT THIS SIZE — rasterising it if this is the
     * first time that pair has been asked for. Falls back a weight at a time:
     * the requested weight, else Regular, else raylib's built-in font.
     *
     * For the painter; widgets never see this. `sizePx` must be the size the
     * text will actually be drawn at, because that is the entire point.
     *
     * const, and it mutates the cache, because "which atlas" is a question
     * measure() has to ask too and neither caller is conceptually writing to
     * the font set. */
    const Font& fontFor(FontWeight weight, float sizePx) const;

    /* ---- TextMetrics --------------------------------------------------- */
    Vec2  measure(std::string_view text, const TextStyle& style) const override;
    float lineHeight(const TextStyle& style) const override;

private:
    static constexpr int kWeightCount = 5;
    static int weightIndex(FontWeight weight);

    /* Rounds and clamps a style size to the integer the rasteriser takes, so
     * every path that keys the cache rounds identically. */
    static int atlasSizeFor(float sizePx);

    /* (weight, size) packed into one integer key. A map rather than a hash: the
     * whole UI produces a few dozen text runs a frame, so this is looked up
     * tens of times per frame against a container holding single digits of
     * entries — the tree walk is two or three comparisons and it never
     * rehashes. This is cold code by the standard in UiContext.hpp. */
    static std::uint32_t cacheKey(int weightIndex, int sizePx);

    /* The file behind each weight, empty when it never loaded. */
    std::string paths_[kWeightCount]{};
    bool        loaded_[kWeightCount]{};

    /* mutable because fontFor is const — see its comment. */
    mutable std::map<std::uint32_t, Font> atlases_;
};

}  // namespace cromwell::ui
