/* common/shadow.glsl - how much of the sun reaches a point, and in what colour.
 *
 * Two systems meeting at exactly one place, which is Source 2's split: static
 * lattice geometry takes its visibility from a BAKE, everything that moves
 * takes it from the shadow map. bakedSunVisibility answers or declines, and
 * declining is what hands the fragment to the PCSS path.
 *
 * TAKES WORLD POSITION AS A PARAMETER rather than reading a varying. The
 * obvious version reads vWorldPosition directly and works fine - right up to
 * the first shader family that names its varyings differently, at which point
 * a shared include silently stops being shareable. Passing it in costs
 * nothing and keeps this file honest about its inputs.
 *
 * Returns a COLOUR, not a scalar, because light that arrives through glass is
 * both dimmer and differently coloured than light that arrives directly.
 */
#ifndef XCOM_COMMON_SHADOW
#define XCOM_COMMON_SHADOW

#include "common/colour.glsl"

uniform sampler2D uShadowMap;
uniform mat4  uLightViewProjection;
uniform vec2  uShadowTexel;       /* one texel, in shadow map UV */
uniform float uShadowWorldTexel;  /* one texel, in world units   */
uniform float uShadowDepthRange;  /* world units spanned by the depth buffer */
uniform vec2  uShadowSoftness;    /* tan(sun angular radius), max penumbra in texels */
uniform float uShadowStrength;    /* 0 disables shadowing entirely */

/* The shadow map's colour plane: the FRACTION OF SUNLIGHT THAT SURVIVED the
 * path to this texel. 1 is open air, which is also what the pass clears to; a
 * clean pane writes its own transmittance, and grime multiplies that down
 * towards 0.
 *
 * One channel cannot carry a colour as well, so the split is deliberate:
 * uGlassTint is a pure HUE (largest channel 1) and the dimming lives entirely
 * in the plane. uGlassClearPane says how much a clean pane takes, which is
 * what turns "how much was lost" back into "how much of that loss was glass"
 * - the weight the hue is mixed in at. Folding the dimming into the tint too
 * would count it twice. */
uniform sampler2D uShadowTransmission;
uniform vec3      uGlassTint;
uniform float     uGlassClearPane;

/* ---- the baked sun --------------------------------------------------------
 * A bake has no texel grid to staircase against, no depth bias to tune, and
 * its softness is a real area light - which is exactly how Source 2 shadows
 * static geometry, leaving the depth buffer to things that move.
 *
 * There is no lightmap UV channel. The atlas is laid out to match the world -
 * one page per (z, face), one block per (x, y) - so the address is a pure
 * function of world position and surface normal, computed here. */
uniform sampler2D uLightmap;
uniform sampler2D uLightmapIndex;   /* (cell, face) -> patch slot, 16 bit in RG */
uniform vec4  uLightmapParams;      /* texelsPerTile, patchesPerRow, atlasW, atlasH */
uniform vec4  uLightmapGrid;        /* gridW, gridH, gridDepth, cellHeight          */
uniform vec2  uLightmapIndexSize;
uniform float uUseLightmap;         /* 0 for units and props: they stay dynamic     */

/* One shadow sample, bilinearly filtered - COMPARE FIRST, INTERPOLATE AFTER.
 *
 * A plain texture() followed by a compare is BINARY: each tap is lit or it is
 * not, so a 3x3 kernel can only ever return one of ten values. Wherever a
 * shadow edge runs at a shallow angle to the shadow map's texel grid, the
 * staircase it makes has a long period, and those ten plateaus are wide enough
 * on screen to read as regular scallops along what is geometrically a dead
 * straight line. Raising the resolution shortens the scallops but never
 * removes them, because the quantisation is in the FILTER, not the map.
 *
 * Comparing all four neighbours and then interpolating the RESULTS turns the
 * same taps into a continuous ramp. This is what a hardware sampler2DShadow
 * does for free; rlgl exposes no way to set GL_TEXTURE_COMPARE_MODE, so it is
 * done by hand. */
