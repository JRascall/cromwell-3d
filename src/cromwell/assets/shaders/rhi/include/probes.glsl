/* rhi/probes.glsl — what a surface sees along its reflection vector.
 *
 * The same selection, parallax correction and blend common/environment.glsl
 * gives the raylib path, against a std140 block and a numbered sampler binding
 * instead of loose uniforms. That file cannot be shared with this dialect
 * because it declares `uniform vec3` globals, which CONVENTIONS.md forbids and
 * SPIR-V has no concept of.
 *
 * THE ARGUMENTS ARE THE SAME AND ARE NOT REPEATED IN FULL. Read
 * cromwell/lighting/ReflectionProbeSet.hpp for why there is a probe per room
 * rather than per board, and common/environment.glsl for the long-form version
 * of each function below. What is kept here is the reasoning a reader needs to
 * avoid breaking this file specifically.
 *
 * Requires rhi/scene_block.glsl — uProbeParams.x is the live probe count, and
 * it is ZERO INSIDE A PROBE CAPTURE. That is not an optimisation: a cubemap
 * array cannot be sampled while one of its slices is the colour attachment, and
 * a probe that reflected the probes would compound its own error every sweep.
 */
#ifndef XCOM_RHI_PROBES
#define XCOM_RHI_PROBES

#define MAX_PROBES 16

/* BINDING 0 — the FRAME slot in CONVENTIONS.md's frequency table, and that is
 * the honest frequency: the volumes change when the world does, which is a wall
 * coming down, and not once per pass or per material. The C++ half is
 * ProbeBlockData in DeviceProbeSet.cpp. */
layout(std140, binding = 0) uniform ProbeBlock {
    vec4 uProbeCapture[MAX_PROBES];        /* xyz = capture point, w = transition */
    vec4 uProbeParallaxMin[MAX_PROBES];    /* xyz = box min, w = priority (volume) */
    vec4 uProbeParallaxMax[MAX_PROBES];
    vec4 uProbeInfluenceMin[MAX_PROBES];
    vec4 uProbeInfluenceMax[MAX_PROBES];
};

/* SLOT 4, after the occlusion plane, the shadow map and the transmission
 * plane. The capture pass binds a 1x1 stand-in here rather than leaving the
 * unit holding the array it is drawing into — see DeviceProbeSet::emptyTexture. */
layout(binding = 4) uniform samplerCubeArray uProbeArray;

/* PARALLAX CORRECTION. A cubemap sampled by the raw reflection vector is an
 * environment infinitely far away, so a wall reflected in a window slides about
 * as the camera moves and never sits ON the wall. Intersecting the reflection
 * ray with the probe's box and re-aiming from the CAPTURE POINT at that hit
 * point pins it to real geometry.
 *
 * THE BOX IS THE ROOM'S, NOT THE BOARD'S, and that is the whole reason there is
 * more than one probe. With a board-sized box the re-aim starts at a point in
 * the middle of the map — for a wall's interior face, on the far side of that
 * wall — so the lookup returns geometry the wall is blocking and the wall reads
 * as transparent. Correcting against the room the surface is actually IN means
 * the ray cannot cross a wall to be corrected, because the wall is the box. */
vec3 probeDirection(int probe, vec3 reflection, vec3 worldPosition)
{
    /* AN ENVIRONMENT AT INFINITY, for a probe whose box is not a usable proxy
     * for what it captured. The outdoor volume is the case: board-sized bounds
     * and one capture point in the middle of the map, so the re-aim below
     * travels twenty tiles and can point at the opposite half of the world —
     * geometry behind the camera turning up in a window on the wrong side. That
     * looks exactly like a mirrored cubemap and is not one. Uncorrected the
     * same reflection is merely imprecise, which the eye reads as distance. */
    if (uProbeParallaxMax[probe].w <= 0.0) return reflection;

    /* Guarded against a component of exactly zero — a reflection parallel to a
     * box face has no intersection with it, and 1/0 would poison the min. */
    vec3 safe = mix(reflection, vec3(1e-5), lessThan(abs(reflection), vec3(1e-5)));
    vec3 inverseRay = 1.0 / safe;

    /* CLAMPED INTO THE BOX FIRST, because what follows is the box's EXIT
     * intersection and that is only defined from INSIDE it. A fragment outside
     * yields a NEGATIVE distance, and the hit then walks BACKWARDS along the
     * reflection ray into a completely different part of the cubemap — silently,
     * because a plausible wrong direction still returns a plausible colour.
     *
     * It is reachable: a pane sits astride a room boundary, so half its
     * thickness is outside the box its inner face selects, and a floor at the
     * box's own minimum is on the boundary exactly. The clamp moves the origin
     * by at most that overhang — four hundredths of a tile — which is nothing
     * against a reflection and is the difference between defined and not. */
    vec3 origin = clamp(worldPosition, uProbeParallaxMin[probe].xyz,
                        uProbeParallaxMax[probe].xyz);

    vec3 toMaximum = (uProbeParallaxMax[probe].xyz - origin) * inverseRay;
    vec3 toMinimum = (uProbeParallaxMin[probe].xyz - origin) * inverseRay;

    /* The forward intersection on each axis, then the nearest of the three.
     * Floored at zero: the clamp above makes this non-negative in exact
     * arithmetic, and a fragment sitting on a face with the ray pointing out of
     * it lands on the boundary where rounding decides the sign. */
    vec3 furthest = max(toMaximum, toMinimum);
    float distanceToBox = max(min(min(furthest.x, furthest.y), furthest.z), 0.0);

    vec3 hit = origin + reflection * distanceToBox;
    return hit - uProbeCapture[probe].xyz;
}

