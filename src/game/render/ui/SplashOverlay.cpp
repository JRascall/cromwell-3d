#include "game/render/ui/SplashOverlay.hpp"

#include "cromwell/math/curve/Easing.hpp"
#include "cromwell/ui/core/UiTheme.hpp"
#include "cromwell/ui/loader/LoadingBar.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

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

/* THE WHOLE SCREEN FADES UP FROM BLACK, image and bar together, rather than
 * each element arriving on its own schedule. One veil over everything is both
 * simpler and better: elements that fade in separately read as a UI being
 * assembled, and what this wants to read as is a curtain going up.
 *
 * Long enough to be seen as a deliberate reveal rather than a dropped frame.
 * The splash shader ramps its own effects over half a second
 * (SplashPass::kRampSeconds) and is deliberately quicker — the painting should
 * be present and settling while the light is still coming up on it. */
constexpr float kFadeInSeconds = 1.4f;

/* Fast at first, settling at the end.
 *
 * NOT LINEAR, and the difference is the entire point of the change: a linear
 * fade from black spends its first third in territory too dark to see, so it
 * reads as a delay followed by a fade. An eased-out curve puts most of the
 * brightness in early and then takes its time over the last few percent, which
 * is where the eye actually notices the arrival. Cubic rather than Expo because
 * Expo is almost fully lit within a fifth of the duration, which is a cut with
 * a flourish rather than a fade. */
constexpr cromwell::Ease kFadeCurve = cromwell::Ease::CubicOut;

}  // namespace

void drawSplashOverlay(ui::UiContext& context, float splashSeconds, float progress)
{
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

    /* Full strength. The veil below fades the bar along with the image, so
     * fading it here as well would apply the curve twice and leave it lagging
     * behind the painting it sits on. */
    bar.fillColour = ui::theme::accent();
    bar.trackColour = ui::UiColor::white().withAlpha(0.18f);

    /* NO HALO, for now. The glow is what makes a bright fill read as emissive
     * against a lit scene, and on a painting this soft it reads instead as the
     * bar being slightly out of focus. Kept explicit even though the kit now
     * defaults the halo off, because the reason it is off here is about this
     * artwork and would otherwise be lost the day the default flips back. Give
     * it a strength of ~1.5 if the splash art ever gets darker or the bar
     * heavier. */
    bar.glowStrength = 0.0f;

    ui::drawLoadingBar(context, ui::UiContext::id("splash.bar"),
                       barBounds, ui::scaled(bar, context.scale()));

    /* NO CAPTION. The image already says what screen this is, and "LOADING"
     * under a bar that is visibly loading is a label on a label — the bar's
     * own movement carries it. The kit's animated-dots helper is still there
     * (ui/core/UiText.hpp) for a screen that genuinely needs words. */

    /* THE VEIL, LAST, OVER EVERYTHING — including the bar, which is the point:
     * the screen arrives as one picture rather than as parts.
     *
     * A black rectangle at falling opacity rather than a tint on the image,
     * because the image is drawn by a shader that knows nothing about this and
     * the ImGui text splash (shown when there is no image at all) is not ours
     * to tint either. One rect over the top fades whatever happens to be
     * beneath it. */
    const float reveal = cromwell::ease(kFadeCurve,
                                        std::clamp(splashSeconds / kFadeInSeconds, 0.0f, 1.0f));
    if (reveal < 1.0f) {
        ui::shapes::addRect(context.drawList(), screen,
                            ui::UiColor::black().withAlpha(1.0f - reveal));
    }
}

}  // namespace game
