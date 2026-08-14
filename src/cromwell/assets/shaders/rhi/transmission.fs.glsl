#version 450 core
/* transmission.fs.glsl — what the sun becomes on the far side of a surface.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ===================== WHAT THIS PLANE IS FOR =============================
 *
 * A translucent surface does not block light, so it must not write depth — a
 * floor behind a window is LIT, not shadowed. But it does not pass light
 * unchanged either: real glass takes a few percent and tints what is left.
 *
 * So the sun's view needs somewhere to record "the light reaching here came
 * through something", and this is it. The lit passes sample it beside the depth
 * map and tint their sunlight by it, which is what makes a window cast a
 * coloured patch on the floor rather than a hole in the shadow.
 *
 * ============== WHY IT CARRIES A COLOUR AND NOT ONE CHANNEL ===============
 *
 * The raylib path stores a single channel — the FRACTION that survived — and
 * keeps the hue as one frame-global uniform, because a one-channel plane cannot
 * hold a colour. That works while there is exactly one glass material and stops
 * working the moment there are two: both would author a `transmissionTint` in
 * their .mat and one would silently win.
 *
 * A material property that is authored per material and consumed globally is
 * not really the material's, which is the same defect as setting a surface's
 * roughness from C++. So this plane is RGBA8 and holds the transmittance
 * COLOUR, and every material means what its file says.
 *
 * IT IS HALF THE SHADOW MAP'S RESOLUTION, which is what pays for the extra
 * channels: quarter the texels times four bytes is the same memory the
 * single-channel plane used at full size. That trade is sound because the two
 * planes answer different questions — depth resolution buys shadow EDGE
 * sharpness, while transmittance is low-frequency by nature (a whole pane is
 * one value) and is filtered over the PCSS disc before anything sees it.
 *
 * =================== DEPTH TESTED BY HAND, AND WHY ========================
 *
 * Being a different size, this cannot be an attachment on the shadow pass —
 * GL would clip that pass to the smaller of the two. So the depth test that
 * would have been free is done here against the shadow map itself.
 *
 * It is exactly the "depth test on, depth write off" the raylib pass relies on,
 * and it earns the same property: glass in FRONT of an opaque caster records
 * its tint, and glass BEHIND one is discarded — because light already stopped
 * by a wall never reached the window to be tinted.
 */
#include "rhi/material_block.glsl"

/* The sun's depth map, at its own full resolution. */
layout(binding = 0) uniform sampler2D uShadowDepth;

layout(location = 0) in vec4 vLightClip;

layout(location = 0) out vec4 outTransmission;

void main()
{
    /* THIS FRAGMENT'S OWN PLACE IN THE SUN'S CLIP SPACE, carried from the
     * vertex stage rather than rebuilt from gl_FragCoord — which would need
     * this target's size, and this target is deliberately not the size of the
     * map being sampled. Sampling the map by UV rather than by texel is what
     * makes the two resolutions independent.
     *
     * z is ALREADY 0..1 — the engine's clip depth convention, which the GL
     * backend sets glClipControl to match. See Mat4.hpp. */
    vec3 projected = vLightClip.xyz / vLightClip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;

    /* A SMALL BIAS, for the same reason the lit pass has one: a pane and the
     * depth recorded for whatever is behind it can be a hair apart, and without
     * a margin the comparison is decided by precision. Generous here because
     * being wrong costs a missing tint rather than acne. */
    const float kBias = 0.0008;

    float occluder = texture(uShadowDepth, uv).r;
    if (projected.z > occluder + kBias) discard;

    /* WHAT SURVIVED, PER CHANNEL. 1 is open air, which is what the pass clears
     * to; a clean pane writes its own transmittance, and an opaque texel writes
     * 0 and shadows properly. */
    outTransmission = vec4(uSunTransmittance.rgb, 1.0);
}
