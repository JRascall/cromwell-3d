/* IUiPainter.hpp — the seam between a UI draw list and a GPU.
 *
 * SINGLE RESPONSIBILITY: execute a UiDrawList, whatever is underneath.
 *
 * ====================== WHY THIS IS A ONE-METHOD INTERFACE ================
 *
 * Because the kit already narrowed to one place. UiPainter's own header says it
 * is "the only part of the UI that talks to the GPU" — everything above it is
 * arithmetic over rectangles producing data. That was written as a testability
 * argument and it turns out to be a PORTABILITY one: a boundary thin enough to
 * test headlessly is thin enough to swap.
 *
 * So the whole UI kit reaches the device path through this, and the widgets,
 * the layout, the theme and the shapes are untouched by the port. Compare the
 * renderer, where converting one pass dragged in its targets, its shaders and
 * everything else that wrote them.
 *
 * IF A WIDGET EVER NEEDS SOMETHING THIS CANNOT EXPRESS, the answer is a new
 * COMMAND KIND in UiDrawList — not a widget that reaches for GL, and not a
 * method here. This interface stays one call precisely so that both
 * implementations stay honest about what a draw list is.
 */
#pragma once

namespace cromwell::ui {

class UiDrawList;
class UiFontSet;

class IUiPainter {
public:
    virtual ~IUiPainter() = default;

    /* Executes every command in order. `fonts` resolves the text runs' weights;
     * it must be the same set the widgets measured against, or the layout and
     * the drawing will disagree.
     *
     * PAINTER'S ORDER IS THE LIST'S ORDER. No sorting and no layer ids — a
     * halo appended before its shape draws under it. An implementation that
     * reordered anything for batching would break the one property every widget
     * above here relies on. */
    virtual void draw(const UiDrawList& drawList, const UiFontSet& fonts) = 0;

    /* Releases whatever GPU scratch the implementation holds. Called by the
     * destructor; exposed for a caller that has to let go before the context
     * does — which on the device path is every caller, because the platform
     * owns the device and closes it first. */
    virtual void release() = 0;
};

}  // namespace cromwell::ui