float shadowTap(vec2 uv, float compare)
{
    vec2 coord = uv / uShadowTexel - 0.5;
    vec2 base  = floor(coord);
    vec2 f     = coord - base;
    vec2 uv00  = (base + 0.5) * uShadowTexel;

    float d00 = texture(uShadowMap, uv00).r;
    float d10 = texture(uShadowMap, uv00 + vec2(uShadowTexel.x, 0.0)).r;
    float d01 = texture(uShadowMap, uv00 + vec2(0.0, uShadowTexel.y)).r;
    float d11 = texture(uShadowMap, uv00 + uShadowTexel).r;

    /* step(compare, d) is 1 where the occluder is at least as far as we are,
     * i.e. where this tap is lit. */
    vec4 lit = step(vec4(compare), vec4(d00, d10, d01, d11));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

/* The baked sun for a lattice surface, or -1 when this fragment is not one:
 * a downward face, or geometry outside the grid. The caller falls back to the
 * shadow map, so the two systems meet at exactly one place. */
float bakedSunVisibility(vec3 worldPosition, vec3 normal)
{
    if (uUseLightmap < 0.5) return -1.0;

    float texelsPerTile = uLightmapParams.x;
    float patchesPerRow = uLightmapParams.y;
    vec2  atlasSize     = uLightmapParams.zw;
    float cellHeight    = uLightmapGrid.w;

    /* Step off the surface INTO the cell that owns it. A wall face belongs to
     * the room its normal points into, and the two sides of one wall are lit
     * completely differently - one can be in full sun while the other is
     * indoors. */
    vec3 inside = worldPosition + normal * 0.08;

    float cx = floor(inside.x);
    float cy = floor(inside.z);

    /* Up-facing surfaces ROUND to the nearest cell boundary rather than
     * flooring. A floor slab is drawn at cellBase + floorOffset - artDrop, and
     * artDrop (kerbs, sunken roads) puts it BELOW its own cell base - flooring
     * then attributes the road to the cell underneath, finds no patch there,
     * and silently drops back to the shadow map. Walls genuinely live inside
     * their cell, so they floor. */
    float cz = (normal.y > 0.5) ? floor(inside.y / cellHeight + 0.5)
                                : floor(inside.y / cellHeight);

    if (cx < 0.0 || cy < 0.0 || cz < 0.0) return -1.0;
    if (cx >= uLightmapGrid.x || cy >= uLightmapGrid.y || cz >= uLightmapGrid.z) return -1.0;

    /* Faces, matching SunBaker: 0 floor, then North, East, South, West - where
     * Direction.hpp has N = +z and E = +x, and each patch's normal points back
     * into its own cell. */
    float face;
    vec2  local;
    if (normal.y > 0.5) {
        face  = 0.0;
        local = vec2(inside.x - cx, inside.z - cy);
    } else if (normal.z < -0.5) {
        face  = 1.0;
        local = vec2(inside.x - cx, (inside.y - cz * cellHeight) / cellHeight);
    } else if (normal.x < -0.5) {
        face  = 2.0;
        local = vec2(inside.z - cy, (inside.y - cz * cellHeight) / cellHeight);
    } else if (normal.z > 0.5) {
        face  = 3.0;
        local = vec2(inside.x - cx, (inside.y - cz * cellHeight) / cellHeight);
    } else if (normal.x > 0.5) {
        face  = 4.0;
        local = vec2(inside.z - cy, (inside.y - cz * cellHeight) / cellHeight);
    } else {
        return -1.0;   /* a downward face - not baked */
    }

    /* Where did this surface's patch land? The atlas is packed, so the answer
     * is a lookup rather than arithmetic - the price of not wasting
     * twenty-three parts in twenty-four of the texture on cells that hold no
     * geometry. */
    vec2 indexTexel = vec2(cx * 5.0 + face, cz * uLightmapGrid.y + cy) + 0.5;
    vec2 encoded = texture(uLightmapIndex, indexTexel / uLightmapIndexSize).rg;
    float slot = floor(encoded.r * 255.0 + 0.5) + floor(encoded.g * 255.0 + 0.5) * 256.0;

    if (slot >= 65535.0) return -1.0;   /* no patch here */

    /* Packed neighbours are not world neighbours, so bilinear must not leave
     * the patch - clamp to its inner half texel. */
    vec2 clamped = clamp(local, vec2(0.5 / texelsPerTile), vec2(1.0 - 0.5 / texelsPerTile));

    vec2 slotOrigin = vec2(mod(slot, patchesPerRow), floor(slot / patchesPerRow))
                    * texelsPerTile;

    return texture(uLightmap, (slotOrigin + clamped * texelsPerTile) / atlasSize).r;
}

vec3 sunVisibility(vec3 worldPosition, vec3 normal, float nDotL)
{
    float baked = bakedSunVisibility(worldPosition, normal);
    if (baked >= 0.0) return vec3(mix(1.0, baked, uShadowStrength));

    if (uShadowStrength <= 0.0) return vec3(1.0);

    /* NORMAL OFFSET, not a depth bias alone. Offsetting the lookup along the
     * surface normal moves it to where the shadow map's own texel footprint
     * already agrees with the surface, which kills acne on grazing faces
     * without the peter-panning a depth bias large enough to do the same job
     * would cause. Scaled by the slope, because a face edge-on to the sun is
     * where a whole texel spans the most depth. */
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    vec3 offsetPosition = worldPosition + normal * uShadowWorldTexel * (1.5 + 3.0 * slope);

    vec4 lightSpace = uLightViewProjection * vec4(offsetPosition, 1.0);
    vec3 projected = (lightSpace.xyz / lightSpace.w) * 0.5 + 0.5;

    /* Outside the map is lit, not shadowed - anything beyond the focused
     * projection is treated as open ground. */
    if (projected.z > 1.0) return vec3(1.0);
    if (any(lessThan(projected.xy, vec2(0.0)))) return vec3(1.0);
    if (any(greaterThan(projected.xy, vec2(1.0)))) return vec3(1.0);

    /* PERCENTAGE-CLOSER SOFT SHADOWS.
     *
     * A fixed filter kernel blurs every shadow by the same amount, so a shaft
     * through a door frame is as soft where it meets the frame as it is ten
     * tiles away. Real shadows do the opposite: sharp at contact, widening
     * with distance from the caster, because a light source has ANGULAR SIZE.
     *
     * PCSS recovers that in three steps - find what is blocking, work out how
     * far behind it we are, and size the filter from that.
     *
     * THE TEXTBOOK FORMULA DOES NOT APPLY HERE. The usual
     * (receiver - blocker) / blocker * lightSize comes from similar triangles
     * under a PERSPECTIVE light, where the shadow map's depth is a projected
     * distance from a point. The sun is directional and its map is
     * orthographic: depth is linear and there is no apex to form triangles
     * with. For a light of angular radius t, the penumbra is simply
     * 2 * distance * tan(t) - geometry, not a projection artefact. */

    /* BIAS IN WORLD UNITS, converted to normalised depth here.
     *
     * Written as a raw normalised constant it means whatever the current
     * projection happens to make it mean, and the focused projection refits
     * every frame. Four millimetres of tile, plus a slope term for faces
     * nearly edge-on to the sun where one texel spans the most depth. Too much
     * of this is what lifts a shadow off its caster and lets light seep out
     * from the foot of a wall. */
    const float kWorldBias      = 0.004;
    const float kWorldSlopeBias = 0.020;

    float bias = (kWorldBias + kWorldSlopeBias * slope) / max(uShadowDepthRange, 0.001);
    float compare = projected.z - bias;

    /* A ROTATED POISSON DISC, not a grid.
     *
     * A regular grid of taps is the maximally ORDERED sampling pattern, and an
     * ordered pattern turns undersampling into structure: moire, and edges
     * that step in step with the kernel rather than dissolving. Poisson points
     * are irregular by construction, and rotating the whole disc by a
     * per-pixel hash decorrelates neighbouring pixels - so what is left of the
     * error arrives as noise instead of as a pattern, which the eye forgives
     * and the 2x supersample largely averages away. */
    const vec2 kPoisson[12] = vec2[12](
        vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
        vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
        vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
        vec2( 0.896,  0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

    float angle = hash12(gl_FragCoord.xy) * 6.2831853;
    float cosA = cos(angle);
    float sinA = sin(angle);
    mat2 rotation = mat2(cosA, -sinA, sinA, cosA);

    float tanAngular   = uShadowSoftness.x;
    float maxPenumbra  = uShadowSoftness.y;   /* texels */

    /* ---- 1. blocker search ------------------------------------------------
     * Raw depth reads, no filtering: this is asking "what is in the way and
     * how far in front is it", and an averaged depth across a silhouette
     * would answer with a surface that does not exist. */
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kPoisson[i] * maxPenumbra * uShadowTexel;
        float depth = texture(uShadowMap, projected.xy + offset).r;
        if (depth < compare) {
            blockerSum += depth;
            blockerCount += 1.0;
        }
    }

    /* ---- 2. penumbra from the sun's angular size --------------------------
     * Sized BEFORE the glass lookup, because the edge of a pane wants the
     * same filter width the edge of a shadow does — it is the same sun with
     * the same angular size. With nothing blocking there is no distance to
     * size it from, so it falls back to the one-texel floor. */
    float penumbra = 1.0;
    if (blockerCount >= 0.5) {
        float averageBlocker = blockerSum / blockerCount;
        float distanceWorld = max((projected.z - averageBlocker) * uShadowDepthRange, 0.0);
        penumbra = clamp(2.0 * distanceWorld * tanAngular / uShadowWorldTexel,
                         1.0, maxPenumbra);
    }

    /* ---- how much of the sun's path was glass -----------------------------
     * FILTERED OVER THE SAME DISC AS THE DEPTH. This was a single unjittered
     * tap, on the argument that a pane's edges already sit inside a shadow
     * edge because solid wall bounds the glass band above and below.
     *
     * The band's TOP is bounded that way. Its FOOT is not. A pane stands on
     * its sill and the sill takes the sun right up to the glass, so the lower
     * edge of the transmission footprint lands in full light on a ledge the
     * camera can see along. Point-sampled, that edge is a hard step a whole
     * texel high running at a shallow angle to the texel grid — a staircase
     * along what is a dead straight line, and the reason a serrated fringe
     * traces the inside of every sill.
     *
     * It was also the last unjittered lookup in this function, which is why
     * it alone read as a repeating pattern instead of as noise.
     *
     * Averaging the disc turns the step into a ramp; the per-pixel rotation
     * scatters what the ramp cannot cover — the same trick the depth taps
     * use, for the same reason. */
    float survived = 0.0;
    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kPoisson[i] * penumbra * uShadowTexel;
        survived += texture(uShadowTransmission, projected.xy + offset).r;
    }
    survived /= 12.0;

    /* How much of the loss to read as glass, saturating at a clean pane's own
     * loss: anything dirtier than that is still fully glass, it just passes
     * less. Clamped rather than branched so the ramp across the edge of a
     * pane stays continuous. */
    float glassAmount = clamp((1.0 - survived) / max(1.0 - uGlassClearPane, 0.001),
                              0.0, 1.0);
    vec3  transmission = mix(vec3(1.0), uGlassTint, glassAmount) * survived;

    /* Nothing between us and the sun anywhere in the search disc. */
    if (blockerCount < 0.5) return transmission;

    /* ---- 3. filter at that radius ----------------------------------------
     * Floored at one texel rather than zero: at true contact the penumbra
     * vanishes, and a zero-width filter is a single point sample, which
     * aliases exactly as hard as no filtering at all.
     *
     * SAMPLE COUNT FOLLOWS RADIUS. Twelve taps cover a small disc densely and
     * a large one barely - sample density falls as the square of the radius,
     * so a cap raised without more samples just trades a hard edge for a
     * grainy one. A wide penumbra gets a second inner ring, rotated
     * differently so the two do not line up. */
    float lit = 0.0;
    float taken = 12.0;

    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kPoisson[i] * penumbra * uShadowTexel;
        lit += shadowTap(projected.xy + offset, compare);
    }

    if (penumbra > 8.0) {
        float innerAngle = angle + 1.0472;   /* 60 degrees off the outer ring */
        mat2 innerRotation = mat2(cos(innerAngle), -sin(innerAngle),
                                  sin(innerAngle),  cos(innerAngle));
        for (int i = 0; i < 12; i++) {
            vec2 offset = innerRotation * kPoisson[i] * penumbra * 0.55 * uShadowTexel;
            lit += shadowTap(projected.xy + offset, compare);
        }
        taken = 24.0;
    }

    return vec3(mix(1.0, lit / taken, uShadowStrength)) * transmission;
}

#endif
