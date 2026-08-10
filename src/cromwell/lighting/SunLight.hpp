/* SunLight.hpp — the one directional light, and the sky it hangs in.
 *
 * SINGLE RESPONSIBILITY: turn a time of day into every number the shading
 * needs — a direction, a sun radiance, the three sky colours the ambient term
 * is built from, and the matrix the shadow map is rendered with.
 *
 * ONE ENTITY, TWO CONSUMERS, which is how Source 2's light_environment works:
 * the same sun feeds the shadow pass and the lit pass, so a shadow can never
 * point somewhere the shading does not.
 *
 * COLOUR COMES FROM ELEVATION, not from an authored swatch. Low sun travels
 * through more atmosphere, loses its blue end and goes warm and dim while the
 * sky above it stays cool — so tying both to the elevation angle means the
 * warm/cool split that sells outdoor light falls out of moving one slider,
 * rather than having to be hand-matched every time.
 *
 * Everything returned is LINEAR radiance, not sRGB. Nothing here is display
 * referred; ToneMapPass is the only thing allowed to think about the screen.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class SunLight {
public:
    /* Mid-afternoon and well off the camera axis, so the sun rakes across the
     * tops of walls and reads their depth rather than flattening them.
     *
     * Not lower than this, tempting as it is. Shadow length goes as
     * height/tan(elevation), and a three-storey building at 34 degrees lays
     * nine tiles of darkness down the street — which looks superb and buries
     * the cover the player is trying to read. This is a tactical board before
     * it is a picture, so the elevation is set where the shadows still model
     * the geometry without swallowing the ground the game is played on. */
    static constexpr float kDefaultAzimuth   = 125.0f;
    static constexpr float kDefaultElevation = 48.0f;

    /* HOW BIG THE SUN LOOKS, in radians — Source 2 calls it SunSpreadAngle.
     *
     * This is the one number that decides shadow softness, and it has to be
     * the SAME number for the bake and the shadow map or the two paths
     * disagree about the same sun.
     *
     * NEARLY PHYSICAL, ON PURPOSE. The real sun subtends about 0.0047 rad.
     * This was 0.035 — seven times too big — picked by eye back when a fixed
     * filter kernel was doing the blurring and the number barely mattered.
     * Under PCSS it matters enormously: penumbra is 2 * distance * tan(this),
     * so an oversized sun blurs every shadow into fog, saturates the filter's
     * radius cap so contact hardening stops being visible at all, and spreads
     * twelve samples so thin that the result grains.
     *
     * Slightly above physical, which keeps a hint of softness without the
     * cut-paper look of a true point sun at tile scale. */
    static constexpr float kAngularRadius = 0.0055f;

    /* Peak sun radiance, in the same arbitrary linear units the sky colours
     * are in. What matters is the RATIO between them; ToneMapPass's exposure
     * maps the pair onto the display. */
    static constexpr float kDefaultPeak    = 4.6f;
    static constexpr float kDefaultAmbient = 0.42f;

    /* THE DIALS, not the model. Everything below multiplies or scales what the
     * elevation already decided — the warm/cool split, the extinction curve
     * and the dusk ramps stay where they are, because those are the part that
     * makes moving one slider look like weather rather than like a filter.
     *
     * Defaults ARE the authored look. Reset restores this struct and nothing
     * else, so no amount of dragging can lose it. */
    struct Tuning {
        float   peak    = kDefaultPeak;      /* sun brightness              */
        Vector3 tint    { 1.0f, 1.0f, 1.0f };/* multiplies the sun's colour */
        float   ambient = kDefaultAmbient;   /* how much sky reaches a face */
        Vector3 skyTint { 1.0f, 1.0f, 1.0f };/* multiplies all three sky colours */

        /* How big the sun looks, and so how soft every shadow is. Shared with
         * the bake, which has to be re-run after it changes — the baked path
         * cannot notice on its own. */
        float angularRadius = kAngularRadius;
    };

    Tuning&       tuning()       { return tuning_; }
    const Tuning& tuning() const { return tuning_; }

    float angularRadius() const { return tuning_.angularRadius; }

    float azimuthDegrees() const { return azimuth_; }
    float elevationDegrees() const { return elevation_; }

    void setAzimuth(float degrees);
    void setElevation(float degrees);
    void nudgeAzimuth(float degrees) { setAzimuth(azimuth_ + degrees); }
    void nudgeElevation(float degrees) { setElevation(elevation_ + degrees); }

    /* The direction light TRAVELS — pointing down and away from the sun. The
     * shading wants the opposite; it negates, because "toward the light" is
     * the vector every BRDF is written in terms of. */
    Vector3 travelDirection() const;

    Vector3 radiance() const;         /* the sun's own linear colour x intensity */
    Vector3 zenithColour() const;     /* straight up                             */
    Vector3 horizonColour() const;    /* the band the sun sits in                */
    Vector3 groundColour() const;     /* the bounce a surface facing down sees   */

    /* How much of the sky reaches a surface. One dial over the whole ambient
     * term, so the sun/ambient ratio — the single biggest lever on how harsh a
     * scene reads — can be tuned without touching the colours the sky is
     * DRAWN with.
     *
     * Well under one, and that is not a fudge. The sky colours above are
     * authored to look right as a backdrop, and a backdrop's radiance is
     * nothing like the irradiance it delivers: on a clear day the sun puts
     * roughly six to ten times more light on a horizontal surface than the
     * whole rest of the sky does. Left at one, the sky wins the blue channel
     * outright, every shadow fills in, and the scene reads flat and overcast
     * no matter how bright the sun is made.
     *
     * Nudged back up from the ratio a photograph would want, for the same
     * reason the sun is not lower: a player has to read cover and footing
     * inside a shadow, so shadows keep some sky in them rather than going to
     * black. */
    float ambientIntensity() const { return tuning_.ambient; }

    /* The shadow camera, plus how much world one of its texels covers. The
     * shading needs the second number as badly as the first: normal-offset
     * bias is measured in texels, and a bias in the wrong units is either
     * acne or a shadow that has floated off its caster. */
    struct ShadowProjection {
        Matrix viewProjection;
        float  worldTexelSize;

        /* World units spanned by the projection's depth range.
         *
         * The shader needs it because a depth-buffer comparison bias is in
         * NORMALISED depth, and normalised depth means nothing on its own —
         * the same 0.001 is a millimetre in a tight projection and half a tile
         * in a loose one. Without this the bias silently rescales every time
         * the projection is refitted, which is exactly how a working bias
         * turns into light leaking out from under a wall. */
        float depthRange;
    };

    /* An orthographic box aimed down the sun, fitted to a sphere.
     *
     * A SPHERE, not the box itself, because the sun rotates: a sphere is the
     * only shape whose extent does not change when it does, so texel density —
     * and with it the acne threshold — stays put as the sun sweeps.
     *
     * TEXEL SNAPPING. The projection's origin is quantised to whole shadow
     * texels. Without it, moving the camera slides the texel grid continuously
     * under every static shadow edge and the whole scene crawls. */
    /* `depthExtent` is how far up-sun and down-sun the box reaches, in world
     * units. It must cover any caster that can throw a shadow into the sphere
     * — the world's diagonal always does — and no more, because every unit of
     * slack costs bias precision. */
    ShadowProjection shadowProjectionForSphere(Vector3 centre, float radius,
                                               float depthExtent,
                                               int mapResolution) const;

    /* Fitted to the whole lattice. Simple, and what to fall back to when a
     * focused fit degenerates. */
    ShadowProjection shadowProjectionFor(Vector3 boundsMin, Vector3 boundsMax,
                                         int mapResolution) const;

private:
    float  azimuth_   = kDefaultAzimuth;
    float  elevation_ = kDefaultElevation;
    Tuning tuning_;

    /* 0 at the horizon, 1 overhead — the parameter every colour below is a
     * function of. */
    float dayFactor() const;
};

}  // namespace cromwell
