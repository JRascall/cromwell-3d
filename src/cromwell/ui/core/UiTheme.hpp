/* UiTheme.hpp — the shared look, and the two easing curves that carry it.
 *
 * SINGLE RESPONSIBILITY: hold the constants and blend maths every widget in the
 * kit agrees on, so "the accent colour" and "the standard fade" are one
 * definition rather than a number copied into twelve headers.
 *
 * WHY THE FADE TIMES ARE HERE AND NOT PER WIDGET. They are per widget — every
 * spec carries its own — but they all DEFAULT to these, and that is the point:
 * a menu whose slider fades in 0.08s and whose buttons fade in 0.12s reads as
 * broken in a way nobody can name. One set of defaults makes consistency the
 * path of least resistance and leaves deviation available where a designer
 * wants it.
 *
 * THE EASING IS UNREAL'S, REPRODUCED EXACTLY. `easeInOut` is
 * FMath::InterpEaseInOut and `interpConstantTo` is FMath::FInterpConstantTo,
 * down to the exponent convention (1 = linear, higher = softer). The PO widgets
 * were tuned by eye against those curves, so a curve that is merely similar
 * would change every animation in the kit by a little — which is exactly the
 * kind of difference that is impossible to debug later because nothing is
 * obviously wrong.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui::theme {

/* The accent (#5FE08B): the hover/focus/selection colour shared by everything
 * that tints on attention. Quoted as the sRGB hex it was picked as, decoded to
 * linear here, so the number in this file matches the one in the palette. */
inline UiColor accent()
{
    return UiColor::fromSrgb8(0x5F, 0xE0, 0x8B);
}

/* The dim companion to a bright fill: a ring's track, a bar's groove, an empty
 * chip. White at low alpha rather than a grey, so it takes the colour of
 * whatever it is laid over. */
inline UiColor track()
{
    return UiColor::white().withAlpha(0.15f);
}

/* Standard fade timings. Faster in than out on purpose — attention should
 * arrive immediately and leave gently, and equal times read as sluggish on the
 * way in and abrupt on the way out. */
inline constexpr float kFadeInSeconds = 0.08f;
inline constexpr float kFadeOutSeconds = 0.20f;

/* Fade curve exponent. 1 is linear; 2 is the kit's default ease-in-out. */
inline constexpr float kFadeEase = 2.0f;

/* Glow defaults. OFF BY DEFAULT: strength zero, so no widget in the kit wears a
 * halo unless its spec asks for one. The halo is an effect, and an effect that
 * arrives without being asked for is one that has to be switched off at every
 * call site — SplashOverlay was already doing exactly that. Opt-in puts the
 * decision where the look is being designed rather than in a header nobody
 * reading a menu's code is looking at.
 *
 * THE RADIUS KEEPS ITS TUNED VALUE. Strength is the on/off dial — every draw
 * site no-ops when it is zero — so leaving the radius at the eight pixels the PO
 * widgets were tuned against means raising the strength alone restores the
 * original look, rather than requiring both numbers to be rediscovered.
 *
 * The PO value was 1.5: a halo strong enough to read as emissive without the
 * shape looking blurred. Set `spec.glowStrength = 1.5f` to get it back. */
inline constexpr float kGlowStrength = 0.0f;
inline constexpr float kGlowRadiusPx = 8.0f;

/* ---- easing ------------------------------------------------------------ */

/* Symmetric ease-in-out with a shape exponent, matching FMath::InterpEaseInOut.
 * `exponent` below 1 is clamped away — it would invert the curve's convexity
 * and there is no look that wants that here. */
inline float easeInOut(float alpha, float exponent)
{
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    exponent = std::max(exponent, 1.0f);
    return alpha < 0.5f
        ? 0.5f * std::pow(alpha * 2.0f, exponent)
        : 1.0f - 0.5f * std::pow((1.0f - alpha) * 2.0f, exponent);
}

/* Move `current` toward `target` at a CONSTANT rate — matching
 * FMath::FInterpConstantTo, not the exponential FInterpTo.
 *
 * Constant rate is the right choice for progress bars and fades and the wrong
 * one for camera smoothing, and the difference matters: an exponential
 * approach never actually arrives, so a bar told to go to 100% sits at 99.6%
 * forever and the last pixel of fill never appears. */
inline float interpConstantTo(float current, float target, float deltaSeconds, float ratePerSecond)
{
    if (ratePerSecond <= 0.0f || deltaSeconds <= 0.0f) {
        return target;
    }
    const float step = ratePerSecond * deltaSeconds;
    const float distance = target - current;
    if (std::abs(distance) <= step) {
        return target;
    }
    return current + std::clamp(distance, -step, step);
}

/* Blend an idle colour toward a highlight colour by a raw 0..1 fade alpha,
 * eased. Every hover-tinted control in the kit goes through this, so the eased
 * shape of a highlight is defined once. */
inline UiColor blendHover(const UiColor& idle, const UiColor& highlight, float alpha, float ease)
{
    return lerp(idle, highlight, easeInOut(alpha, ease));
}

}  // namespace cromwell::ui::theme
