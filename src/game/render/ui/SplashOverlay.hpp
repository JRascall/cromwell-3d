/* SplashOverlay.hpp — the loading bar over the splash screen.
 *
 * SINGLE RESPONSIBILITY: draw the progress bar and its caption on top of the
 * splash image, and nothing else.
 *
 * WHY THE BAR IS WORTH HAVING AT ALL, given the splash is mostly a timer today.
 * Because a logo with no progress indication reads as a hang the moment it
 * lasts longer than someone expected, and it will: `splashLoadComplete()` in
 * Application is the hook asynchronous loading reports into, and the day it
 * returns false for four seconds the difference between "loading" and "frozen"
 * is this bar. Adding it now, while the progress is honest and boring, is
 * cheaper than adding it later while chasing a complaint.
 *
 * THE PROGRESS IS THE CALLER'S, and deliberately: Application owns both halves
 * of the splash's exit condition — the minimum display time and whether the
 * work is done — so it is the only thing that can say what fraction of the way
 * through we are. A bar that timed itself would disagree with the screen it is
 * drawn on.
 *
 * A free function rather than a class because it holds nothing: the bar's glide
 * lives in the UI context's per-widget state, keyed by id, which is exactly
 * what that mechanism is for.
 */
#pragma once

#include "cromwell/ui/core/UiContext.hpp"

namespace game {

/* Draws over the whole window, bottom-anchored.
 *
 * `splashSeconds` is the clock the splash shader animates on — used here only
 * to fade the bar in, so it does not appear fully formed on the first frame
 * while the image behind it is still ramping up.
 *
 * `progress` is 0..1 toward the splash being allowed to leave. */
void drawSplashOverlay(cromwell::ui::UiContext& context, float splashSeconds, float progress);

}  // namespace game
