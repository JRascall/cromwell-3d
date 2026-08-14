/* UiFontSet.hpp — the UI's typefaces, and the answer to "how big is this".
 *
 * SINGLE RESPONSIBILITY: own one rasterised atlas per (weight, size, phase),
 * and implement the measurement interface the headless widgets lay out against.
 *
 * THIS IS THE SEAM. Everything above it — every widget in the kit — is
 * arithmetic over the numbers this returns, and none of it links raylib. This
 * file is where a weight becomes a file on disk and a string becomes a width in
 * pixels, and it is the only place in the UI that knows either.
 *
 * WEIGHTS DEGRADE, THEY DO NOT FAIL. A weight that was never given a file falls
 * back to Regular, and a font set given no files at all measures as empty and
 * draws through raylib's built-in font. A UI missing its Bold should look
 * slightly wrong; it should never fail to draw, because the alternative is a
 * menu that vanishes on a machine where an asset did not ship.
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
 * ================= THE BAKE IS NEUTRAL, THE UPLOAD IS NOT =================
 *
 * A cache entry is a GlyphAtlas — coverage bytes and metrics, no API — because
 * there are two painters and text is the only thing in the UI that samples a
 * texture. See GlyphAtlas.hpp for why the split falls exactly there.
 *
 * The raylib Font is built FROM one, lazily, and only when something asks for
 * it. That ordering matters more than it looks: it means loadWeight validates a
 * font file WITHOUT a GL context, and it means the device path — which asks
 * only for atlases — never creates a raylib texture at all.
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
#include "cromwell/ui/paint/GlyphAtlas.hpp"

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

    /* Horizontal subpixel positions per pixel. Must be a power of two - the
     * cache key masks with it - and ONE means every glyph lands on a whole
     * pixel, which is what the painter's phase arithmetic degrades to on its
     * own.
     *
     * ONE, NOT FOUR, AND THE REASON IS THE HINTING. Four gives quarter-pixel
     * placement, which is Godot's default and buys exact letter spacing. But it
     * only works with hinting off or light: native hinting snaps stems onto the
     * pixel grid, and then shifting the hinted outline by a quarter of a pixel
     * throws that away again. The two features are alternatives, not additions.
     *
     * We take the hinting. It is what Dear ImGui and Source both do - hint
     * hard, place on whole pixels - and on this project's own text it is
     * visibly crisper at the 10 to 16 px the kit lives at. The cost is that
     * fractional letter spacing quantises: 2.4 px of tracking becomes
     * alternating 2s and 3s. Raise this to 4 and turn hinting off in
     * GlyphAtlas::bake to swap back to Godot's trade. */
    static constexpr int kPhaseCount = 1;

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
     * NO GL CONTEXT REQUIRED, unlike the version of this that uploaded as it
     * baked. Nothing about validating a font file is a graphics operation, and
     * a call that quietly needed a window was a load-order constraint nobody
     * could see from the signature. */
    bool loadWeight(FontWeight weight, const std::string& path);

    /* Releases every atlas at every size, and every texture built from one.
     * Safe to call twice and safe to call on a set that loaded nothing. */
    void unload();

    /* Bumped by anything that invalidates the atlases — a load, an unload.
     *
     * FOR A PAINTER CACHING GPU RESOURCES AGAINST THEM. DeviceUiPainter keys
     * its glyph textures by atlas address, which is stable while the cache is,
     * and is emphatically not stable across an unload that frees the nodes and
     * a reload that allocates new ones at the same addresses. Comparing this
     * first turns "a label drawn in the wrong typeface after a font reload"
     * into a cache miss. */
    std::uint32_t generation() const { return generation_; }

    /* The integer pixel size the atlas backing this style is rasterised at.
     *
     * PUBLIC BECAUSE THE PAINTERS HAVE TO DRAW AT IT. A glyph rasterised for
     * 13 px and drawn at 13.5 is resampled, and resampling is the one thing
     * per-size atlases exist to avoid. Style sizes are fractional the moment a
     * display scale multiplies them, so the two numbers cannot be assumed
     * equal. */
    static float rasterSize(float sizePx);

    /* THE ATLAS for this weight AT THIS SIZE — rasterising it if this is the
     * first time that pair has been asked for. Null when nothing could be
     * rasterised, which is the caller's cue to draw nothing rather than to
     * guess.
     *
     * Falls back a weight at a time: the requested weight, else Regular. There
     * is no atlas-level equivalent of raylib's built-in font, and that is
     * deliberate — a built-in is a raylib resource, so offering it here would
     * put raylib back into the path this exists to keep clear of it.
     *
     * `phase` selects which horizontal subpixel variant to use: 0 is on the
     * pixel, 1 is a quarter right, and so on. A painter picks it from the
     * fractional part of the position it wants and then draws on the whole
     * pixel below it - so the glyph is never resampled, but the text still sits
     * where the layout asked. Anything measuring rather than drawing should
     * pass 0; the advances are identical across phases by construction.
     *
     * const, and it mutates the cache, because "which atlas" is a question
     * measure() has to ask too and neither caller is conceptually writing to
     * the font set. */
    const GlyphAtlas* atlasFor(FontWeight weight, float sizePx, int phase) const;

    /* The same thing as a raylib Font, for UiPainter — built from the atlas
     * above on first use and cached beside it. Falls back to raylib's built-in
     * when there is no atlas, so the shipping path always has something to
     * draw with.
     *
     * REQUIRES A GL CONTEXT, because this is the call that uploads. */
    const Font& fontFor(FontWeight weight, float sizePx, int phase) const;

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
     * rehashes. This is cold code by the standard in UiContext.hpp.
     *
     * A MAP ALSO KEEPS ITS ELEMENTS PUT, which atlasFor's callers rely on: the
     * pointer it hands back stays valid as the cache grows, and a container
     * that reallocated would dangle every atlas a painter had cached against. */
    static std::uint32_t cacheKey(int weightIndex, int sizePx, int phase);

    /* Which cache slot a request resolves to after the weight fallback, and
     * whether there is a file behind it at all. Shared by atlasFor and fontFor
     * so the two can never disagree about which entry a style names. */
    bool resolve(FontWeight weight, float sizePx, int phase,
                 int& outIndex, int& outSize, int& outPhase) const;

    /* The file behind each weight, empty when it never loaded. */
    std::string paths_[kWeightCount]{};
    bool        loaded_[kWeightCount]{};

    std::uint32_t generation_ = 0;

    /* mutable because atlasFor and fontFor are const — see their comments. */
    mutable std::map<std::uint32_t, GlyphAtlas> atlases_;
    mutable std::map<std::uint32_t, Font>       fonts_;
};

}  // namespace cromwell::ui
