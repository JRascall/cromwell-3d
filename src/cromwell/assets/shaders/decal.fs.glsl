#version 330
/* decal.fs.glsl — project one decal into the DBuffer.
 *
 * WHAT THIS PASS IS. For every pixel inside the projector box: recover the real
 * surface that is actually there by unprojecting the depth buffer, ask whether
 * that surface lies inside the box and faces the projector, and if so write the
 * decal's MATERIAL — not its lighting — into the three DBuffer planes. The lit
 * pass then blends those planes over its own material inputs and lights the
 * result once. See DecalBuffer.hpp for the plane layout and the blend.
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

/* ---- the DBuffer planes, in DecalBuffer's order ------------------------- */
layout(location = 0) out vec4 outAlbedo;    /* rgb premultiplied, a = 1 - cov */
layout(location = 1) out vec4 outNormal;    /* world normal encoded, likewise */
layout(location = 2) out vec4 outSurface;   /* metal, rough, emissive mask    */

/* ---- the frame: what is already on screen ------------------------------- */
/* Both are the geometry prepass's attachments, and both arrive through the
 * decal material's map slots rather than through SetShaderValueTexture — the
 * pass draws with DrawMesh, which does not flush rlgl's batch and would
 * therefore leave a batch-registered sampler bound to nothing. RibbonShader
 * documents the same trap at length. */
uniform sampler2D uSceneDepth;     /* window-space depth, the prepass's own   */
uniform sampler2D uSceneNormals;   /* rgb = world normal * 0.5 + 0.5          */

/* ---- this decal --------------------------------------------------------- */
uniform sampler2D uDecalAlbedo;    /* sRGB, ALPHA IS COVERAGE     (1x1 white) */
uniform sampler2D uDecalPacked;    /* metal R, rough G, emissive B (1x1 white)*/
uniform sampler2D uDecalNormal;    /* tangent space, linear        (1x1 flat) */

uniform mat4 matModel;             /* unit cube -> world, set by DrawMesh     */
uniform mat4 uInverseModel;        /* and its inverse, which raylib does not  */
uniform mat4 uInverseViewProjection;

uniform vec2 uResolution;          /* of the DBUFFER, which is the prepass's  */
uniform vec4 uTint;                /* multiplies the albedo map               */
uniform vec4 uFactors;             /* rough, metal, normalStrength, opacity   */
uniform vec4 uFade;                /* angleStart, angleEnd, depthFade, emissive */

/* 1 to resolve the projection per pixel and carry the texture round corners,
 * 0 for a single fixed axis. Kept switchable rather than always on because the
 * two are genuinely different looks: a poster is a flat rectangle that should
 * stop at the edge of its wall, and a splatter is a thing thrown at the world
 * that should run over whatever it hits. */
uniform float uWrap;

