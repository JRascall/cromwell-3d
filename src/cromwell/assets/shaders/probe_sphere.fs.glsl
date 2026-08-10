#version 400
/* probe_sphere.fs.glsl - one reflection probe, drawn the way every engine draws
 * one: a mirrored ball sitting at the point the cubemap was captured from.
 *
 * WHY A BALL AND NOT A MIRRORED WORLD. The debug view this replaced made every
 * surface in the scene a mirror, which answers "is there anything in the
 * cubemap" and almost nothing else. It cannot show WHERE a probe is, how many
 * there are, or which of them a given room actually got - and those are the
 * questions a probe set raises that a single probe never did. A ball at the
 * capture point answers all three at a glance, which is exactly why Unreal and
 * Source both draw their captures this way.
 *
 * THE SAMPLE IS RAW, NOT PARALLAX-CORRECTED, and that is the point rather than
 * a shortcut. Parallax correction is a function of where the SHADING surface
 * is, and a preview ball is not shading anything - correcting it would show
 * the cubemap as distorted by the ball's own position, which is a picture of
 * the correction rather than of the capture. Raw is what is actually stored,
 * so a face that came out black, mirrored or aimed wrong is visible as itself.
 *
 * 400 for samplerCubeArray, which environment.glsl declares.
 */
#include "common/environment.glsl"

in vec3 vWorldPosition;
in vec3 vNormal;

out vec4 finalColor;

uniform float uProbeLayer;    /* which probe this ball is showing */
uniform float uProbeTint;     /* 0 interior, 1 outdoor - see below */

void main()
{
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPosition - vWorldPosition);
    vec3 R = reflect(-V, N);

    vec4 probe = texture(uEnvironmentMap, vec4(R, uProbeLayer));

    /* Uncovered directions fall back to the analytic sky exactly as a real
     * surface's would, so the ball reads as a chrome ball in the scene rather
     * than as a sphere with holes punched in it. Alpha is coverage - see
     * ReflectionProbeSet.hpp. */
    vec3 sky    = skyIrradiance(R) * uAmbientIntensity;
    vec3 colour = mix(sky, probe.rgb, clamp(probe.a, 0.0, 1.0));

    /* A THIN RIM, tinted by whether the probe is an interior or the outdoor
     * fallback. Without it a chrome ball in a bright scene is very hard to
     * pick out from the geometry behind it, which defeats the "where are my
     * probes" half of the job. Warm for an interior, cool for the outdoors. */
    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);
    vec3  rimColour = mix(vec3(1.0, 0.55, 0.15), vec3(0.25, 0.6, 1.0), uProbeTint);

    /* LINEAR RADIANCE OUT, like every other lit shader here - the tonemap pass
     * is the only stage allowed to think about the screen. The rim is added at
     * a strength that survives tonemapping without blowing out. */
    finalColor = vec4(colour + rimColour * rim * 2.0, 1.0);
}
