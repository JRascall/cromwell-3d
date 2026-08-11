/* BlurPanel.hpp — frosted-glass backdrop.
 *
 * SINGLE RESPONSIBILITY: blur what is behind a rectangle, lay a tinted fill
 * over it, and hand back the rectangle the content goes in.
 *
 * WHAT IT IS FOR: a dialog scrim, a pause-menu backdrop, a HUD card. The point
 * of frosting rather than dimming is that the scene stays legible as CONTEXT —
 * you can still see that your squad is where you left it — while losing enough
 * detail that text on top of it reads. A flat dark plate loses the context; a
 * plain translucent one loses the legibility.
 *
 * THE FILL'S ALPHA IS THE PANEL'S OPACITY, from 0 (pure blur, the scene ghosts
 * through in colour) to 1 (a solid plate that happens to have a blur behind it
 * nobody can see). One number rather than a separate opacity dial, because the
 * two were never independent.
 *
 * NO CHILD SLOT, unlike the retained original. An immediate-mode panel cannot
 * own its content — the content is whatever the caller draws next — so it
 * returns the padded content rect and the caller draws into it. That is less
 * machinery and strictly more flexible: the content can be anything, including
 * another panel.
 *
 * THE BLUR ITSELF is a draw-list command rather than geometry, because it reads
 * the framebuffer. See UiBackdropBlur in ui/core/UiDrawList.hpp, and the
 * painter for how it is executed.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct BlurPanelSpec {
    /* Blur radius in screen pixels. 0 skips the blur entirely and leaves just
     * the fill, which is the cheap path and worth having — not every card
     * needs frosting. */
    float blurStrengthPx = 12.0f;

    /* Laid over the blur. Its alpha is the panel's opacity — see the header. */
    UiColor fillColour = UiColor::black().withAlpha(0.4f);

    /* Per corner: top-left, top-right, bottom-right, bottom-left. */
    float cornerRadii[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    UiPadding contentPadding;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`.
 *
 * THE BLUR STRENGTH SCALES, and it has to: it is a radius in device pixels, so
 * leaving it alone would frost a panel half as much at 200% and the glass would
 * look thinner on exactly the monitors that show it best. */
inline BlurPanelSpec scaled(const BlurPanelSpec& spec, float factor)
{
    BlurPanelSpec out = spec;
    out.blurStrengthPx *= factor;
    out.contentPadding = ui::scaled(out.contentPadding, factor);
    for (float& radius : out.cornerRadii) {
        radius *= factor;
    }
    return out;
}

/* Draws the panel and returns the rect its content should go in. */
UiRect drawBlurPanel(UiContext& context, const UiRect& bounds, const BlurPanelSpec& spec);

}  // namespace cromwell::ui
