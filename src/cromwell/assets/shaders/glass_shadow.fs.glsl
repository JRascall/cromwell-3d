#version 330
/* glass_shadow.fs.glsl - what the sun loses on its way through a window.
 *
 * Glass does not block light, so it must not write depth - a receiver behind a
 * window is lit, not shadowed. But it does not transmit light UNCHANGED
 * either: real glass takes a few percent, tints what is left, and once it is
 * dirty it takes considerably more.
 *
 * That needs somewhere to record "the light reaching here came through glass",
 * and the shadow map already has somewhere: OpenGL forces a colour attachment
 * onto the framebuffer whether or not the pass wants one (see ShadowMap.hpp),
 * so this pass spends the plane that was being thrown away. No second target,
 * no second pass setup, one extra draw of geometry the map already holds.
 *
 * DEPTH TEST ON, DEPTH WRITE OFF. That combination is what makes it correct
 * for free: glass in front of an opaque caster records its tint, glass BEHIND
 * one fails the test and records nothing - because light that was already
 * stopped by a wall never reached the window to be tinted.
 *
 * WHAT THE CHANNEL MEANS: the fraction of sunlight that survived, so the
 * pass's clear value of 1 is exactly right for open air. A clean pane writes
 * its own transmittance; grime multiplies that down, and a fully painted or
 * frosted texel writes 0 and shadows properly. One channel cannot also carry
 * the glass's COLOUR, so the colour is a hue held on the lit shader and picked
 * up in proportion to how much was lost here - see pbr.fs.glsl.
 */
in vec2 vUv;

uniform sampler2D uTranslucencyMap;   /* 1 = clear, 0 = opaque. dirt, frost, patterns */

/* (remapMin, remapMax, uvScale, cleanPaneTransmittance)
 *
 * uvScale has to match uMaterialFactors.w in the lit pass exactly. The pane's
 * UVs are a world-space planar projection, so a mismatch does not merely
 * scale the dirt - it puts the shadow of a streak somewhere the streak is
 * not. */
uniform vec4 uGlassTransmit;

out vec4 finalColor;

void main()
{
    float translucency = texture(uTranslucencyMap, vUv * uGlassTransmit.z).r;
    translucency = mix(uGlassTransmit.x, uGlassTransmit.y, translucency);

    finalColor = vec4(uGlassTransmit.w * translucency, 0.0, 0.0, 1.0);
}
