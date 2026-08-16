#version 450 core
/* preview_cube.fs.glsl — one probe of the cubemap array, as a strip of six.
 *
 * See assets/shaders/CONVENTIONS.md. Driven by
 * DeviceTexturePreviews::renderCube.
 *
 * ================== WHY THIS EXISTS AND WHY IT IS SEPARATE ================
 *
 * rhi/MIGRATION.md §4.3 leaves "a dev-panel preview of a layer" open with the
 * reason stated: a cubemap ARRAY you cannot look at is worse to debug than a
 * single cubemap, because now there is also the question of which layer is
 * wrong. Six faces side by side answers both at once — whether a face is stale
 * and which probe it belongs to.
 *
 * It is its own shader rather than a mode of preview.fs.glsl because a fragment
 * stage declaring both a sampler2D and a samplerCubeArray needs BOTH bound on
 * every draw; see the note on the two pipelines in DeviceTexturePreviews.hpp.
 *
 * ============= THE FACE TABLE IS GL'S, AND IT IS EASY TO GET WRONG ========
 *
 * The direction below is built from GL's own `sc`/`tc` mapping — the same table
 * DeviceProbeSet::faceViewProjection renders WITH, which is what makes this a
 * preview of the capture rather than a second opinion about it. Four of the six
 * faces have a `tc` that runs DOWNWARD, which is why the vertical term is
 * negated on most rows and not on all of them.
 *
 * Getting a face's orientation wrong here would look exactly like a capture
 * with a mirrored face, and §5 already records an afternoon spent at that face
 * table over a bug that turned out to be a parallax lookup. A preview that can
 * introduce the very artefact it is used to hunt is worse than no preview, so
 * this reads out of the same convention rather than being re-derived.
 */
layout(binding = 0) uniform samplerCubeArray uProbes;

layout(std140, binding = 1) uniform PreviewBlock {
    /* x = mode (unused here; the strip has one interpretation)
     * y = which PROBE, as a cube index rather than an array slice
     * zw = the target's size in pixels */
    vec4 uPreview;

    /* Unread here, and declared anyway: one buffer feeds both preview shaders,
     * and a std140 block that disagrees between two programs about its own
     * members is the offset bug CONVENTIONS.md warns about. Keeping the two
     * declarations identical is what makes that unable to happen. */
    vec4 uDepthRange;
};

layout(location = 0) out vec4 outColour;

void main()
{
    vec2 uv = gl_FragCoord.xy / max(uPreview.zw, vec2(1.0));

    /* Which of the six squares, and where inside it. */
    int face = int(clamp(floor(uv.x * 6.0), 0.0, 5.0));

    /* ---- sc and tc, GL's names, in -1..1 --------------------------------
     *
     * NO FLIP, unlike preview.fs.glsl beside it, and the asymmetry is real
     * rather than an oversight. That one COPIES a target whose first row is at
     * the bottom into one ImGui reads from the top, so it flips. This one
     * SYNTHESISES the image: a cube face's `tc` already runs top-down, and
     * gl_FragCoord.y measures up from a target ImGui also reads from the top —
     * so the two conventions already agree and flipping would introduce the
     * artefact rather than remove it. Both files are right; write the flip
     * where the frames of reference actually differ. */
    vec2 local = vec2(fract(uv.x * 6.0), uv.y) * 2.0 - 1.0;

    vec3 direction;
    if (face == 0)      direction = vec3( 1.0,      -local.y, -local.x);  /* +X */
    else if (face == 1) direction = vec3(-1.0,      -local.y,  local.x);  /* -X */
    else if (face == 2) direction = vec3( local.x,   1.0,      local.y);  /* +Y */
    else if (face == 3) direction = vec3( local.x,  -1.0,     -local.y);  /* -Y */
    else if (face == 4) direction = vec3( local.x,  -local.y,  1.0);      /* +Z */
    else                direction = vec3(-local.x,  -local.y, -1.0);      /* -Z */

    /* LEVEL ZERO EXPLICITLY. The array carries a prefiltered chain, and an
     * implicit lookup here would pick a level from the screen-space derivative
     * of a direction that jumps at every face boundary — so five sixths of the
     * strip would be sharp and the seams would be blurred, which reads as the
     * capture having soft edges. This is a preview of what was CAPTURED. */
    vec4 probe = textureLod(uProbes, vec4(normalize(direction), uPreview.y), 0.0);

    /* ---- what the alpha means, and why the background is magenta ----------
     *
     * The capture clears to TRANSPARENT BLACK and draws geometry only, so alpha
     * is "the world is in this direction" and zero is open sky — see
     * ScenePipeline::drawProbeCapture. Compositing that over black would make
     * open sky and a black wall the same picture, and the difference between
     * them is most of what this preview is for.
     *
     * Magenta because nothing in a lit scene is magenta, which is the whole
     * value of the choice: it can only mean "nothing was drawn here". The
     * raylib probe preview says the same thing in its note. */
    vec3 sky = vec3(1.0, 0.0, 1.0);

    /* Range-compressed exactly as preview.fs.glsl's Hdr mode: the array is
     * RGBA16F radiance, and the two must agree or the same surface reads
     * differently depending on which preview it is being looked at in. */
    vec3 radiance = sqrt(probe.rgb / (1.0 + max(probe.rgb, vec3(0.0))));

    outColour = vec4(mix(sky, radiance, clamp(probe.a, 0.0, 1.0)), 1.0);
}
