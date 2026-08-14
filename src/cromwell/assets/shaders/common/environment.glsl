/* common/environment.glsl - the sun and the sky, as every lit family sees them.
 *
 * These uniforms are FRAME state, not material state: one sun, one sky, pushed
 * once per frame by PbrShader::updateEnvironment. Any shader family that
 * includes this file gets them under the same names, so a water shader and the
 * surface shader cannot disagree about where the sun is.
 *
 * The ambient term is a two-lobe hemisphere rather than a real prefiltered
 * environment: sky above, bounce below. It is the cheap stand-in for the
 * irradiance half of an IBL, and the thing a light probe bake would replace.
 */
#ifndef XCOM_COMMON_ENVIRONMENT
#define XCOM_COMMON_ENVIRONMENT

uniform vec3  uCameraPosition;
uniform vec3  uSunDirection;    /* the direction light TRAVELS */
uniform vec3  uSunColour;
uniform vec3  uZenithColour;
uniform vec3  uHorizonColour;
uniform vec3  uGroundColour;
uniform float uAmbientIntensity;

/* The same two-lobe sky the SkyPass draws, sampled as irradiance. Using the
 * normal directly is the usual hemisphere-ambient shortcut: it is a cosine
 * lobe's worth of sky, without integrating one. */
vec3 skyIrradiance(vec3 direction)
{
    float up = direction.y;
    vec3 sky = mix(uHorizonColour, uZenithColour, clamp(up, 0.0, 1.0));
    return mix(uGroundColour, sky, smoothstep(-0.3, 0.2, up));
}

/* ---- the reflection probes ------------------------------------------------
 * One cubemap per ROOM, in a cubemap array with a layer each. See
 * ReflectionProbeSet.hpp for why there is more than one and why each carries
 * two boxes rather than one. */
#define MAX_PROBES 16

uniform samplerCubeArray uEnvironmentMap;
uniform int  uProbeCount;                      /* 0 = no probes, analytic sky */
uniform vec4 uProbeCapture[MAX_PROBES];        /* xyz = capture point, w = transition */
uniform vec4 uProbeParallaxMin[MAX_PROBES];    /* xyz = box min, w = priority (volume) */
uniform vec4 uProbeParallaxMax[MAX_PROBES];
uniform vec4 uProbeInfluenceMin[MAX_PROBES];
uniform vec4 uProbeInfluenceMax[MAX_PROBES];

/* PARALLAX CORRECTION. A cubemap sampled by the raw reflection vector is an
 * environment infinitely far away, so a wall reflected in a window slides
 * about as the camera moves and never sits ON the wall. Intersecting the
 * reflection ray with the probe's box and re-aiming from the CAPTURE POINT at
 * that hit point pins it to real geometry.
 *
 * THE BOX IS NOW THE ROOM'S, NOT THE BOARD'S, and that is the whole fix. With
 * a board-sized box the re-aim started at a point in the middle of the map —
 * for a wall's interior face, on the far side of that wall — so the lookup
 * returned geometry the wall was blocking and the wall read as transparent.
 * Correcting against the room the surface is actually IN means the ray can no
 * longer cross a wall to be corrected, because the wall is the box.
 *
 * AND THE OUTDOOR VOLUME IS EXEMPT FROM THE CORRECTION ENTIRELY, because it
 * never got a room-sized box — it kept the board. Same bug, one volume,
 * discovered later: geometry behind the camera appearing in a window on the
 * wrong side. See ReflectionProbeSet.hpp. */
vec3 probeDirection(int probe, vec3 reflection, vec3 worldPosition)
{
    /* AN ENVIRONMENT AT INFINITY, for a probe whose box is not a usable proxy
     * for what it captured — which is what a board-sized capture from a single
     * point actually is. Uncorrected the reflection is merely imprecise, and
     * imprecise in a direction the eye reads as distance; corrected, it was
     * precise and pointing the wrong way. */
    if (uProbeParallaxMax[probe].w <= 0.0) return reflection;

    /* Guarded against a component of exactly zero — a reflection parallel to a
     * box face has no intersection with it, and 1/0 would poison the min. */
    vec3 safe = mix(reflection, vec3(1e-5), lessThan(abs(reflection), vec3(1e-5)));
    vec3 inverseRay = 1.0 / safe;

    /* CLAMPED INTO THE BOX FIRST, because what follows is the box's EXIT
     * intersection and that is only defined from INSIDE it. A fragment outside
     * yields a NEGATIVE distance, and the hit then walks BACKWARDS along the
     * reflection ray into a completely different part of the cubemap —
     * silently, because a plausible wrong direction still returns a plausible
     * colour. A pane sits astride a room boundary, so half its thickness is
     * outside the box its inner face selects; the clamp moves the origin by at
     * most that overhang and is the difference between defined and not. */
    vec3 origin = clamp(worldPosition, uProbeParallaxMin[probe].xyz,
                        uProbeParallaxMax[probe].xyz);

    vec3 toMaximum = (uProbeParallaxMax[probe].xyz - origin) * inverseRay;
    vec3 toMinimum = (uProbeParallaxMin[probe].xyz - origin) * inverseRay;

    /* The forward intersection on each axis, then the nearest of the three,
     * floored at zero for a fragment sitting exactly on a face with the ray
     * pointing out of it — where rounding decides the sign. */
    vec3 furthest = max(toMaximum, toMinimum);
    float distanceToBox = max(min(min(furthest.x, furthest.y), furthest.z), 0.0);

    vec3 hit = origin + reflection * distanceToBox;
    return hit - uProbeCapture[probe].xyz;
}

