#include "cromwell/math/curve/Easing.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/* Back's overshoot constant, and the derived pair for the symmetric form. The
 * value is Penner's original: it overshoots by about 10%, which is enough to
 * read as physical and small enough not to look like a mistake. */
constexpr float kBackOvershoot = 1.70158f;
constexpr float kBackOvershootCubic = kBackOvershoot + 1.0f;
constexpr float kBackOvershootInOut = kBackOvershoot * 1.525f;

/* Bounce's piecewise constants, likewise the standard ones. */
constexpr float kBounceScale = 7.5625f;
constexpr float kBounceDivisor = 2.75f;

float bounceOut(float t)
{
    if (t < 1.0f / kBounceDivisor) {
        return kBounceScale * t * t;
    }
    if (t < 2.0f / kBounceDivisor) {
        t -= 1.5f / kBounceDivisor;
        return kBounceScale * t * t + 0.75f;
    }
    if (t < 2.5f / kBounceDivisor) {
        t -= 2.25f / kBounceDivisor;
        return kBounceScale * t * t + 0.9375f;
    }
    t -= 2.625f / kBounceDivisor;
    return kBounceScale * t * t + 0.984375f;
}

}  // namespace

float ease(Ease curve, float t)
{
    /* Clamped here, once, so no individual curve has to defend itself against a
     * caller whose clock overran. */
    t = std::clamp(t, 0.0f, 1.0f);

    /* The inverse, named — half the formulas below are "the In curve, mirrored"
     * and spelling that out each time is where sign errors come from. */
    const float u = 1.0f - t;

    switch (curve) {
    case Ease::Linear:       return t;

    case Ease::SmoothStep:   return t * t * (3.0f - 2.0f * t);
    case Ease::SmootherStep: return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);

    case Ease::QuadIn:       return t * t;
    case Ease::QuadOut:      return 1.0f - u * u;
    case Ease::QuadInOut:    return t < 0.5f ? 2.0f * t * t
                                             : 1.0f - 2.0f * u * u;

    case Ease::CubicIn:      return t * t * t;
    case Ease::CubicOut:     return 1.0f - u * u * u;
    case Ease::CubicInOut:   return t < 0.5f ? 4.0f * t * t * t
                                             : 1.0f - 4.0f * u * u * u;

    case Ease::QuintIn:      return t * t * t * t * t;
    case Ease::QuintOut:     return 1.0f - u * u * u * u * u;
    case Ease::QuintInOut:   return t < 0.5f ? 16.0f * t * t * t * t * t
                                             : 1.0f - 16.0f * u * u * u * u * u;

    case Ease::SineIn:       return 1.0f - std::cos(t * kPi * 0.5f);
    case Ease::SineOut:      return std::sin(t * kPi * 0.5f);
    case Ease::SineInOut:    return -(std::cos(kPi * t) - 1.0f) * 0.5f;

    /* The exponential family is not defined at its own endpoints — 2^-inf never
     * reaches zero — so both ends are pinned explicitly rather than left to
     * arrive within floating-point tolerance of them. */
    case Ease::ExpoIn:       return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
    case Ease::ExpoOut:      return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
    case Ease::ExpoInOut:
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) * 0.5f
                        : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;

    case Ease::CircIn:       return 1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t));
    case Ease::CircOut:      return std::sqrt(std::max(0.0f, 1.0f - u * u));
    case Ease::CircInOut:
        return t < 0.5f
            ? (1.0f - std::sqrt(std::max(0.0f, 1.0f - 4.0f * t * t))) * 0.5f
            : (std::sqrt(std::max(0.0f, 1.0f - 4.0f * u * u)) + 1.0f) * 0.5f;

    case Ease::BackIn:
        return kBackOvershootCubic * t * t * t - kBackOvershoot * t * t;
    case Ease::BackOut:
        return 1.0f + kBackOvershootCubic * (-u) * u * u + kBackOvershoot * u * u;
    case Ease::BackInOut:
        return t < 0.5f
            ? (4.0f * t * t * ((kBackOvershootInOut + 1.0f) * 2.0f * t - kBackOvershootInOut)) * 0.5f
            : ((2.0f * t - 2.0f) * (2.0f * t - 2.0f)
                   * ((kBackOvershootInOut + 1.0f) * (2.0f * t - 2.0f) + kBackOvershootInOut)
               + 2.0f) * 0.5f;

    case Ease::ElasticOut: {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        /* Period chosen so the first overshoot lands where Back's does; the
         * decay is what makes the rest read as settling rather than ringing. */
        constexpr float period = 2.0f * kPi / 3.0f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * period) + 1.0f;
    }

    case Ease::BounceOut:    return bounceOut(t);
    }

    return t;
}

const char* toString(Ease curve)
{
    switch (curve) {
    case Ease::Linear:       return "linear";
    case Ease::SmoothStep:   return "smoothstep";
    case Ease::SmootherStep: return "smootherstep";
    case Ease::QuadIn:       return "quad-in";
    case Ease::QuadOut:      return "quad-out";
    case Ease::QuadInOut:    return "quad-in-out";
    case Ease::CubicIn:      return "cubic-in";
    case Ease::CubicOut:     return "cubic-out";
    case Ease::CubicInOut:   return "cubic-in-out";
    case Ease::QuintIn:      return "quint-in";
    case Ease::QuintOut:     return "quint-out";
    case Ease::QuintInOut:   return "quint-in-out";
    case Ease::SineIn:       return "sine-in";
    case Ease::SineOut:      return "sine-out";
    case Ease::SineInOut:    return "sine-in-out";
    case Ease::ExpoIn:       return "expo-in";
    case Ease::ExpoOut:      return "expo-out";
    case Ease::ExpoInOut:    return "expo-in-out";
    case Ease::CircIn:       return "circ-in";
    case Ease::CircOut:      return "circ-out";
    case Ease::CircInOut:    return "circ-in-out";
    case Ease::BackIn:       return "back-in";
    case Ease::BackOut:      return "back-out";
    case Ease::BackInOut:    return "back-in-out";
    case Ease::ElasticOut:   return "elastic-out";
    case Ease::BounceOut:    return "bounce-out";
    }
    return "linear";
}

}  // namespace cromwell
