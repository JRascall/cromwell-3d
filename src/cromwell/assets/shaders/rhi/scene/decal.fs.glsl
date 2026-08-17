#version 450 core
/* rhi/scene/decal.fs.glsl — project one decal into the DBuffer.
 *
 * Converted from ../../decal.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * WHAT THE CONVERSION CHANGED, and it is a short list because the technique is
 * the same one: the uniforms became two std140 blocks at the pass and object
 * frequencies (rhi/include/decal_blocks.glsl), the maps became explicit
 * bindings rather than raylib material map slots, matModel became a member of
 * the object block rather than something DrawMesh set, and the depth
 * unprojection dropped its -1..1 remap because this renderer runs at 0..1 clip
 * depth. Everything else below — the wrap, the fold, the angle fade, the
 * gradient clamp, the premultiplied output — is the original, and so are its
 * notes.
 *
 * WHAT THIS PASS IS. For every pixel inside the projector box: recover the real
 * surface that is actually there by unprojecting the depth buffer, ask whether
 * that surface lies inside the box and faces the projector, and if so write the
 * decal's MATERIAL — not its lighting — into the three DBuffer planes. The lit
 * pass then blends those planes over its own material inputs and lights the
 * result once. See rhi/include/dbuffer.glsl for the plane layout and the blend.
 *
 * WHY IT WRAPS. Nothing here knows what shape the receiver is. The UV comes
 * from where the recovered surface sits inside the box, so a decal spanning a
 * kerb, a stair nose and the road either side inks all three continuously, and
 * no geometry was clipped, generated or offset to do it. A decal cannot
 * z-fight, because no coplanar triangle is ever submitted — the box is a
 * bounding volume and its own faces are never shaded.
 *
 * WHY IT DOES NOT SMEAR. Projection alone is not enough: a surface nearly
 * parallel to the projection axis occupies almost no area in the projected UV,
 * so one column of texels stretches down its entire length. That is the
 * signature artefact of this technique and the angle fade below is the only
 * cure — there is no filter that turns a one-texel streak back into a decal.
 *
 * ONE DRAW PER DECAL, BACK FACES ONLY, DEPTH TEST OFF. Front faces would be
 * clipped away the moment the camera entered the box, which for a large ground
 * decal is most of the time; back faces are always present and always cover the
 * box's full screen extent. The depth test has nothing left to decide once the
 * box bounds are tested here, and leaving it on would reject the very fragments
 * whose receiver is in front of the box's far side.
 */
#include "rhi/include/decal_blocks.glsl"

/* ---- the DBuffer planes, in DecalBuffer's order ------------------------- */
layout(location = 0) out vec4 outAlbedo;    /* rgb premultiplied, a = 1 - cov */
layout(location = 1) out vec4 outNormal;    /* world normal encoded, likewise */
layout(location = 2) out vec4 outSurface;   /* metal, rough, emissive mask    */

/* ---- the frame: what the prepass already put on screen ------------------ */
layout(binding = 0) uniform sampler2D uSceneDepth;     /* window-space depth  */
layout(binding = 1) uniform sampler2D uSceneNormals;   /* n * 0.5 + 0.5       */

/* ---- this decal's maps -------------------------------------------------- */
layout(binding = 2) uniform sampler2D uDecalAlbedo;    /* sRGB, ALPHA IS COVERAGE */
layout(binding = 3) uniform sampler2D uDecalPacked;    /* metal R, rough G, emis B */
layout(binding = 4) uniform sampler2D uDecalNormal;    /* tangent space, linear  */

/* ---- what this decal could SEE from where it was thrown ------------------
 * One cube per decal, holding the distance to the nearest surface in every
 * direction, rendered when the decal was placed. See the test in main() and
 * rhi/scene/decal_visibility.vs.glsl for what fills it and why it exists. */
layout(binding = 5) uniform samplerCubeArray uDecalVisibility;

