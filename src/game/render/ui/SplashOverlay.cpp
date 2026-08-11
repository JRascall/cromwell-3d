#include "game/render/ui/SplashOverlay.hpp"

#include "cromwell/ui/core/UiTheme.hpp"
#include "cromwell/ui/loader/LoadingBar.hpp"

#include <algorithm>

namespace game {
namespace ui = cromwell::ui;

namespace {

/* Reference pixels — see the display-scale note in GameUi.hpp. Everything here
 * reaches a widget through context.px(). */
constexpr float kBottomMargin = 64.0f;
constexpr float kBarWidth = 420.0f;
constexpr float kBarHeight = 4.0f;

/* The bar never spans more than this fraction of the window, so it stays a
 * detail on a wide monitor instead of a stripe across it. */
constexpr float kMaxWidthFraction = 0.45f;

/* Seconds for the overlay to fade in. The splash image ramps its own effects up
 * over half a second (SplashPass::kRampSeconds); arriving with it rather than
 * before it is what stops the bar reading as a separate thing bolted on top. */
constexpr float kFadeInSeconds = 0.6f;

}  // namespace

void drawSplashOverlay(ui::UiContext& context, float splashSeconds, float progress)
{
    const float fade = std::clamp(splashSeconds / kFadeInSeconds, 0.0f, 1.0f);
    if (fade <= 0.0f) {
        return;
    }

    /* Eased, so it arrives the way everything else in the kit does. */
    const float alpha = ui::theme::easeInOut(fade, ui::theme::kFadeEase);

    const ui::UiRect screen = context.screenRect();
    const float width = std::min(context.px(kBarWidth), screen.width * kMaxWidthFraction);
    const float barHeight = context.px(kBarHeight);

    /* Bottom-anchored: the splash image carries the wordmark in the middle, and
     * a bar across the centre of a painting is vandalism. */
    const float barY = screen.bottom() - context.px(kBottomMargin);
    const ui::UiRect barBounds{ screen.centre().x - width * 0.5f, barY, width, barHeight };

    ui::LoadingBarSpec bar;
    bar.progress = std::clamp(progress, 0.0f, 1.0f);
    bar.barHeightPx = kBarHeight;
    bar.roundedEnds = true;

    /* A slow glide. The splash's progress moves smoothly today, but the moment
     * real loading reports in it will arrive in lumps — a shader cache resolving
     * thirty programs at once — and a bar that teleports makes the lumps the
     * most noticeable thing on the screen. Half a second for a full traverse is
     * enough to smooth that without ever looking like it is lagging. */
    bar.fillAnimationSeconds = 0.5f;

    bar.fillColour = ui::theme::accent().scaledAlpha(alpha);
    bar.trackColour = ui::UiColor::white().withAlpha(0.18f * alpha);

    /* NO HALO, for now. The glow is what makes a bright fill read as emissive
     * against a lit scene, and on a painting this soft it reads instead as the
     * bar being slightly out of focus. Set it back to theme::kGlowStrength if
     * the splash art ever gets darker or the bar heavier. */
    bar.glowStrength = 0.0f;

    ui::drawLoadingBar(context, ui::UiContext::id("splash.bar"),
                       barBounds, ui::scaled(bar, context.scale()));

    /* NO CAPTION. The image already says what screen this is, and "LOADING"
     * under a bar that is visibly loading is a label on a label — the bar's
     * own movement carries it. The kit's animated-dots helper is still there
     * (ui/core/UiText.hpp) for a screen that genuinely needs words. */
}

}  // namespace game