/* HOW STRONGLY A PROBE CLAIMS A POINT: 1 well inside its influence box, 0
 * outside it, ramped across the transition band at the boundary. The band is
 * what stops a doorway being a hard switch between two completely different
 * reflections — Source 2's edge fade distance, Unreal's box transition
 * distance, the same idea in both. */
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

/* A SURFACE BELONGS TO THE VOLUME IT FACES, NOT THE ONE ITS TRIANGLE SITS IN.
 *
 * A wall is a box 0.09 tiles thick centred on the cell boundary, so its two
 * faces are 0.045 apart — one inside the room and one on the street, with the
 * room boundary between them. Testing the raw fragment position against a box
 * edge at that scale is a coin toss decided by floating point, and it shows up
 * as a seam that flickers along every wall.
 *
 * Stepping along the normal first removes the ambiguity entirely, and states
 * the right rule while doing it: the interior face faces into the room and gets
 * the room's probe, the exterior face faces the street and gets the street's.
 * Nothing has to know how thick a wall is.
 *
 * The step has to clear half the thickest surface (a half-cover block at 0.16,
 * so 0.08) with room to spare, and stay well inside the smallest room — one
 * cell is 1.0 x 1.0 x 0.667, so a quarter tile is comfortable at both ends. */
const float kProbeSurfaceStep = 0.25;

vec3 probeSamplePoint(vec3 worldPosition, vec3 normal)
{
    return worldPosition + normal * kProbeSurfaceStep;
}

/* WHICH PROBE OWNS THIS FRAGMENT, and how firmly.
 *
 * ============== HIGHEST PRIORITY WINS, THEN DEEPEST INSIDE ================
 *
 * Every interior room sits inside the outdoor volume, so containment alone is
 * ambiguous and something has to break the tie.
 *
 * THIS USED TO BE "SMALLEST BOX WINS", with uProbeParallaxMin.w carrying the
 * influence box's VOLUME. That is a reasonable default and it cannot express
 * the two things a level designer needs: it has no way to say "this volume
 * overrides that one" when the two are the same size, and it therefore forbids
 * OVERLAPPING volumes — two equal volumes tie, and the tie fell to whichever
 * came first in the array rather than to the box the fragment sits deeper
 * inside. Overlapping volumes with an explicit priority is what Source 2's
 * env_cubemap_box gives a designer, and it is the model here now.
 *
 * So uProbeParallaxMin.w is an EXPLICIT priority and HIGH wins. Ties break on
 * influence weight, which is depth inside the box — so two overlapping volumes
 * of equal priority hand the fragment to whichever one it is further inside,
 * and crossfade across the overlap rather than popping at an array index.
 *
 * SELECTION IS PER-PIXEL, and that is forced rather than chosen. The static
 * world is batched per storey and per material, so one draw call is every wall
 * on a floor and a wall is a single box carrying BOTH its faces. The interior
 * face and the exterior face need different probes and are in the same draw, so
 * no per-draw uniform can separate them. A test against the fragment's position
 * can, and gets the two faces right for free.
 *
 * Two probes are tracked, not one: the winner and the best OTHER candidate, so
 * a fragment in a doorway's transition band can blend between the room it is
 * leaving and the one it is entering rather than snapping at the threshold. */
