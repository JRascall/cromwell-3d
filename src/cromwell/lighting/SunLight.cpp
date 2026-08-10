#include "cromwell/lighting/SunLight.hpp"

#include "raymath.h"

#include <cmath>

namespace cromwell {
namespace {

Vector3 mix(Vector3 a, Vector3 b, float t)
{
    return Vector3{ a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t };
}

float smoothstep(float edge0, float edge1, float x)
{
    const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Sun colour at the two ends of its arc. The horizon value is not a sunset
 * cliche — it is what is left of daylight after the short wavelengths have
 * been scattered out of a long slant path through the atmosphere. */
constexpr Vector3 kSunHorizon = { 1.00f, 0.42f, 0.16f };
constexpr Vector3 kSunHigh    = { 1.00f, 0.97f, 0.92f };

constexpr Vector3 kZenithDay  = { 0.16f, 0.29f, 0.52f };
constexpr Vector3 kZenithDusk = { 0.07f, 0.09f, 0.17f };
constexpr Vector3 kHorizonDay  = { 0.52f, 0.62f, 0.75f };
constexpr Vector3 kHorizonDusk = { 0.42f, 0.26f, 0.20f };

/* Not black. A surface facing straight down still sees light bounced off the
 * ground, and zeroing it is the single most common reason untextured geometry
 * reads as plastic — every downward face turns into a hole. */
constexpr Vector3 kGroundDay  = { 0.13f, 0.12f, 0.10f };
constexpr Vector3 kGroundDusk = { 0.05f, 0.04f, 0.04f };

Vector3 modulate(Vector3 colour, Vector3 tint)
{
    return Vector3{ colour.x * tint.x, colour.y * tint.y, colour.z * tint.z };
}

}  // namespace

void SunLight::setAzimuth(float degrees)
{
    azimuth_ = std::fmod(degrees, 360.0f);
    if (azimuth_ < 0.0f) azimuth_ += 360.0f;
}

void SunLight::setElevation(float degrees)
{
    /* Never let it reach the horizon or the pole. At 0 the shadow projection
     * degenerates to an infinitely long box, and at 90 the look-at up vector
     * becomes parallel to the view direction. */
    elevation_ = Clamp(degrees, 4.0f, 86.0f);
}

float SunLight::dayFactor() const
{
    return std::sin(elevation_ * DEG2RAD);
}

Vector3 SunLight::travelDirection() const
{
    const float elevation = elevation_ * DEG2RAD;
    const float azimuth   = azimuth_ * DEG2RAD;
    const float horizontal = std::cos(elevation);

    /* the vector pointing AT the sun; light travels the other way */
    const Vector3 toSun{ horizontal * std::cos(azimuth),
                         std::sin(elevation),
                         horizontal * std::sin(azimuth) };
    return Vector3Normalize(Vector3Negate(toSun));
}

Vector3 SunLight::radiance() const
{
    const float day = dayFactor();

    /* Extinction is steep near the horizon and flat overhead, so the warm
     * shift and the dimming both key off a curve, not off the angle. */
    const float warmth   = smoothstep(0.02f, 0.42f, day);
    const float strength = std::pow(Clamp(day, 0.0f, 1.0f), 0.55f);

    return modulate(Vector3Scale(mix(kSunHorizon, kSunHigh, warmth),
                                 tuning_.peak * strength),
                    tuning_.tint);
}

Vector3 SunLight::zenithColour() const
{
    return modulate(mix(kZenithDusk, kZenithDay, smoothstep(0.0f, 0.45f, dayFactor())),
                    tuning_.skyTint);
}

Vector3 SunLight::horizonColour() const
{
    return modulate(mix(kHorizonDusk, kHorizonDay, smoothstep(0.0f, 0.45f, dayFactor())),
                    tuning_.skyTint);
}

Vector3 SunLight::groundColour() const
{
    return modulate(mix(kGroundDusk, kGroundDay, smoothstep(0.0f, 0.45f, dayFactor())),
                    tuning_.skyTint);
}

SunLight::ShadowProjection SunLight::shadowProjectionForSphere(Vector3 centre, float radius,
                                                               float depthExtent,
                                                               int mapResolution) const
{
    if (radius < 0.5f) radius = 0.5f;

    const Vector3 travel = travelDirection();
    const float worldTexel = (radius * 2.0f) / static_cast<float>(mapResolution);

    /* Orientation only — eye at the origin — so the light's axes are a fixed
     * frame the centre can be quantised in. Building the view around the
     * moving centre instead would put the snap in the wrong space and do
     * nothing. */
    const Matrix lightRotation =
        MatrixLookAt(Vector3{ 0.0f, 0.0f, 0.0f }, travel, Vector3{ 0.0f, 1.0f, 0.0f });

    Vector3 lightCentre = Vector3Transform(centre, lightRotation);
    lightCentre.x = std::floor(lightCentre.x / worldTexel) * worldTexel;
    lightCentre.y = std::floor(lightCentre.y / worldTexel) * worldTexel;

    /* Enough to reach any caster up-sun, and NO MORE. Slack here is not free:
     * the shader's bias is in normalised depth, so a range twice as deep makes
     * every bias twice as large in world units, and an over-large bias detaches
     * a shadow from its caster — light creeping out from the foot of a wall. */
    const float depth = depthExtent < 1.0f ? 1.0f : depthExtent;

    const Matrix projection = MatrixOrtho(lightCentre.x - radius, lightCentre.x + radius,
                                          lightCentre.y - radius, lightCentre.y + radius,
                                          -lightCentre.z - depth, -lightCentre.z + depth);

    /* raylib's convention: model x view x projection, applied to a row vector,
     * which is the same matrix a column-vector shader uses as projection x
     * view x model. Built here the way DrawMesh builds its mvp. */
    ShadowProjection result;
    result.viewProjection = MatrixMultiply(lightRotation, projection);
    result.worldTexelSize = worldTexel;
    result.depthRange     = depth * 2.0f;
    return result;
}

SunLight::ShadowProjection SunLight::shadowProjectionFor(Vector3 boundsMin, Vector3 boundsMax,
                                                         int mapResolution) const
{
    const Vector3 centre = Vector3Scale(Vector3Add(boundsMin, boundsMax), 0.5f);
    const float   span   = Vector3Length(Vector3Subtract(boundsMax, boundsMin));
    return shadowProjectionForSphere(centre, span * 0.5f, span, mapResolution);
}

}  // namespace cromwell
