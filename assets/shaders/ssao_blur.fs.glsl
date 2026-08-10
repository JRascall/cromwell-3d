#version 330
/* ssao_blur.fs.glsl - the box filter that pays off the per-pixel rotation.
 *
 * The occlusion pass rotates its kernel by a hash of the pixel, which turns
 * banding into noise on purpose. This is the second half of that bargain: a
 * 4x4 box exactly the size of the rotation's correlation window averages the
 * noise back out, and because AO is a low-frequency signal nothing of value
 * goes with it.
 *
 * BILATERAL, i.e. depth-aware, and it has to be. A plain box average does not
 * know that two neighbouring pixels can be metres apart in the world, so it
 * carries occlusion straight across a silhouette: the darkening computed for
 * whatever is BEHIND an edge bleeds onto the surface in front of it, and
 * because the thing behind is recognisable the bleed reads as the far geometry
 * showing THROUGH the near surface rather than as a soft edge.
 *
 * The guard is the standard one: skip any tap whose linear depth differs from
 * this pixel's by more than a threshold. Frictional use two centimetres in
 * SOMA; the equivalent here is a fraction of a tile, since a tile is about a
 * metre and a half.
 *
 * The weights are renormalised by how many taps survived, so a pixel beside a
 * silhouette blurs over fewer neighbours rather than darkening because the
 * rejected ones contributed zero.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;      /* the raw occlusion */
uniform vec4 colDiffuse;

uniform sampler2D uDepth;        /* the same G-buffer depth the occlusion read */
uniform mat4  uInverseProjection;
uniform vec2  uResolution;

/* A quarter of a tile. Wide enough that a wall's own gentle recession is still
 * blurred as one surface, tight enough that anything across a silhouette is a
 * different surface and is dropped. */
const float kDepthThreshold = 0.25;

float viewDepthAt(vec2 uv)
{
    float rawDepth = texture(uDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, rawDepth * 2.0 - 1.0, 1.0);
    vec4 view = uInverseProjection * clip;
    return view.z / view.w;
}

/* UV from gl_FragCoord, NOT from fragTexCoord. Both this target and the one it
 * reads are FBOs, which raylib stores bottom-up, while a DrawTexturePro blit
 * carries whatever flip its source rectangle asks for. Deriving the coordinate
 * from the pixel position instead keeps every stage of the AO chain - the
 * occlusion pass, this blur, and the lit shader's lookup - in one orientation,
 * so there is no flip to get wrong. */
void main()
{
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec2 texel = 1.0 / uResolution;

    float centreDepth = viewDepthAt(uv);

    float sum = 0.0;
    float weight = 0.0;

    for (int y = -2; y < 2; y++) {
        for (int x = -2; x < 2; x++) {
            vec2 tapUv = uv + vec2(float(x), float(y)) * texel;

            /* THE REJECTION. A tap on the far side of a silhouette describes a
             * different surface, and averaging it in is what carries occlusion
             * through geometry. */
            if (abs(viewDepthAt(tapUv) - centreDepth) > kDepthThreshold) continue;

            sum    += texture(texture0, tapUv).r;
            weight += 1.0;
        }
    }

    /* Renormalised by the taps that survived. Dividing by 16 regardless would
     * darken every pixel near an edge in proportion to how many neighbours it
     * lost, which is a black outline around everything. */
    finalColor = vec4(vec3(weight > 0.0 ? sum / weight : texture(texture0, uv).r), 1.0);
}