/* HOW STRONGLY A PROBE CLAIMS A POINT: 1 well inside its influence box, 0
 * outside it, ramped across the transition band at the boundary.
 *
 * The band is what stops a doorway being a hard switch between two completely
 * different reflections. Source 2 calls it the edge fade distance and Unreal
 * calls it the box transition distance; it is the same idea in both, and
 * without it a soldier walking inside pops. */
float probeInfluence(int probe, vec3 worldPosition)
{
    vec3 lower = worldPosition - uProbeInfluenceMin[probe].xyz;
    vec3 upper = uProbeInfluenceMax[probe].xyz - worldPosition;

    /* Distance to the nearest face, negative when outside on any axis. */
    vec3 inside = min(lower, upper);
    float depth = min(min(inside.x, inside.y), inside.z);

    float transition = max(uProbeCapture[probe].w, 1e-4);
    return clamp(depth / transition, 0.0, 1.0);
}

/* WHICH PROBE OWNS THIS FRAGMENT, and how firmly.
 *
 * SMALLEST BOX WINS. Every interior room sits inside the outdoor volume, so
 * containment alone is ambiguous and something has to break the tie. Tighter
 * bounds describe a point better than looser ones — the same ordering Unreal
 * uses when it composites captures smallest-last — so the priority carried in
 * uProbeParallaxMin.w is the influence box's VOLUME, and low wins.
 *
 * Two probes are tracked, not one: the winner and the best OTHER candidate, so
 * a fragment in a doorway's transition band can blend between the room it is
 * leaving and the one it is entering rather than snapping at the threshold.
 * Returns false when nothing claims the fragment at all, which on a map with
 * an outdoor probe should never happen — its influence is the whole board. */
/* A SURFACE BELONGS TO THE VOLUME IT FACES, NOT THE ONE ITS TRIANGLE SITS IN.
 *
 * A wall is a box 0.09 tiles thick centred on the cell boundary, so its two
 * faces are 0.045 apart — one inside the room and one on the street, with the
 * room boundary between them. Testing the raw fragment position against a box
 * edge at that scale is a coin toss decided by floating point, and it shows up
 * as a seam that flickers along every wall.
 *
 * Stepping along the normal first removes the ambiguity entirely, and states
 * the right rule while doing it: the interior face faces into the room and
 * gets the room's probe, the exterior face faces the street and gets the
 * street's. Nothing has to know how thick a wall is.
 *
 * The step has to clear half the thickest surface (a half-cover block at 0.16,
 * so 0.08) with room to spare, and stay well inside the smallest room — one
 * cell is 1.0 x 1.0 x 0.667, so a quarter tile is comfortable at both ends. */
const float kProbeSurfaceStep = 0.25;

vec3 probeSamplePoint(vec3 worldPosition, vec3 normal)
{
    return worldPosition + normal * kProbeSurfaceStep;
}

