#version 450 core
/* sky.fs.glsl — the analytic sky, drawn as one covering triangle before any
 * geometry.
 *
 * Converted from ../sky.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * Not a cubemap. The whole sky is two colour lobes and a sun, evaluated per
 * pixel from the view ray — which costs nothing to author, updates the instant
 * the sun moves, and is exactly the same function the lit shader integrates as
 * its ambient term. One sky, two consumers: what you see behind the geometry is
 * what the geometry is lit by.
 *
 * Output is LINEAR radiance with a sun far brighter than 1, like everything
 * else that goes into the scene target. It is the tone map that turns that into
 * a picture, and the reason the sun can bloom out to white while the sky under
 * it stays blue.
 *
 * ===================== WHAT THE CONVERSION CHANGED =========================
 *
 * fragTexCoord / colDiffuse are gone with the textured quad raylib blitted them
 * with; the loose uniforms became one std140 block; and the near plane in
 * viewRay() moved from -1 to 0, which is the whole of the depth-convention
 * difference and is the one line that silently produces a plausible wrong sky
 * rather than an obvious one.
 */
/* No vertex stage here: the pipeline pairs this with the shared covering
 * triangle in ScenePipeline.cpp, the same one SSAO and the tone map use. */

layout(location = 0) out vec4 outRadiance;

layout(std140, binding = 1) uniform PassBlock {
    mat4 uInverseViewProjection;
    vec4 uResolution;      /* xy = the scene target's size. zw spare */
    vec4 uSunDirection;    /* xyz = the direction light TRAVELS      */
    vec4 uSunColour;
    vec4 uSkyZenith;
    vec4 uSkyHorizon;
    vec4 uSkyGround;
};

/* Unproject the pixel. gl_FragCoord is bottom-up and so is the target, so this
 * needs no flip anywhere.
 *
 * THE NEAR PLANE IS 0, NOT -1. The engine's clip depth runs 0..1 and the GL
 * backend sets glClipControl to match — see Mat4.hpp. Carrying the raylib
 * version's -1 across would unproject the near point from outside the frustum
 * and tilt every view ray, which reads as a sky whose horizon sits at the wrong
 * height rather than as a depth-convention mistake. */
vec3 viewRay()
{
    vec2 ndc = (gl_FragCoord.xy / uResolution.xy) * 2.0 - 1.0;
    vec4 nearPoint = uInverseViewProjection * vec4(ndc, 0.0, 1.0);
    vec4 farPoint  = uInverseViewProjection * vec4(ndc, 1.0, 1.0);
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

void main()
{
    vec3 ray = viewRay();

    /* The gradient is biased toward the horizon rather than linear in height,
     * because the atmosphere's optical depth is — a linear ramp puts the pale
     * band far too high and reads as a backdrop. */
    vec3 sky = mix(uSkyHorizon.rgb, uSkyZenith.rgb, pow(clamp(ray.y, 0.0, 1.0), 0.42));

    /* BELOW THE HORIZON IS NOT uSkyGround. That value is an IRRADIANCE — what a
     * downward-facing surface receives off the ground — and painting it
     * straight onto the screen makes a flat pale slab behind the board, which
     * at a tactical camera is most of the frame. What belongs there is haze:
     * the horizon's own colour, dimmed hard, falling away into the ground tone
     * as the ray tips further down. That reads as distance instead of as a
     * wall, and it stays dark enough for the lit scene to sit against. */
    vec3 haze = mix(uSkyHorizon.rgb * 0.11, uSkyGround.rgb * 0.16,
                    smoothstep(0.0, 0.45, -ray.y));

    vec3 colour = mix(haze, sky, smoothstep(-0.05, 0.02, ray.y));

    float towardSun = max(dot(ray, -uSunDirection.xyz), 0.0);

    /* The disc, and the aureole around it. Two powers rather than one: a single
     * exponent either gives a hard dot with no halo or a smear with no disc,
     * and the halo is most of what reads as haze. */
    colour += uSunColour.rgb * pow(towardSun, 1800.0) * 45.0;
    colour += uSunColour.rgb * pow(towardSun, 6.0) * 0.22;

    outRadiance = vec4(colour, 1.0);
}
