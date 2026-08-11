/* UiText.hpp — a run of text a widget wants drawn, and the metrics it needs to
 * lay one out.
 *
 * SINGLE RESPONSIBILITY: describe text as data, and declare the one question a
 * layout has to ask about it — how big is this string going to be.
 *
 * WHY THE METRICS ARE AN INTERFACE. Glyph sizes come from a rasterised font
 * atlas, which lives on the renderer's side of the engine and needs a window to
 * exist. Widget LAYOUT — where the keycap sits relative to the label, how wide
 * a button ends up, where a paragraph wraps — is arithmetic over those sizes and
 * nothing more. Putting the arithmetic behind an interface keeps every
 * text-bearing widget in cromwell_base, testable headlessly against a stub whose
 * glyphs are a known width, which is the only way to assert that a button
 * centres its content without opening a window and looking.
 *
 * WEIGHTS, NOT FONT HANDLES. A widget asks for SemiBold at 16px; which file
 * that resolves to is the font library's problem (see ui/paint/UiFontSet.hpp).
 * The alternative — passing font objects through every widget signature — puts
 * a renderer type in the headless half's interface, which is the one thing the
 * split exists to prevent.
 *
 * LETTER SPACING IS IN PIXELS, deliberately unlike Slate, where it is thousandths
 * of an em. The em conversion belongs at the point a style is authored, not
 * inside every measure() implementation; the PO styles' 150 becomes
 * `0.15f * sizePx` at the one call site that names the style.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cromwell::ui {

/* The Inter typefaces the PO UI is drawn in. Regular is the fallback for any
 * weight a font set has not been given a file for — a missing Bold should make
 * the UI look slightly wrong, never make it disappear. */
enum class FontWeight {
    Regular,
    Medium,
    SemiBold,
    Bold,
    ExtraBold,
};

/* How a line sits inside the box it was given. */
enum class TextAlign {
    Left,
    Centre,
    Right,
};

/* Everything about a piece of text except where it goes.
 *
 * ONE-SHOT DATA CARRIER, the second exception in the project's encapsulation
 * rule: built by one widget, read by one painter, no invariant spans the
 * fields, dead at the end of the frame, owns nothing that needs a destructor
 * beyond the string. */
struct TextStyle {
    float      sizePx = 16.0f;
    FontWeight weight = FontWeight::Regular;
    float      letterSpacingPx = 0.0f;
    UiColor    colour = UiColor::white();
};

/* Multiplies the pixel dimensions by `factor`, leaving the colour alone.
 *
 * Part of the display-scale contract described in UiContext.hpp: a style is
 * authored at a reference size and scaled ONCE on the way in, so that by the
 * time a run reaches the font set its size is in device pixels and the atlas is
 * rasterised at the size it will actually be drawn. */
inline TextStyle scaled(const TextStyle& style, float factor)
{
    return TextStyle{ style.sizePx * factor, style.weight,
                      style.letterSpacingPx * factor, style.colour };
}

/* A laid-out run: this string, at this position, in this style. `position` is
 * the TOP-LEFT of the line box, not the baseline — baselines differ per font
 * and per size, and every widget here positions by box. */
struct TextRun {
    std::string text;
    Vec2        position;
    TextStyle   style;
};

/* ---- string helpers the controls share ---------------------------------- */

/* ASCII-only upper-casing, for the kit's SHOUTED labels.
 *
 * ASCII-only deliberately, and it is a limitation worth stating rather than
 * hiding: correct case mapping is locale-dependent (Turkish dotless i) and can
 * change a string's length (the German eszett), which is a localisation
 * library's job. Everything here is authored UI copy that a translator would
 * supply already cased for their language, so the honest thing is to upper-case
 * what is unambiguous and leave everything else alone. */
std::string toUpperAscii(std::string_view text);

/* Caption text with animated trailing dots: "Loading." -> "Loading.." ->
 * "Loading...", stepping every `stepSeconds` on the given clock.
 *
 * Dots already at the end of the authored string set the cycle's peak, so
 * "Working...." breathes up to four and a string with none gets three. That
 * rule exists so the animation is authored in the copy itself rather than
 * configured beside it. */
std::string animateTrailingDots(std::string_view text, float stepSeconds, double timeSeconds);

/* What a layout needs to know about a font it cannot see.
 *
 * Implementations live with the renderer. The headless tests supply a stub with
 * fixed-width glyphs, which is enough to pin down every layout decision the
 * widgets make. */
class TextMetrics {
public:
    virtual ~TextMetrics() = default;

    /* Width and height of `text` drawn in `style`, in pixels. Height is the
     * line box, so a single-line measure of an empty string still returns the
     * line height — a button with no label keeps its bar height rather than
     * collapsing, which is what every menu wants. */
    virtual Vec2 measure(std::string_view text, const TextStyle& style) const = 0;

    /* Height of one line at this size, independent of content. */
    virtual float lineHeight(const TextStyle& style) const = 0;

    /* Break `text` into lines no wider than `maxWidth`, at spaces, appending to
     * `outLines` (which is CLEARED first). A word longer than the limit is left
     * over-long on its own line rather than broken mid-word: hyphenation needs
     * a dictionary, and a URL that overflows a tip card is a better failure than
     * one chopped in half.
     *
     * Non-virtual and implemented once in terms of measure() — wrapping is the
     * same algorithm for every font, and an implementation that got it subtly
     * different per backend is a bug nobody would look for. */
    void wrap(std::string_view text, float maxWidth, const TextStyle& style,
              std::vector<std::string>& outLines) const;
};

}  // namespace cromwell::ui