bool selectProbes(vec3 worldPosition, out int best, out int second,
                  out float blend)
{
    best   = -1;
    second = -1;
    blend  = 0.0;

    /* NEGATIVE infinity now, because high wins. */
    float bestPriority   = -1e30;
    float secondPriority = -1e30;
    float bestWeight     = 0.0;
    float secondWeight   = 0.0;

    for (int i = 0; i < uProbeCount && i < MAX_PROBES; i++) {
        float weight = probeInfluence(i, worldPosition);
        if (weight <= 0.0) continue;

        float priority = uProbeParallaxMin[i].w;

        /* HIGHER PRIORITY FIRST, then deeper inside. The second test is what
         * makes overlapping volumes of equal priority usable: without it the
         * winner is whichever the loop reached first, which is an array index
         * and not a fact about where the fragment is. */
        bool beatsBest = priority > bestPriority ||
                         (abs(priority - bestPriority) < 1e-4 && weight > bestWeight);

        if (beatsBest) {
            second         = best;
            secondPriority = bestPriority;
            secondWeight   = bestWeight;

            best         = i;
            bestPriority = priority;
            bestWeight   = weight;
        } else if (priority > secondPriority ||
                   (abs(priority - secondPriority) < 1e-4 && weight > secondWeight)) {
            second         = i;
            secondPriority = priority;
            secondWeight   = weight;
        }
    }

    if (best < 0) return false;

    /* ---- how much of the winner, and how much of the runner-up -----------
     *
     * TWO RULES, because the two cases mean different things.
     *
     * DIFFERENT PRIORITIES — a room inside the outdoors. The winner's own
     * weight IS the blend: at 1 it is fully inside its room and owns the
     * fragment outright; at 0.3 it is a third of the way into the doorway and
     * the volume behind should still be two thirds of the answer.
     *
     * EQUAL PRIORITIES — two overlapping volumes a designer placed side by
     * side. Neither is "behind" the other, so the split is PROPORTIONAL to how
     * far inside each one the fragment is. Using the winner's raw weight here
     * would fade a block out towards its neighbour and never fade the
     * neighbour in, leaving a dark seam down the middle of the overlap. */
    blend = bestWeight;
    if (second >= 0 && abs(bestPriority - secondPriority) < 1e-4)
        blend = bestWeight / max(bestWeight + secondWeight, 1e-4);

    if (second < 0 || secondWeight <= 0.0) blend = 1.0;

    return true;
}

/* One probe's radiance along a reflection vector, and its coverage. */
vec4 sampleProbe(int probe, vec3 reflection, vec3 worldPosition)
{
    vec3 direction = probeDirection(probe, reflection, worldPosition);
    return texture(uEnvironmentMap, vec4(direction, float(probe)));
}

/* What a surface sees along its reflection vector.
 *
 * Two blends, and they do different jobs. The first is the probe's ALPHA:
 * geometry where the capture found some, analytic sky where it saw open air —
 * which is what keeps the sky gradient smooth without rendering the sky into
 * the cubemap. The second is ROUGHNESS: there is no prefiltered mip chain to
 * sample (rlgl cannot build one for a cubemap), so rather than hand a rough
 * surface a mirror-sharp reflection it never could produce, the result slides
 * back to the analytic sky as roughness rises. That is not a fudge — a fully
 * rough reflection converges on the irradiance, and the two-lobe sky is
 * already an irradiance approximation. */
/* THE PROBE IS NOT SCALED BY uAmbientIntensity AND THE SKY IS.
 *
 * They are not the same kind of quantity. skyIrradiance returns lobe colours
 * that need a fudge factor to land at a plausible irradiance — that factor is
 * uAmbientIntensity, currently 0.42. The cubemap holds RADIANCE that the lit
 * pass actually computed, sun and shadows and all. Multiplying it by 0.42 as
 * well attenuates a physically captured reflection for no reason, and on a
 * dielectric already reflecting 4% head-on it is the difference between a
 * faint reflection and an invisible one. So the intensity is applied HERE, to
 * the sky term only, and the caller must not apply it again. */
vec3 environmentSpecular(vec3 reflection, vec3 worldPosition, vec3 normal,
                         float roughness, float skyIntensity)
{
    vec3 sky = skyIrradiance(reflection) * skyIntensity;
    if (uProbeCount <= 0) return sky;

    /* SELECTED at the offset point, so the surface picks the room it faces;
     * PARALLAX-CORRECTED from the real one, because that is genuinely where
     * the reflection ray leaves the surface. */
    int best, second;
    float blend;
    if (!selectProbes(probeSamplePoint(worldPosition, normal), best, second, blend))
        return sky;

    vec4 probe = sampleProbe(best, reflection, worldPosition);

    /* THE SECOND PROBE IS SAMPLED, NOT GUESSED. Blending the two rooms'
     * radiance is the point of tracking a runner-up at all — crossfading
     * between a room's reflection and the analytic sky instead would make
     * every doorway flash bright as you walked through it. */
    if (second >= 0 && blend < 1.0)
        probe = mix(sampleProbe(second, reflection, worldPosition), probe, blend);

    vec3 world = mix(sky, probe.rgb, clamp(probe.a, 0.0, 1.0));

    return mix(world, sky, smoothstep(0.12, 0.55, roughness));
}

#endif
