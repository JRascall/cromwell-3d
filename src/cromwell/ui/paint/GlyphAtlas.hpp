/* GlyphAtlas.hpp — one typeface at one size, rasterised into coverage bytes.
 *
 * SINGLE RESPONSIBILITY: turn a font file, a pixel size and a subpixel phase
 * into an atlas of 8-bit coverage plus the metrics that place each glyph. It
 * uploads nothing, owns no GPU resource, and names no graphics API.
 *
 * ==================== WHY THIS IS SPLIT OUT OF UiFontSet ===================
 *
 * There are two painters now — UiPainter through rlgl, DeviceUiPainter through
 * IRenderDevice — and text is the only thing in the UI that samples a texture,
 * so it is the only thing that made the two need different resources. Every
 * shape above them is untextured geometry both can draw from the same arrays.
 *
 * The bake, though, is identical for both. The hinting, the phase shift, the
 * shelf pack and the coverage curve are decisions about TYPE, and none of them
 * changes because a texture is created by a different call. Leaving them in a
 * file that includes raylib.h meant the device path could not have text without
 * a second implementation of all of it — which is precisely the thing that
 * drifts, and it would drift in the one place where "drifted" means "the labels
 * look slightly worse and nobody can say why".
 *
 * So: bake here, upload there. UiFontSet caches these and hands one to whoever
 * is drawing; each painter turns it into a texture in its own API. THE METRICS
 * ARE SHARED, which is the property that actually matters — two painters that
 * advanced the pen differently would put a label's end in two places, and the
 * layout above them measured against only one of the answers.
 *
 * ================= ONE CHANNEL, AND IT IS COVERAGE NOT ALPHA ===============
 *
 * A texel here is the fraction of that pixel the glyph covers, with the gamma
 * curve already applied — see the long note in the .cpp for why that curve
 * exists and why it is baked rather than shaded. A consumer multiplies its
 * text colour's alpha by this and blends straight; there is nothing further to
 * decode, and applying an sRGB decode to it would be the classic double
 * correction that makes light text look thin.
 *
 * raylib's font convention is two channels — grey pinned at 255, coverage in
 * alpha — and UiFontSet expands to that when it builds a raylib Font. The
 * expansion lives at that consumer rather than here because it is a fact about
 * DrawTexturePro's blending, not about the type.
 *
 * WHERE THIS LIVES. In `cromwell` rather than `cromwell_base`, only because
 * FreeType is linked into the former. Nothing in this file needs a window or a
 * GL context, and moving it down is a CMake edit rather than a rewrite the day
 * the headless half wants to measure text without a renderer.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cromwell::ui {

class GlyphAtlas {
public:
    /* Printable ASCII, 32 through 126 — raylib's default set, kept because the
     * UI's copy is authored in it. A wider charset is a localisation decision
     * (and a packing one), not a rendering change, so it is not smuggled in
     * here ahead of the decision. */
    static constexpr int kFirstCodepoint = 32;
    static constexpr int kCodepointCount = 95;

    /* Where one glyph sits in the atlas, and how it is placed when drawn.
     *
     * A ONE-SHOT DATA CARRIER in the sense CLAUDE.md exempts from the private-
     * members rule: filled by the bake, read by a painter, no invariant spans
     * the fields.
     *
     * `x/y/width/height` are texels into coverage(). `offsetX/offsetY` place
     * the bitmap relative to the pen and the TOP of the line's glyph box — the
     * baseline is already folded into offsetY, because every widget in this kit
     * positions by box and none of them knows what a baseline is. `advanceX` is
     * how far the pen moves afterwards, and it is the UNSHIFTED advance: the
     * phase moves where a glyph is drawn, not how far the pen travels, or every
     * following character in the run would drift.
     *
     * Plain ints rather than a packed struct. There are 95 of these against an
     * atlas of hundreds of kilobytes, built once per (weight, size, phase) —
     * cold code by the standard in UiContext.hpp, where clarity is worth more
     * than three kilobytes. */
    struct Glyph {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int offsetX = 0;
        int offsetY = 0;
        int advanceX = 0;
    };

    /* Rasterises `path` at `sizePx`, with every outline shifted right by
     * `phase`/`phaseCount` of a pixel before rendering — see the subpixel note
     * in the .cpp, and UiFontSet::kPhaseCount for why that count is currently
     * one.
     *
     * AN ATLAS THAT COULD NOT BE BUILT COMES BACK !valid(), rather than
     * throwing or logging. A missing or unreadable font is an ordinary outcome
     * the font set already degrades around — weights fall back a step at a time
     * — and a rasteriser that aborted would turn a shipped machine's missing
     * asset into a crash. */
    static GlyphAtlas bake(const std::string& path, int sizePx, int phase, int phaseCount);

    bool valid() const { return !coverage_.empty(); }

    int width() const { return width_; }
    int height() const { return height_; }

    /* The size this was rasterised at, which is the size it must be drawn at.
     * Every consumer divides by it to form the scale factor, and that factor is
     * one by construction — see UiFontSet::rasterSize. */
    int sizePx() const { return sizePx_; }

    /* Texels of empty space between packed glyphs. Only a consumer that has to
     * restate it to another library needs this — raylib's Font carries the
     * number — and it is exposed rather than assumed so the two cannot drift
     * apart into a one-texel bleed nobody can source. */
    int padding() const { return padding_; }

    /* width() * height() bytes, one per texel, tightly packed, top row first. */
    const std::vector<std::uint8_t>& coverage() const { return coverage_; }

    const Glyph& glyph(int index) const { return glyphs_[index]; }

    /* The glyph index for a codepoint, falling back to '?' for anything outside
     * the set.
     *
     * MATCHING raylib's GetGlyphIndex DELIBERATELY, including the fallback. The
     * two painters resolve characters independently, and a string with a
     * non-ASCII byte in it that fell back to '?' on one path and to index zero —
     * a space — on the other would measure and draw at two different widths. */
    static int indexOf(int codepoint);

private:
    int width_ = 0;
    int height_ = 0;
    int sizePx_ = 0;
    int padding_ = 0;

    std::vector<std::uint8_t> coverage_;
    Glyph                     glyphs_[kCodepointCount]{};
};

}  // namespace cromwell::ui