bool selectProbes(vec3 worldPosition, out int best, out int second, out float blend)
{
    best   = -1;
    second = -1;
    blend  = 0.0;

    /* NEGATIVE infinity now, because high wins. */
    float bestPriority   = -1e30;
    float secondPriority = -1e30;
    float bestWeight     = 0.0;
    float secondWeight   = 0.0;

    int count = min(int(uProbeParams.x), MAX_PROBES);

    for (int i = 0; i < count; i++) {
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

/* HOW MANY LEVELS THE PREFILTERED CHAIN HAS. The C++ half is
 * DeviceProbeSet::kMipLevels and the two are one contract written twice. */
#define PROBE_MIP_LEVELS 6

/* One probe's radiance along a reflection vector, and its coverage, AT THE
 * LEVEL THAT MATCHES THIS ROUGHNESS.
 *
 * Level L holds the probe GGX-convolved for roughness L/(levels-1), so the
 * mapping is a multiply — see rhi/probe_prefilter.fs.glsl, which is what makes
 * that true. Before the chain existed this was a plain texture() and the whole
 * term was faded out to the analytic sky as roughness rose, because a mirror
 * reflection handed to a rough surface is worse than no reflection. */
vec4 sampleProbe(int probe, vec3 reflection, vec3 worldPosition, float roughness)
{
    vec3 direction = probeDirection(probe, reflection, worldPosition);
    float level = clamp(roughness, 0.0, 1.0) * float(PROBE_MIP_LEVELS - 1);
    return textureLod(uProbeArray, vec4(direction, float(probe)), level);
}

/* WHAT A SURFACE SEES ALONG ITS REFLECTION VECTOR — the probe where one claims
 * this fragment, the analytic sky where none does.
 *
 * ONE BLEND, AND IT IS THE PROBE'S ALPHA: geometry where the capture found
 * some, analytic sky where it saw open air — which is what keeps the sky
 * gradient smooth without the sky ever being rendered into the cubemap.
 *
 * THERE USED TO BE A SECOND, ON ROUGHNESS, and this paragraph outlived it. With
 * no prefiltered chain the only level available was a mirror, so the whole term
 * slid back to the analytic sky as roughness rose rather than hand a rough
 * surface a reflection it could never produce. The chain exists now
 * (DeviceProbeSet::kMipLevels, rhi/probe_prefilter.fs.glsl) and sampleProbe
 * reads roughness straight off as a LOD, so the fade is gone — see the note at
 * the end of this function, which is where the argument for deleting it lives.
 *
 * THE PROBE IS NOT SCALED BY THE AMBIENT INTENSITY AND THE SKY IS, and they are
 * not the same kind of quantity. skyIrradiance returns lobe colours that need a
 * fudge factor to land at a plausible irradiance — that factor is
 * uExposureAndAmbient.y, currently 0.42. The cubemap holds RADIANCE the lit
 * pass actually computed, sun and shadows and all. Multiplying it by 0.42 as
 * well attenuates a physically captured reflection for no reason, and on a
 * dielectric already reflecting 4% head-on that is the difference between a
 * faint reflection and an invisible one. So the intensity is applied HERE, to
 * the sky term only, and the caller must not apply it again.
 *
 * Requires rhi/sky.glsl for skyIrradiance. */
vec3 environmentSpecular(vec3 reflection, vec3 worldPosition, vec3 normal,
                         float roughness, float skyIntensity)
{
    vec3 sky = skyIrradiance(reflection) * skyIntensity;
    if (uProbeParams.x <= 0.0) return sky;

    /* SELECTED at the offset point, so the surface picks the room it faces;
     * PARALLAX-CORRECTED from the real one, because that is genuinely where the
     * reflection ray leaves the surface. */
    int best;
    int second;
    float blend;
    if (!selectProbes(probeSamplePoint(worldPosition, normal), best, second, blend))
        return sky;

    vec4 probe = sampleProbe(best, reflection, worldPosition, roughness);

    /* THE SECOND PROBE IS SAMPLED, NOT GUESSED. Blending the two rooms'
     * radiance is the point of tracking a runner-up at all — crossfading
     * between a room's reflection and the analytic sky instead would make every
     * doorway flash bright as you walked through it. */
    if (second >= 0 && blend < 1.0)
        probe = mix(sampleProbe(second, reflection, worldPosition, roughness), probe, blend);

    /* AND NO ROUGHNESS FADE ANY MORE. This used to end with
     *
     *     return mix(world, sky, smoothstep(0.12, 0.55, roughness));
     *
     * which slid the entire probe out to the analytic sky by roughness 0.55.
     * That was an honest stand-in for a chain that did not exist — with only a
     * mirror level available, handing a rough surface a mirror is worse than
     * handing it a gradient — and it became load-bearing enough that the probe
     * face size was justified by it.
     *
     * Deleting it IS the payoff for the prefilter. A rough surface now reflects
     * a blurred version of its actual surroundings, which is what it is supposed
     * to do and what every engine ships; the sky still comes through wherever
     * the capture found open air, by the coverage alpha, at every level. */
    return mix(sky, probe.rgb, clamp(probe.a, 0.0, 1.0));
}

#endif
