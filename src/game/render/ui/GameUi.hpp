/* GameUi.hpp — this game's one UI surface.
 *
 * SINGLE RESPONSIBILITY: own the font set, the context and the painter, and
 * bracket a frame of widgets between raylib's input and raylib's framebuffer.
 *
 * WHY ONE OF THESE RATHER THAN ONE PER SCREEN. The font set is a cache of
 * rasterised atlases — a texture per weight per size — so a second one is a
 * second copy of every atlas the first already had. More importantly the
 * CONTEXT holds the per-widget state that makes hover fades and drags work
 * across frames, and two contexts would each hold half of it: a control drawn
 * by one screen and then by another would forget it had been hovered.
 *
 * One surface, every screen draws into it, one paint at the end.
 *
 * WHERE THE DISPLAY SCALE COMES FROM, and it is the whole DPI story from the
 * game's side: raylib reports a framebuffer the size of the window in real
 * device pixels (this app does not set FLAG_WINDOW_HIGHDPI, so nothing is ever
 * upscaled by the OS), and GetWindowScaleDPI says how much bigger the monitor
 * wants things to be. Multiply those and the UI is the right physical size AND
 * crisp; use neither and it is crisp but tiny on a good monitor. See the long
 * note in cromwell/ui/core/UiContext.hpp for why the multiply happens here, on
 * the way in, rather than at the painter.
 *
 * IT IS IN game/ AND NOT IN cromwell BECAUSE IT MAKES CHOICES. Which typeface,
 * which weights, where the files live, what the UI scale is — those are a
 * project's decisions, and an engine that made them would be making them for
 * every project that embedded it.
 */
#pragma once

#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/paint/UiFontSet.hpp"
#include "cromwell/ui/paint/UiPainter.hpp"

#include <memory>

namespace game {

class GameUi {
public:
    GameUi() = default;

    /* Loads the typeface. REQUIRES A GL CONTEXT — call after the window is
     * open. Safe to call every frame; it does the work once.
     *
     * A missing font file is not an error: the kit falls back a weight at a
     * time and finally to raylib's built-in face, so a checkout without the
     * font pack (they are gitignored — see assets/fonts/README.md) gets a UI
     * that looks wrong rather than no UI at all. */
    void setup();

    /* Starts a frame: samples the cursor, the clock and the display scale, and
     * clears the draw list. Returns the context to draw into.
     *
     * Call it once per frame, on whichever path is running — the front end and
     * the in-game HUD are alternatives, never both. */
    cromwell::ui::UiContext& begin();

    /* Paints everything submitted since begin(). Call inside BeginDrawing. */
    void end();

    /* For screens that need to measure before they lay out. Null before the
     * first setup(). */
    cromwell::ui::UiContext* context() { return context_.get(); }

    /* True when a widget claimed the cursor in the LAST completed frame.
     *
     * Last, not this: an immediate-mode control's bounds exist only inside the
     * call that draws it, so hover is resolved during the render — which is
     * after input has been arbitrated. Asking here gives the most recent
     * complete answer, which is what PointerFocus is built to consume. See the
     * one-frame note in cromwell/input/PointerFocus.hpp, and wantsMouse() in
     * UiContext.hpp for what "claimed" means.
     *
     * False before the first frame, which is correct: nothing has drawn, so
     * nothing is under the cursor. */
    bool wantsMouse() const { return context_ && context_->wantsMouse(); }

    const cromwell::ui::UiFontSet& fonts() const { return fonts_; }

private:
    bool loaded_ = false;

    cromwell::ui::UiFontSet fonts_;
    cromwell::ui::UiPainter painter_;

    /* By pointer because the context takes the font set by reference at
     * construction, so the font set has to exist first. */
    std::unique_ptr<cromwell::ui::UiContext> context_;
};

}  // namespace game