void main()
{
    vec2 screen = gl_FragCoord.xy / uResolution;

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
    vec4 clip  = vec4(screen * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInverseViewProjection * clip;
    world.xyz /= world.w;

    /* Into the decal's own space, where the box is the unit cube. */
    vec3 local = (uInverseModel * vec4(world.xyz, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    /* ---- the receiver ---------------------------------------------------- */
    vec3 receiver = normalize(texture(uSceneNormals, screen).rgb * 2.0 - 1.0);

    /* ============ THE SURFACE MUST FACE WHERE THE DECAL WAS THROWN =========
     *
     * ONE DOT PRODUCT, AND IT IS THE ONLY THING KEEPING A MARK OUT OF THE NEXT
     * ROOM. The signed distance of the decal's centre from the plane the
     * receiver lies in: positive when the centre is on the side the receiver
     * faces, negative when the receiver has its back to it.
     *
     * WHY IT IS NEEDED, and why a flat wall never showed it. The box test above
     * bounds `local` and nothing else, so the box contains every surface inside
     * it — and inside a CORNER that includes the far side of the adjoining
     * wall, 9 cm away, with a normal perpendicular to the projection axis. The
     * angle fade is a test against the box's AXES, and that face is aligned
     * with one of them exactly as squarely as the near face is, so it takes the
     * same ink. On a flat wall there is no such face and nothing leaks, which is
     * why this only ever appears in corners.
     *
     * WHY SOURCE DOES NOT HAVE THIS PROBLEM: it does not project into a volume.
     * `UTIL_DecalTrace` hands the decal to the entity the trace hit, and
     * `IStudioRender::AddDecal` does a planar projection ALONG THE RAY onto
     * that one model's triangles — a face on the other side of a wall is never
     * enumerated. Valve's own name for the artefact is in the API
     * (`AddDecal(..., bool noPokethru, ...)`) and their cure where they do
     * project is an extra clip plane, "used to prevent pokethru and
     * back-casting" (public/engine/ishadowmgr.h). This is that clip plane,
     * chosen per pixel from the receiver. The device path's copy of this shader
     * carries the long version of the note.
     *
     * THE SLACK IS 2 cm, because the placement surface has the centre exactly
     * in its plane and must survive, while the far face of a 9 cm wall is
     * rejected by a wide margin. It costs the fold that turns AWAY from the
     * decal — down over a kerb edge, round an outside corner — which is the
     * same trade Source's per-face clipping makes. */
    if (dot(receiver, matModel[3].xyz - world.xyz) < -0.02) discard;

    /* The projector's own frame, read straight back out of the model matrix's
     * columns: +Z out of the surface, X and Y the decal's U and V. Lengths are
     * the box's extents, which the unwrap below needs — local coordinates are
     * normalised per axis, so a distance along W is only comparable with one
     * along U after being scaled by the ratio of the two. */
    vec3  axisU = matModel[0].xyz, axisV = matModel[1].xyz, axisW = matModel[2].xyz;
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

    /* HOW FAR A WRAP MAY REACH BEHIND THE SURFACE THE DECAL IS STUCK TO, in
     * world units. It stops a decal on an upper floor wrapping down onto the
     * walls of the storey below — which the plane test above cannot catch,
     * because that wall really does face back toward the decal's centre; what
     * disqualifies it is being on the far side of the floor.
     *
     * IT COSTS NOTHING THAT STILL WORKED: behind the placement plane, the plane
     * test has already rejected every fold that turns away (kerb edge, stair
     * riser, outside corner), so the only ink left back there is on the far side
     * of something solid. Two centimetres is under the 6 cm a floor slab is
     * thick and the 9 cm a wall is, and above the rounding at the fold itself.
     * The primary branch is deliberately not capped — see the device path's copy
     * of this shader for why. */
    const float kSolidReach = 0.02;

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
    if (uWrap > 0.5 && alignU >= alignV && alignU > alignW) {
        float facingSign = (dot(receiver, dirU) >= 0.0) ? 1.0 : -1.0;
        float foldSign   = (local.z >= 0.0) ? 1.0 : -1.0;   /* + concave, - convex */
        float side  = -facingSign * foldSign;

        /* Not through the floor or the wall this decal is stuck to. */
        if (-local.z * lenW > kSolidReach) discard;

        float u = local.x + side * abs(local.z) * (lenW / lenU);
        uv = vec2(u + 0.5, 0.5 - local.y);

        /* U now runs along the projection axis. Its world direction is
         * side * foldSign * dirW, and side already carries a foldSign — the two
         * cancel, leaving the normal's sign alone. Only the normal map reads
         * this, but getting it wrong inverts the relief on wrapped faces. */
        tangentDir = dirW * -facingSign;
        facing     = alignU;
    } else if (uWrap > 0.5 && alignV > alignW) {
        /* The same rule on the other in-plane axis. U still comes from local.x
         * here, so the tangent is unchanged. */
        float facingSign = (dot(receiver, dirV) >= 0.0) ? 1.0 : -1.0;
        float foldSign   = (local.z >= 0.0) ? 1.0 : -1.0;
        float side  = -facingSign * foldSign;

        if (-local.z * lenW > kSolidReach) discard;   /* see the branch above */

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
