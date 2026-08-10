#version 330
/* sky.fs.glsl - the analytic sky, drawn as one fullscreen rectangle before any
 * geometry.
 *
 * Not a cubemap. The whole sky is two colour lobes and a sun, evaluated per
 * pixel from the view ray - which costs nothing to author, updates the instant
 * the sun moves, and is exactly the same function the lit shader integrates as
 * its ambient term. One sky, two consumers: what you see behind the geometry
 * is what the geometry is lit by.
 *
 * Output is LINEAR radiance with a sun far brighter than 1, like everything
 * else that goes into the scene target. It is the tonemap that turns that into
 * a picture, and the reason the sun can bloom out to white while the sky under
 * it stays blue.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform vec2  uResolution;
uniform mat4  uInverseViewProjection;
uniform vec3  uSunDirection;    /* the direction light TRAVELS */
uniform vec3  uSunColour;
uniform vec3  uZenithColour;
uniform vec3  uHorizonColour;
uniform vec3  uGroundColour;

/* Unproject the pixel. gl_FragCoord is bottom-up and so is OpenGL's NDC, and
 * the scene target is bottom-up too - so this needs no flip anywhere. */
vec3 viewRay()
{
    vec2 ndc = (gl_FragCoord.xy / uResolution) * 2.0 - 1.0;
    vec4 nearPoint = uInverseViewProjection * vec4(ndc, -1.0, 1.0);
    vec4 farPoint  = uInverseViewProjection * vec4(ndc,  1.0, 1.0);
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

void main()
{
    vec3 ray = viewRay();

    /* The gradient is biased toward the horizon rather than linear in height,
     * because the atmosphere's optical depth is - a linear ramp puts the pale
     * band far too high and reads as a backdrop. */
    vec3 sky = mix(uHorizonColour, uZenithColour, pow(clamp(ray.y, 0.0, 1.0), 0.42));

    /* BELOW THE HORIZON IS NOT uGroundColour. That value is an IRRADIANCE - what
     * a downward-facing surface receives off the ground - and painting it
     * straight onto the screen makes a flat pale slab behind the board, which
     * at a tactical camera is most of the frame. What belongs there is haze:
     * the horizon's own colour, dimmed hard, falling away into the ground tone
     * as the ray tips further down. That reads as distance instead of as a
     * wall, and it stays dark enough for the lit scene to sit against. */
    vec3 haze = mix(uHorizonColour * 0.11, uGroundColour * 0.16,
                    smoothstep(0.0, 0.45, -ray.y));

    vec3 colour = mix(haze, sky, smoothstep(-0.05, 0.02, ray.y));

    float towardSun = max(dot(ray, -uSunDirection), 0.0);

    /* The disc, and the aureole around it. Two powers rather than one: a
     * single exponent either gives a hard dot with no halo or a smear with no
     * disc, and the halo is most of what reads as haze. */
    colour += uSunColour * pow(towardSun, 1800.0) * 45.0;
    colour += uSunColour * pow(towardSun, 6.0) * 0.22;

    finalColor = vec4(colour, 1.0);
}