void main()
{
    vec2 screen = gl_FragCoord.xy / uResolution.xy;

    /* THE SKY IS NOT A SURFACE. At the far plane the unprojection below is
     * still perfectly well defined and lands a decal's worth of ink on a point
     * kilometres away, which the box test would usually but not always reject.
     * Rejecting it here is both cheaper and certain. */
    float depth = texture(uSceneDepth, screen).r;
    if (depth >= 1.0) discard;

    /* Window space -> NDC -> world. The inverse view-projection is rebuilt on
     * the CPU from the same camera the prepass used rather than read back out
     * of rlgl, because this pass runs inside a 3D mode whose matrices rlgl
     * holds in its own stack — the same reason AmbientOcclusion rebuilds it. */
    /* 0..1 CLIP DEPTH, NOT -1..1, AND THIS IS THE ONE LINE THE CONVERSION HAD
     * TO CHANGE. The raylib original remaps depth into -1..1 because that is
     * the convention its context runs at; this renderer sets glClipControl to
     * zero-to-one, so the stored value IS the NDC z. Doing the remap anyway
     * puts every recovered world position at the wrong distance, which reads as
     * decals floating off their surfaces rather than as a depth convention.
     * See the glClipControl entry in rhi/MIGRATION.md 5. */
    vec4 clip  = vec4(screen * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInverseViewProjection * clip;
    world.xyz /= world.w;

    /* Into the decal's own space, where the box is the unit cube. */
    vec3 local = (uInverseModel * vec4(world.xyz, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    /* ---- the receiver ---------------------------------------------------- */
    vec3 receiver = normalize(texture(uSceneNormals, screen).rgb * 2.0 - 1.0);

    /* ============ COULD THIS SURFACE BE SEEN FROM WHERE THE DECAL WAS
     * ============ THROWN? IF NOT, IT TAKES NO INK.
     *
     * ONE FETCH AND ONE COMPARE, and it replaces two tests that were guesses.
     *
     * THE PROBLEM IT SOLVES. The box test above bounds `local` and nothing
     * else, so the box contains every surface inside it — in a corner that
     * includes the far side of the adjoining wall, 9 cm away, with a normal
     * perpendicular to the projection axis and therefore as square to one of
     * the box's axes as the near face is. The angle fade cannot tell them
     * apart, because it is a test against the box's AXES and both faces pass
     * it. On a flat wall no such face exists, which is why the artefact only
     * ever appeared in corners.
     *
     * WHY THE TWO OBVIOUS TESTS BOTH FAILED, since they are the ones anybody
     * reaches for first and both shipped here briefly. Rejecting a surface
     * whose UNWRAP DOUBLES BACK over the decal's own area catches the far face
     * of a wall in front of the placement plane and misses it behind, where the
     * fold sign flips and takes the unwrap's direction with it. Rejecting a
     * surface the decal's CENTRE IS BEHIND catches both — and also catches
     * every stair riser, kerb edge and outside corner, because a fold that
     * turns away from the decal has exactly the same shape as a wall seen from
     * the wrong side. THAT IS NOT A TUNING PROBLEM. A riser and the back of a
     * wall present identical normals at identical angles; the only difference
     * between them is whether anything is standing in the way, and no function
     * of a normal and a position can see that.
     *
     * SO ASK THE GEOMETRY INSTEAD. When the decal is placed, the world around
     * it is rendered from its own position into one cube of distances. A
     * surface is inked only if it is no further away than what that capture saw
     * in its direction — which is to say, only if it was VISIBLE from the point
     * the decal was thrown at. The far side of a wall is not: the wall itself
     * is in the way, at a shorter distance, in that exact direction. A stair
     * riser is. So is a kerb, a crate, a beam, a rubble pile, and anything else
     * a game puts in a room, at any orientation, with no notion of what any of
     * them are.
     *
     * THIS IS SOURCE'S RULE — the surfaces reachable from the impact point,
     * `UTIL_DecalTrace` handing the decal to the entity the trace actually hit
     * — reached with a rasteriser rather than a triangle list, and it asks
     * nothing of the game. Valve's own name for the artefact is in their API
     * (`AddDecal(..., bool noPokethru, ...)`) and their cure where they project
     * is an extra clip plane, "used to prevent pokethru and back-casting"
     * (public/engine/ishadowmgr.h). This is that idea with the planes replaced
     * by a depth buffer, so it costs nothing to be exact about geometry no
     * plane could describe.
     *
     * THE SLACK IS TWO CENTIMETRES AND STAYS SMALL. The capture's resolution
     * error is handled by moving the LOOKUP rather than by loosening the
     * threshold — see the normal offset below, and the note there on why no
     * value of a depth bias can satisfy both corners and floors. What is left
     * for this to cover is the placement surface itself, whose own value in the
     * capture is the thing being compared.
     *
     * The reasoning that got here, kept because it is the trap:
     *
     * This is ordinary shadow acne and it has the ordinary cause. One texel of
     * a cube face covers an ARC, so it covers more world the further out it is,
     * and vastly more of a surface seen edge-on than one seen square: at
     * grazing incidence a single texel spans a long strip of wall whose near
     * and far ends are centimetres apart in distance, and the one value stored
     * for it is right for a point somewhere in the middle and short for
     * everything beyond. Tested against a tight bias, half the strip rejects
     * itself and the mark breaks up — worst exactly where a wrap needs the
     * capture most, because the adjoining wall of a corner is the most grazing
     * surface in the box.
     *
     * So the slack is the width of a texel AT THIS DISTANCE, divided by how
     * square the receiver is to the ray. Head-on it is one texel; edge-on it
     * opens up to twenty, which is not generosity but the honest resolution of
     * the answer being tested. `uWrap.z` carries the arc a texel subtends so
     * this cannot drift from the capture's actual size.
     *
     * The two centimetres on the end is the placement surface itself: every ray
     * along it grazes it, so the capture's own value there IS the thing being
     * compared. See the origin offset in the capture shader for the other half.
     *
     * uWrap.y IS ZERO WHEN THERE IS NO CAPTURE, and then every surface inside
     * the box takes ink — the pass's old behaviour. A decal that could not be
     * given a slice is better slightly wrong than absent, and the count of them
     * is a budget question rather than a correctness one. */
    /* THE FALLBACK, AND CURRENTLY THE PATH THAT RUNS. Reject a receiver whose
     * plane the decal's centre sits behind. It is the cheap approximation the
     * capture is meant to replace — right for walls, wrong for every fold that
     * turns away from the decal — and it is here because it is KNOWN GOOD on
     * corners, which the capture is not yet. See uWrap.y: the pipeline stops
     * setting it while the capture is being diagnosed, and this runs instead. */
    if (uWrap.y < 0.5 && dot(receiver, uModel[3].xyz - world.xyz) < -0.02) discard;

    if (uWrap.y > 0.5) {
        /* THE LOOKUP MOVES, NOT THE THRESHOLD, and that distinction is the
         * whole difference between this working and breaking every corner.
         *
         * A DEPTH BIAS CANNOT WORK HERE, and it was tried. One texel of the
         * capture covers an arc, so it covers a long strip of a surface seen
         * edge-on, and the single distance stored for that strip is short for
         * everything past its middle. Sizing a bias to cover that means sizing
         * it to the strip — which at grazing incidence is tens of centimetres,
         * far more than the 6 cm a floor is thick. So the bias that stops
         * corners breaking up is the same bias that lets ink through a floor:
         * the two requirements meet in the middle at "impossible", and no
         * amount of tuning finds a value that satisfies both.
         *
         * SO OFFSET THE SAMPLE ALONG THE RECEIVER'S NORMAL INSTEAD — the normal
         * offset every shadow map ends up using, for the identical reason. The
         * test point is lifted off its own surface by about a texel, scaled up
         * as the surface turns edge-on, so a grazing surface stops shadowing
         * itself. THE DIRECTION IS WHAT MAKES IT SAFE: the offset runs along
         * the surface's own normal, never toward the projector, so it lifts a
         * receiver clear of ITSELF and never out from behind something else. A
         * point on the wall below a floor still has the floor between it and
         * the decal after the offset, so it stays rejected — which is exactly
         * the case a fat bias got wrong. */
        vec3  toward     = normalize(world.xyz - uCapture.xyz);
        float square     = abs(dot(receiver, toward));
        float texelWorld = length(world.xyz - uCapture.xyz) * uWrap.z;

        /* CLAMPED BELOW THE THINNEST SOLID, and this is the other half of what
         * makes an offset safe rather than a bias in disguise.
         *
         * The offset grows with distance, because a texel does — at a couple of
         * metres and grazing incidence it reaches fifteen centimetres, which is
         * more than the 9 cm a wall is thick. A surface in the next room facing
         * back toward the decal is then lifted THROUGH that wall, lands in front
         * of it, and takes ink: a fragment of the mark on a wall it was never
         * thrown at, which is exactly the artefact this test exists to remove.
         *
         * Four centimetres is under a floor's 6 and a wall's 9, so no offset can
         * ever cross one. It costs the grazing case at long range, where the
         * offset is no longer big enough to lift a surface off itself — that
         * shows up as a decal thinning out at its far edge, which is a fade,
         * not a mark in another room. */
        float lift       = min(texelWorld * (0.5 + 2.0 * (1.0 - square)), 0.04);
        vec3  sampleAt   = world.xyz + receiver * lift;

        vec3  fromOrigin = sampleAt - uCapture.xyz;
        float reach      = length(fromOrigin);
        float visible    = texture(uDecalVisibility, vec4(fromOrigin, uCapture.w)).r;

        /* CLAMPED AT A TWENTIETH, which is about 87 degrees off square. Past
         * that the surface is edge-on to the capture and no bias makes the
         * stored value meaningful; letting the term run to infinity there would
         * switch the test off on exactly the surfaces most likely to be behind
         * something. */
        if (reach > visible + 0.02) discard;
    }

    /* The projector's own frame, read straight back out of the model matrix's
     * columns: +Z out of the surface, X and Y the decal's U and V. Lengths are
     * the box's extents, which the unwrap below needs — local coordinates are
     * normalised per axis, so a distance along W is only comparable with one
     * along U after being scaled by the ratio of the two. */
    vec3  axisU = uModel[0].xyz, axisV = uModel[1].xyz, axisW = uModel[2].xyz;
    float lenU = length(axisU), lenV = length(axisV), lenW = length(axisW);
    vec3  dirU = axisU / lenU,   dirV = axisV / lenV,   dirW = axisW / lenW;

    float alignU = abs(dot(receiver, dirU));
    float alignV = abs(dot(receiver, dirV));
    float alignW = abs(dot(receiver, dirW));

    /* ---- WHICH WAY TO PROJECT, decided PER PIXEL --------------------------
     * This is the difference between a decal that stops at a corner and one that
     * goes round it, and it is the whole reason the receiver's normal is read.
     *
     * A SINGLE projection direction is what Unreal's DBuffer uses, and it has
     * one fatal case: a surface roughly parallel to that direction occupies
     * almost no area in the projected UV, so one column of texels stretches
     * down its entire length. There is no filter that repairs that, which is
     * why the single-axis path has to REJECT those surfaces outright — and
     * rejecting them is exactly the decal stopping at the corner.
     *
     * Source resolves the projection per receiving FACE instead, so a mark on a
     * corner lands on both faces, each unwrapped from the edge they share. That
     * is what this does, per pixel rather than per face: pick the box axis the
     * receiver most faces, project along it, and carry the coordinate of the
     * DROPPED axis across the fold by the distance travelled from it. At the
     * fold the two agree exactly, so the texture is continuous across the
     * corner; past it, it climbs the wall as though the sticker had been folded
     * over the edge.
     *
     * THE FOLD IS local.z == 0, which is the surface the decal was placed on —
     * the box is centred there by construction, so the distance travelled from
     * the fold is |local.z| and the only remaining question is which WAY in UV
     * space that distance runs. See the branch below; it is the one genuinely
     * subtle thing in this shader.
     *
     * NO BLEND BAND BETWEEN THE AXES, and this world is why it is affordable.
     * Selecting an axis per pixel normally needs a cross-fade wherever two are
     * comparable, and cross-fading two different UV sets double-samples the
     * texture and ghosts anything with an edge in it — ruinous for lettering.
     * Here every surface is an axis-aligned box face, so the alignments are 1
     * and 0 and there is no in-between region to blend. Ramps are the one
     * exception and take the dominant axis with some stretch.
     *
     * `facing` is signed on the primary axis and unsigned on the wrapped ones.
     * That is deliberate: a surface facing AWAY along the projection axis is the
     * underside of a floor and must stay clean, while a wall is inked from
     * whichever side of it the depth buffer actually shows. */
    vec2  uv;
    vec3  tangentDir;   /* the world direction the decal's U runs along HERE */
    float facing;

    /* WHICH WAY THE UNWRAP RUNS PAST A FOLD, and it took three wrong answers to
     * get here, so the reasoning is written out.
     *
     * The rule is: away from the region the PLACEMENT face occupies. That face
     * lies on one side of the fold, the wrapped face continues on the other, and
     * the texture must keep going rather than double back. Two candidates look
     * plausible and both are wrong:
     *
     *   `-sign(dot(receiver, axis))` — from which way the wrapped face points.
     *   Correct on concave folds and inverted on convex ones, because it cannot
     *   tell the two apart: the same normal occurs in both.
     *
     *   `sign(local.x)` — from which side of the decal's centre the fold sits.
     *   Correct on both, EXCEPT when the decal is centred on the fold, which is
     *   exactly where anyone aiming at a corner puts it. Every pixel of the
     *   wrapped face then sits at local.x ~= 0, the sign is floating-point noise,
     *   and half of them unwrap each way — one mirrored half, one correct.
     *
     * The missing term is CONCAVE VERSUS CONVEX, and local.z already carries it.
     * The wrapped face extends in front of the placement plane on a concave fold
     * (an inside corner: the adjoining wall comes toward the viewer, local.z > 0)
     * and behind it on a convex one (a building corner or a wall end: it falls
     * away, local.z < 0). So:
     *
     *     side = -sign(dot(receiver, axis)) * sign(local.z)
     *
     * which is the first candidate with the concavity folded in. It reads no
     * position, only a normal and a side — so where the decal is placed relative
     * to the fold stops mattering, and a decal centred exactly on a corner is no
     * longer a special case. Both signs are written as comparisons because
     * sign(0) is 0, and a zero here would unwrap by nothing and stretch one
     * column of texels along the whole face. */
    if (uWrap.x > 0.5 && alignU >= alignV && alignU > alignW) {
        float facingSign = (dot(receiver, dirU) >= 0.0) ? 1.0 : -1.0;
        float foldSign   = (local.z >= 0.0) ? 1.0 : -1.0;   /* + concave, - convex */
        float side  = -facingSign * foldSign;

        float u = local.x + side * abs(local.z) * (lenW / lenU);
        uv = vec2(u + 0.5, 0.5 - local.y);

        /* U now runs along the projection axis. Its world direction is
         * side * foldSign * dirW, and side already carries a foldSign — the two
         * cancel, leaving the normal's sign alone. Only the normal map reads
         * this, but getting it wrong inverts the relief on wrapped faces. */
        tangentDir = dirW * -facingSign;
        facing     = alignU;
    } else if (uWrap.x > 0.5 && alignV > alignW) {
        /* The same rule on the other in-plane axis. U still comes from local.x
         * here, so the tangent is unchanged. */
        float facingSign = (dot(receiver, dirV) >= 0.0) ? 1.0 : -1.0;
        float foldSign   = (local.z >= 0.0) ? 1.0 : -1.0;
        float side  = -facingSign * foldSign;

        float v = local.y + side * abs(local.z) * (lenW / lenV);
        uv         = vec2(local.x + 0.5, 0.5 - v);
        tangentDir = dirU;
        facing     = alignV;
    } else {
        /* The primary face. V runs bottom-to-top in the box and top-to-bottom
         * in the image. */
        uv         = vec2(local.x + 0.5, 0.5 - local.y);
        tangentDir = dirU;
        facing     = dot(receiver, dirW);
    }

    /* THE UNWRAP CAN LEAVE THE TEXTURE, and must be clipped where it does. The
     * box test above bounds `local`, not `uv` — a wrapped coordinate is
     * local.x PLUS the climb, so it runs past the edge long before local.x
     * does. The maps are CLAMP, so without this the outermost column of texels
     * would smear up the wall for as far as the box reaches, which looks
     * exactly like the stretching this whole branch exists to remove. */
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;

    /* What survives is a surface that faces one of the box's axes squarely
     * enough to take ink. On the primary axis this is the anti-smear test it
     * always was; on a wrapped axis the dominant alignment of three orthogonal
     * directions is never below 1/sqrt(3), so it passes and the fade's real job
     * there is only to catch the near-degenerate ramp. */
    float angleFade = smoothstep(uFade.x, uFade.y, facing);
    if (angleFade <= 0.0) discard;

    /* Softens the near and far faces of the box so a decal that only just
     * reaches a surface fades out across it instead of ending in a straight cut
     * — which is the difference between a stain and a rectangle. */
    float depthFade = clamp(uFade.z, 0.001, 0.5);
    float edgeFade = 1.0 - smoothstep(0.5 - depthFade, 0.5, abs(local.z));

    /* ---- the decal's own material ---------------------------------------- */
    /* GRADIENTS ARE CLAMPED, and without it every silhouette in the scene gets
     * a bright fringe. These UVs come from unprojected depth, so across a depth
     * discontinuity two neighbouring pixels land at opposite ends of the decal
     * and dFdx reports a full-texture step — the hardware duly picks the 1x1
     * mip and the decal's average colour appears as a hard line along the edge.
     * Clamping the gradient costs a little aliasing in that one-pixel band and
     * removes the fringe entirely. */
    vec2 dx = clamp(dFdx(uv), -0.125, 0.125);
    vec2 dy = clamp(dFdy(uv), -0.125, 0.125);

    /* NOT named `packed` — it is a reserved word in GLSL, and the driver's
     * complaint about it ("unexpected '=', expecting ';' or '('") names neither
     * the word nor the reason. pbr.fs.glsl carries the same note. */
    vec4 albedo = textureGrad(uDecalAlbedo, uv, dx, dy) * uTint;
    vec3 packedSample = textureGrad(uDecalPacked, uv, dx, dy).rgb;
    vec3 tangentNormal = textureGrad(uDecalNormal, uv, dx, dy).xyz * 2.0 - 1.0;

    /* THE ALPHA CHANNEL IS OPACITY HERE, unlike on a surface material. A decal
     * texture without one is a rectangle, and a rectangle is almost never what
     * a decal is — so there is no mode flag, just coverage. */
    float coverage = albedo.a * uFactors.w * angleFade * edgeFade;
    if (coverage < 0.003) discard;

    /* ---- the normal, in the RECEIVER's frame ------------------------------
     * NOT the projector's, and this is the difference between a decal that
     * follows the surface and one that flattens it. Built from the decal's own
     * U direction projected onto the receiver's own tangent plane, the frame's
     * third axis IS the receiver's normal — so a flat decal normal reproduces
     * the surface exactly, at any angle, and a bumpy one perturbs it from
     * there. Using the projector's frame instead would push every inked pixel
     * to face the projector, turning a decal on a curved surface into a visibly
     * flat patch.
     *
     * The tangent is `tangentDir`, which the projection branch chose — NOT the
     * projector's U axis. On a wrapped face the decal's U runs along the box's
     * W direction, and using the fixed axis there would twist the normal map
     * ninety degrees on exactly the faces the wrap exists to serve. */
    vec3 tangent = tangentDir;
    tangent = tangent - receiver * dot(receiver, tangent);

    /* The projection collapses only where the receiver is edge-on to the
     * decal's U, which the angle fade has already all but rejected; falling
     * back to the unperturbed normal is correct there and costs one branch. */
    vec3 worldNormal = receiver;
    if (dot(tangent, tangent) > 1e-8) {
        tangent = normalize(tangent);
        vec3 bitangent = cross(receiver, tangent);
        tangentNormal.xy *= uFactors.z;
        worldNormal = normalize(mat3(tangent, bitangent, receiver) * tangentNormal);
    }

    /* ---- into the planes --------------------------------------------------
     * Premultiplied by coverage, with the INVERSE of coverage in alpha, because
     * that is the pair the DBuffer's one blend equation consumes:
     *
     *     rgb   = src.rgb + dst.rgb * src.a
     *     alpha =           dst.a   * src.a
     *
     * Overlapping decals therefore compose correctly among themselves and leave
     * dst.a holding exactly the fraction of the base material still visible.
     *
     * ALBEDO STAYS sRGB-ENCODED. The plane is 8 bits and a linear encoding
     * spends most of its codes on brightnesses nothing uses; the lit shader
     * blends against the base albedo BEFORE its own srgbToLinear, so the round
     * trip is exact for a single decal. See common/dbuffer.glsl. */
    float transmit = 1.0 - coverage;

    outAlbedo  = vec4(albedo.rgb * coverage, transmit);
    outNormal  = vec4((worldNormal * 0.5 + 0.5) * coverage, transmit);
    outSurface = vec4(vec3(clamp(packedSample.r * uFactors.y, 0.0, 1.0),
                           clamp(packedSample.g * uFactors.x, 0.0, 1.0),
                           clamp(packedSample.b * uFade.w,    0.0, 1.0)) * coverage,
                      transmit);
}
