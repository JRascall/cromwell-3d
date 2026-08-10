#version 330
/* prepass.fs.glsl - the world normal, packed into an RGB8 colour plane.
 *
 * GEOMETRIC, NOT SHADED. This is the flat face normal, before the procedural
 * detail the lit shader perturbs it with, and that is the right one here:
 * SSAO is asking which way the SURFACE faces so it can orient a sampling
 * hemisphere, and grain-scale wobble in that orientation would only add noise
 * to a signal that then gets blurred anyway.
 */
in vec3 vNormal;

out vec4 finalColor;

/* ALPHA IS ROUGHNESS, and it used to be a wasted constant 1.0.
 *
 * Screen-space passes need to know how sharp a surface reflects, and the
 * forward shader that knows cannot tell them — it runs later, per material,
 * into a different target. One float in a channel that was already allocated
 * and already being written is the cheapest possible way to say it: no extra
 * attachment, no extra bandwidth, no extra pass.
 *
 * Pushed per draw by whatever submits the prepass, so it must be set for every
 * material or the previous one's value leaks. Defaults to 1 — fully rough,
 * which reflects nothing — because a surface nobody described should not
 * suddenly turn into a mirror. */
uniform float uPrepassRoughness;

void main()
{
    vec3 normal = normalize(vNormal);

    /* Undersides seen through a cutaway rasterise back-facing, and a hemisphere
     * built around an inward normal samples through the surface it sits on. */
    if (!gl_FrontFacing) normal = -normal;

    finalColor = vec4(normal * 0.5 + 0.5, clamp(uPrepassRoughness, 0.0, 1.0));
}
