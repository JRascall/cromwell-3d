/* UiColor.hpp — a UI colour, in linear space, and how it reaches a vertex.
 *
 * SINGLE RESPONSIBILITY: be four floats of linear RGBA and know how to encode
 * itself into the eight-bit sRGB a vertex carries.
 *
 * WHY LINEAR IN, sRGB OUT. Every blend the widgets do — hover fades, glow
 * falloff, per-spoke alpha ramps — is a lerp between two colours, and a lerp
 * between two sRGB values is wrong in a way that shows: midpoints come out
 * muddy and a fade to black darkens too fast at the start. So the widget code
 * works in linear throughout and encodes once, at the moment a vertex is
 * written. That is what Slate does (`FLinearColor::ToFColor(true)`), and the
 * ported geometry depends on it — the two-ring glow falloff was tuned against
 * an sRGB-encoded ramp, so encoding somewhere else would change the look.
 *
 * ALPHA IS NOT ENCODED. Coverage is not a colour: it is a blend weight, and
 * running it through a transfer curve would make a 50% feather ring stop being
 * half-covered. sRGB on RGB, linear on A — again matching Slate.
 *
 * PUBLIC MEMBERS, the mathematical-value-type exception the project documents
 * in Vec3.hpp: no combination of four floats is an invalid colour, and
 * `c.a *= fade` is the vocabulary. Colours out of the 0..1 range are allowed on
 * purpose — an HDR accent that blooms is a legitimate thing to author — and
 * clamping happens at encode time rather than in a setter that would silently
 * destroy the value.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cromwell::ui {

struct UiColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    static constexpr UiColor white() { return { 1.0f, 1.0f, 1.0f, 1.0f }; }
    static constexpr UiColor black() { return { 0.0f, 0.0f, 0.0f, 1.0f }; }
    static constexpr UiColor transparent() { return { 0.0f, 0.0f, 0.0f, 0.0f }; }

    /* From an eight-bit sRGB triplet, which is how colours are quoted in art
     * direction and in every hex picker. Decodes to linear, so the value that
     * comes back out of toSrgb8() is the one that went in. */
    static UiColor fromSrgb8(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8, std::uint8_t a8 = 255);

    /* The same colour at a different opacity. Used constantly — the glow
     * passes, the per-spoke tail fade, the highlight blends — and worth having
     * as a name rather than three lines of struct surgery at each site. */
    UiColor withAlpha(float alpha) const { return { r, g, b, alpha }; }

    /* The same colour scaled in opacity, for stacking one fade on another
     * (a widget's tint times a spoke's own ramp). */
    UiColor scaledAlpha(float factor) const { return { r, g, b, a * factor }; }

    /* Fully transparent, SAME RGB. This is the feather colour, and keeping the
     * hue is not cosmetic: interpolating to transparent BLACK instead would
     * darken the outer half-pixel of every shape, which reads as a grubby
     * outline on light UI. */
    UiColor toEdge() const { return { r, g, b, 0.0f }; }

    /* Packed 0xAABBGGRR — little-endian RGBA8, the layout both raylib's Color
     * and a GL_UNSIGNED_BYTE vertex attribute expect, so the painter uploads
     * this word without touching it. */
    std::uint32_t toSrgb8() const;
};

/* Linear interpolation, component-wise and including alpha. */
inline UiColor lerp(const UiColor& a, const UiColor& b, float t)
{
    return { a.r + (b.r - a.r) * t,
             a.g + (b.g - a.g) * t,
             a.b + (b.b - a.b) * t,
             a.a + (b.a - a.a) * t };
}

/* Component-wise product — how a widget's own colour combines with an inherited
 * tint (a screen fading out multiplies everything it contains by one alpha). */
inline UiColor modulate(const UiColor& a, const UiColor& b)
{
    return { a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a };
}

/* ---- the sRGB transfer curve, both directions -------------------------- */
/* The exact piecewise definition rather than the gamma-2.2 approximation. The
 * difference is only visible in the darkest few percent, which is precisely
 * where a glow's tail lives. */
inline float linearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float srgbToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline UiColor UiColor_fromSrgb8_impl(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8, std::uint8_t a8)
{
    return { srgbToLinear(static_cast<float>(r8) / 255.0f),
             srgbToLinear(static_cast<float>(g8) / 255.0f),
             srgbToLinear(static_cast<float>(b8) / 255.0f),
             static_cast<float>(a8) / 255.0f };
}

inline UiColor UiColor::fromSrgb8(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8, std::uint8_t a8)
{
    return UiColor_fromSrgb8_impl(r8, g8, b8, a8);
}

inline std::uint32_t UiColor::toSrgb8() const
{
    const auto quantise = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return quantise(linearToSrgb(r))
         | (quantise(linearToSrgb(g)) << 8)
         | (quantise(linearToSrgb(b)) << 16)
         | (quantise(a) << 24);
}

}  // namespace cromwell::ui
