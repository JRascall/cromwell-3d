#version 400
/* pbr.fs.glsl - the OPAQUE AND GLASS surface family.
 *
 * 400 RATHER THAN 330 FOR EXACTLY ONE FEATURE: samplerCubeArray, which is GL
 * 4.0. The reflection probes are per room and the shader picks between them
 * per pixel, so every probe has to be reachable from one draw call - see
 * ReflectionProbeSet.hpp for why per-pixel rather than per-object. Nothing
 * else in assets/shaders needs to move; GLSL 330 compiles unchanged in a 4.3
 * core context.
 *
 * A shader FAMILY, not the shader. The lighting core it shades with lives in
 * common/ and is shared: common/brdf.glsl is the Cook-Torrance response,
 * common/shadow.glsl decides how much sun arrives, common/environment.glsl is
 * the sky. What remains in this file is the part that is specific to opaque
 * and blended surfaces - how a material's textures become albedo, roughness
 * and coverage. A water family is a sibling of this file that includes the
 * same core, not a branch inside it. That is Source 2's structure, where
 * csgo_glass, csgo_water_fancy and complex are separate shaders over a common
 * texturing.slang.
 *
 * OUTPUT IS LINEAR RADIANCE, NOT A PIXEL. Nothing here knows what a display
 * is: no gamma, no clamping to 1. It writes into an RGBA16F target and
 * tonemap.fs.glsl is the only stage allowed to think about the screen. That
 * separation is the whole reason a sun can be brighter than a wall.
 *
 * ONE PROGRAM FOR TEXTURED AND UNTEXTURED ALIKE. Every map has a 1x1 fallback
 * - white for albedo and mrao, flat for the normal - and every scalar in
 * uMaterialFactors multiplies its map. A surface with no textures is therefore
 * fully described by its factors and its vertex colour, and picks up real art
 * the moment a file appears next to it, with no branch and no permutation.
 */
#include "common/colour.glsl"
#include "common/brdf.glsl"
#include "common/dbuffer.glsl"
#include "common/environment.glsl"
#include "common/shadow.glsl"

in vec3 vWorldPosition;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUv;
in vec4 vColour;

out vec4 finalColor;

uniform vec4 colDiffuse;        /* raylib's per-draw tint; white for the world */

/* ---- material maps, slots 0-3 ------------------------------------------
 * These are the ONLY textures that travel in the material's map array. The
 * frame's shared buffers - shadow map, occlusion, lightmap, transmission -
 * are bound to fixed high texture units instead, and are declared in the
 * common/ headers that use them. See PbrMaterial.hpp for why. */
uniform sampler2D uAlbedoMap;   /* sRGB   base colour            (1x1 white) */
uniform sampler2D uMraoMap;     /* linear packed metal/rough/AO  (1x1 white) */
uniform sampler2D uNormalMap;   /* linear tangent space          (1x1 flat)  */
uniform sampler2D uTranslucencyMap;  /* 1 = clear, 0 = opaque    (1x1 white) */

/* (roughness, metalness, normalStrength, uvScale) */
/* ONE BIT PER SUPPRESSED LIGHTING TERM, for the dev panel's rendering
 * switches. Bit values are defined once, in RenderEffects.hpp.
 *
 * A DISABLE MASK, NOT AN ENABLE MASK, and the polarity is the whole point. An
 * unset uniform reads as ZERO, so with enable bits a mask that failed to
 * arrive — a renamed uniform, a shader reloaded without its uniforms repushed,
 * a pass that forgets to set it — switches off every light in the scene and
 * renders pure black. That is a debug affordance breaking the actual render,
 * which is the one thing it must never do. Zero here means "nothing
 * suppressed", so the failure mode of the whole feature is that the switches
 * stop working, not that the game goes dark.
 *
 * Each term is REMOVED, not faded — a term that is merely scaled down still
 * contributes and still hides whatever is underneath it, which defeats the
 * point of being able to switch it off. */
uniform int uLightingSuppress;

#define TERM_DIRECT_SUN       1
#define TERM_AMBIENT_DIFFUSE  2
#define TERM_AMBIENT_SPECULAR 4
#define TERM_BAKED_OCCLUSION  8
#define TERM_TRANSMISSION    16

bool termOn(int bit) { return (uLightingSuppress & bit) == 0; }

uniform vec4 uMaterialFactors;

/* ---- glass, only consulted when alphaMode is Blend -------------------------
 * CS2 has no separate glass shading model: it is this material with a Fresnel
 * opacity ramp on top. See study/games/valve/source2_rendering.md 12.1. */
uniform vec4 uGlassParams;   /* edgeThickness, edgeFalloff, edgeMaxOpacity, opacityScale */
uniform vec4 uGlassEdge;     /* edge tint rgb, base opacity                              */
uniform vec2 uGlassRemap;    /* squeezes the translucency map into an authored range     */

/* (grime colour rgb, grime roughness).
 *
 * The translucency map is a LAYER's opacity, not just a mask - and the channel
 * that sells it is roughness, not colour. Dirt makes glass locally rough,
 * which breaks the mirror reflection into a haze; a hazed reflection reads as
 * a dirty window far more strongly than a darker one does. */
uniform vec4 uGlassGrime;

/* (channelPacking, alphaMode, alphaCutoff, reserved)
 *
 * channelPacking
 *   0 = MRAO, metalness R / roughness G / occlusion B  (authored here)
 *   1 = ORM,  occlusion R / roughness G / metalness B  (every glTF import)
 * Roughness is green either way, which is precisely why mixing them up
 * survives a casual look: the surface roughness comes out right and only the
 * metalness is wrong.
 *
 * alphaMode
 *   0 = opaque, 1 = mask, 2 = blend. Opaque is the default and it IGNORES the
 * albedo's alpha channel outright, because that channel very often holds
 * something other than opacity - UE3 assets routinely keep a specular mask
 * there, and read as coverage it turns solid geometry into a ghost. */
uniform vec4 uMaterialOptions;

/* (transmissionColour.rgb, transmissionAmount) - Source 2's
 * F_TRANSMISSIVE_BACKFACE_NDOTL. Zero on everything but glass so far, and the
 * branch below is never taken for those. */
uniform vec4 uTransmission;

uniform sampler2D uAmbientOcclusion;   /* screen space; 1x1 white when off */
uniform vec2      uSceneSize;

/* ---- the flat geometry view (F) ------------------------------------------
 * WHERE IS THERE A SURFACE, and nothing else. No sun, no shadows, no ambient,
 * no transparency, no textures - just each face's own colour under a facing
 * ratio so that adjacent faces at different angles stay distinguishable.
 *
 * It exists because "is that a hole in the mesh or a hole in the lighting" is
 * otherwise unanswerable by looking, and we have now spent several rounds
 * failing to answer it. Anything that vanishes here is missing GEOMETRY;
 * anything present here but wrong in the lit view is SHADING. Glass in
 * particular is forced fully opaque - a debug view that leaves the window
 * see-through cannot answer the one question it was built for. */
uniform float uFlatView;

/* ------------------------------------------------------------------ main */
void main()
{
    float roughnessFactor = uMaterialFactors.x;
    float metalnessFactor = uMaterialFactors.y;
    float normalStrength  = uMaterialFactors.z;
    float uvScale         = uMaterialFactors.w;

    vec2 uv = vUv * uvScale;

    /* The flat face normal, kept apart from the mapped one. Shadows want the
     * geometry's own orientation - biasing a shadow lookup along a normal the
     * map has tilted reintroduces the acne the bias exists to remove. */
    vec3 geometric = normalize(vNormal);

    /* Backfacing geometry - the undersides of floor slabs seen through a
     * cutaway - would otherwise light from behind and read inside out. */
    if (!gl_FrontFacing) geometric = -geometric;

    /* ---- tangent frame and the normal map ------------------------------ */
    vec3 tangent = normalize(vTangent.xyz);
    /* Gram-Schmidt: interpolation across a triangle does not preserve the
     * right angle between tangent and normal. */
    tangent = normalize(tangent - geometric * dot(geometric, tangent));
    vec3 bitangent = cross(geometric, tangent) * vTangent.w;

    vec3 tangentNormal = texture(uNormalMap, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalStrength;

    vec3 N = normalize(mat3(tangent, bitangent, geometric) * tangentNormal);

    vec3 V = normalize(uCameraPosition - vWorldPosition);
    vec3 L = -uSunDirection;

    /* ---- decals, before anything is lit ---------------------------------
     * A decal changes what this surface IS — its colour, its relief, its
     * finish — and then the surface lights once, taking its own shadow, its own
     * probe and its own occlusion with it. That is the entire reason the decal
     * pass writes a material buffer instead of drawing lit quads: a blood
     * splatter in a doorway's shadow has to be a shadowed splatter, and nothing
     * downstream of here has to know it is there.
     *
     * The screen lookup is the same one the occlusion buffer uses below, and
     * lands in the same place for the same reason — see common/dbuffer.glsl. */
    vec2 screenUV = gl_FragCoord.xy / uSceneSize;
    DecalLayer decals = readDecals(screenUV);

    /* NOT ON GLASS, for the same reason SSAO is not applied to it further down.
     * A pane is in the depth prepass so the ribbon can test against it, so a
     * decal box reaching a window finds a receiver there and inks it — and a
     * scorch mark blended into a 6%-opaque surface is not a scorch mark on the
     * glass, it is a smear hanging in the opening. Decals on windows are a
     * separate feature (they belong in front of the pane, not in it), and the
     * honest thing until then is to leave the glass alone. */
    if (uMaterialOptions.y > 1.5) decals = noDecals();

    /* ---- material ------------------------------------------------------ */
    vec4 albedoSample = texture(uAlbedoMap, uv);
    vec4 base = albedoSample * vColour * colDiffuse;

    /* IN sRGB, BEFORE THE DECODE, not after. The DBuffer plane is 8 bits and
     * holds encoded values, so blending here rather than in linear is what
     * makes the decal pass's premultiply and this decode cancel exactly. */
    vec3 albedo = srgbToLinear(applyDecalAlbedo(decals, base.rgb));

    /* After the tangent frame, and in WORLD space — the decal pass built its
     * normal against the receiver's frame precisely so that this is a blend of
     * two world normals and not a second tangent-space unpacking. */
    N = applyDecalNormal(decals, N);

    /* Before ANY lighting, and before the alpha branch - so that a surface
     * appears here if and only if its triangles are being drawn.
     *
     * EVERY DEBUG TEST IN THIS FILE IS RANGED, and this one was not. An
     * open-ended `> 2.5` also catches view 4, and because this branch returns
     * before the material is read, selecting the roughness view silently
     * rendered the ROOMS view instead - which is a debug view lying about
     * which debug view it is, the one failure mode that wastes the most time.
     * Any new view added below MUST be bounded at both ends. */
    if (uFlatView > 2.5 && uFlatView < 3.5) {
        /* 3 - WHICH PROBE OWNS THIS SURFACE, as a flat colour per room.
         *
         * Its own view because the mirror view below cannot answer it. A
         * reflection that looks plausible and comes from the WRONG room is
         * exactly the bug this whole system exists to fix, and it is
         * invisible in a mirror - the leaked geometry looks like geometry.
         * Here a wall whose two faces belong to different rooms is two
         * colours, and one that leaks is one colour across both.
         *
         * The hash is arbitrary and only has to make adjacent indices look
         * unalike; magenta is "no probe claims this", which on a map with an
         * outdoor probe should appear nowhere. */
        int best, second;
        float blend;
        if (!selectProbes(probeSamplePoint(vWorldPosition, N), best, second, blend)) {
            finalColor = vec4(0.6, 0.0, 0.6, 1.0);
            return;
        }
        float h = float(best);
        vec3 tint = 0.5 + 0.5 * cos(6.2831853 * (h * 0.618 + vec3(0.0, 0.33, 0.67)));

        /* Darkened where two rooms are being blended, so the transition bands
         * are visible as bands rather than as a hard line that happens to be
         * in the right place. */
        finalColor = vec4(tint * mix(0.35, 1.0, blend), 1.0);
        return;
    }
    if (uFlatView > 4.5) {
        /* 5 - THE SCREEN OCCLUSION BUFFER, on the geometry it is applied to.
         *
         * The texture gallery already shows this buffer flat, and that is not
         * the same picture: flat, it is a grey rectangle nobody can register
         * against the world. Painted onto the surfaces it multiplies, "this
         * darkening is on that wall" becomes a thing you can see rather than
         * infer. Black is fully occluded, white is open. */
        /* AMPLIFIED, because the honest range of this buffer is roughly 0.85
         * to 1.0 and the tonemap crushes all of it to white. Shown raw, a real
         * artefact and a clean buffer are the same picture - which is how a
         * "fixed" claim got made off two images that were both white. The
         * darkening is scaled up eightfold so that what is actually there is
         * unmistakable; this is a measuring instrument, not a preview. */
        float ao = texture(uAmbientOcclusion, gl_FragCoord.xy / uSceneSize).r;
        float shown = clamp(1.0 - (1.0 - ao) * 8.0, 0.0, 1.0);
        finalColor = vec4(vec3(shown) * 2.0, 1.0);
        return;
    }
    /* 2 IS DELIBERATELY ABSENT HERE. The probe view draws chrome balls at the
     * capture points over an ORDINARY frame — see ProbeSpheres.hpp — so the
     * surface shader has nothing special to do for it. The tests below are
     * therefore ranged rather than open-ended: `uFlatView > 0.5` would catch
     * view 2 as well and flatten the very scene the balls need to sit in. */
    if (uFlatView > 0.5 && uFlatView < 1.5) {
        /* 1 - GEOMETRY ONLY. */
        float facing = clamp(dot(N, V), 0.0, 1.0);
        finalColor = vec4(albedo * (0.30 + 0.70 * facing), 1.0);
        return;
    }

    /* Coverage is whatever the material DECLARED, not whatever happened to be
     * sitting in the albedo's fourth channel. */
    float alphaMode = uMaterialOptions.y;
    float coverage = 1.0;
    vec3  edgeTint = vec3(1.0);
    float grime    = 0.0;     /* 0 clean, 1 caked. Applied after the MRAO read. */

    if (alphaMode > 1.5) {
        /* ---- glass -----------------------------------------------------
         * Opacity rises towards grazing angles because a dielectric reflects
         * ~4% head-on and approaches 100% edge-on; with R + T = 1 that IS
         * transmittance. The translucency map then dirties it per texel -
         * grime, frost or a painted pattern making the pane locally opaque.
         *
         * GLASS TAKES NO VERTEX COLOUR. The placeholder palette tints every
         * other surface so an untextured world still reads, but on a blended
         * material that tint is not a colour - it is a COLOURED FILM over
         * everything behind the pane, and a saturated one turns a window into
         * a sheet of cellophane. What little colour real glass has belongs to
         * its own map, so the base here is the texture and the material's own
         * diffuse, and nothing else. */
        albedo = srgbToLinear((albedoSample * colDiffuse).rgb);

        float translucency = texture(uTranslucencyMap, uv).r;
        translucency = mix(uGlassRemap.x, uGlassRemap.y, translucency);

        float viewDotNormalInv = clamp(1.0 - (dot(V, N) - uGlassParams.x), 0.0001, 1.0);
        float fresnel = clamp(pow(viewDotNormalInv, uGlassParams.y), 0.0, 1.0)
                      * uGlassParams.z;

        float plain = uGlassEdge.w;
        coverage = mix(plain, max(plain, fresnel), uGlassParams.w);

        /* Dirt makes a pane MORE opaque, never less. */
        grime = clamp(1.0 - translucency, 0.0, 1.0);
        coverage = clamp(coverage + grime * (1.0 - coverage), 0.0, 1.0);

        edgeTint = mix(vec3(1.0), uGlassEdge.rgb, fresnel);
    } else if (alphaMode > 0.5) {
        if (base.a < uMaterialOptions.z) discard;
    }

    /* Not named `packed` - that is a reserved word in GLSL. */
    vec3 packedSample = texture(uMraoMap, uv).rgb;
    /* Swizzle rather than require assets be repacked on import. */
    vec3 mrao = (uMaterialOptions.x > 0.5)
              ? vec3(packedSample.b, packedSample.g, packedSample.r)
              : packedSample;
    float metalness = clamp(mrao.r * metalnessFactor, 0.0, 1.0);
    float roughness = clamp(mrao.g * roughnessFactor, 0.045, 1.0);
    float bakedOcclusion = termOn(TERM_BAKED_OCCLUSION) ? mrao.b : 1.0;

    /* The decal's finish, over the surface's own. THIS IS THE HALF THAT SELLS
     * IT: a blood pool that is merely dark red reads as a texture, and the same
     * pool at roughness 0.15 on an 0.82 road reads as WET, because the sun and
     * the sky both pick out a tight highlight the concrete around it cannot
     * produce. Same argument as the grime layer on glass a few lines up, and
     * the same cheap mechanism — one mix, no extra fetch.
     *
     * The floor stays where the base material's does: a roughness of zero is a
     * perfect mirror and a numerical cliff in the BRDF, not a wetter surface. */
    metalness = clamp(applyDecalScalar(decals.metalness, metalness, decals.transmit),
                      0.0, 1.0);
    roughness = clamp(applyDecalScalar(decals.roughness, roughness, decals.transmit),
                      0.045, 1.0);

    /* NOT OCCLUSION. The mrao map's blue channel is what this SURFACE's relief
     * hides from the sky, and a decal lying on top of it does not change that —
     * blood on a rough casting is still in the casting's own pits. The decal's
     * blue channel is spent on the emissive mask instead, which is a thing only
     * a decal can be. */

    /* ---- the grime layer, over the glass rather than mixed into it --------
     * Roughness first, because it is the half that does the work: a clean pane
     * at 0.10 gives a tight mirror highlight, and pushing it towards 0.6 where
     * the dirt is spreads that highlight into a broad haze. THAT is what reads
     * as a dirty window; the colour is the finishing touch, not the effect.
     *
     * Both are a plain mix on the layer's opacity, which is what layering a
     * fully opaque coat over a surface reduces to - there is no glass left to
     * see through where the grime is solid. */
    roughness = mix(roughness, clamp(uGlassGrime.w, 0.045, 1.0), grime);
    albedo    = mix(albedo, srgbToLinear(uGlassGrime.rgb), grime);

    if (uFlatView > 3.5) {
        /* 4 - THE ROUGHNESS ACTUALLY BEING SHADED WITH.
         *
         * Late, deliberately: every other debug view returns before the
         * material is read, and this one must not — the whole question it
         * answers is what mrao.g, the material factor and the grime layer
         * combined to, which is only known HERE. A value read off the
         * MaterialLibrary table is what the material was AUTHORED as, and the
         * two disagreeing is exactly the bug worth catching.
         *
         * Greyscale so it can be compared against a number by eye, with the
         * probe cutoff banded: the reflection fades out entirely between 0.12
         * and 0.55, so anything DARKER than the marked band shows probe
         * content and anything lighter cannot. Green marks the band itself. */
        float shown = roughness;
        vec3  tint  = vec3(shown);

        if (shown > 0.12 && shown < 0.55) tint = vec3(0.1, 0.8 - shown, 0.1);

        /* The linear-radiance contract still applies — the tonemap pass owns
         * the screen — so this is written as the value it wants to READ as
         * after tonemapping rather than as a raw 0..1 grey. */
        finalColor = vec4(tint * 2.0, 1.0);
        return;
    }

    /* Metal/rough: a dielectric reflects 4% head-on and keeps its albedo as
     * diffuse; a conductor reflects its albedo and has no diffuse at all. */
    vec3 f0 = mix(vec3(0.04), albedo, metalness);
    vec3 diffuseAlbedo = albedo * (1.0 - metalness);

    float nDotV = clamp(dot(N, V), 1e-4, 1.0);
    float nDotL = clamp(dot(N, L), 0.0, 1.0);

    /* ---- the sun ------------------------------------------------------- */
    /* Diffuse and specular kept apart because the transparent path scales them
     * differently - see the premultiplied output at the end. */
    vec3 directDiffuse = vec3(0.0);
    vec3 directSpecular = vec3(0.0);
    vec3 direct = vec3(0.0);

    if (nDotL > 0.0 && termOn(TERM_DIRECT_SUN)) {
        /* Shadowed by the geometric normal and its own N.L, so a normal map
         * cannot push a fragment in or out of shadow. */
        float geometricNDotL = clamp(dot(geometric, L), 0.0, 1.0);
        vec3 visibility = sunVisibility(vWorldPosition, geometric, geometricNDotL);
        vec3 incoming = uSunColour * nDotL * visibility;

        SurfaceResponse sun =
            evaluateDirectional(N, V, L, incoming, diffuseAlbedo, f0, roughness);

        directDiffuse  = sun.diffuse;
        directSpecular = sun.specular;
        direct = directDiffuse + directSpecular;
    }

    /* ---- transmission: the sun on the FAR side of the surface -------------
     * Source 2's F_TRANSMISSIVE_BACKFACE_NDOTL - light entering the back and
     * leaving the front, which is what makes a leaf, an ear or a coloured pane
     * glow when the sun is behind it. It is the same term for all three, which
     * is why it lives on the material rather than in the glass block.
     *
     * It cannot double-count with the direct term: they are lit by opposite
     * signs of the same N.L, so exactly one of them is non-zero. And the
     * shadow lookup is offset along the REVERSED normal, because the ray we
     * are asking about arrives at the back face - biasing toward the front
     * would sample the wrong side of the surface entirely.
     *
     * The lookup is deliberately NOT taken at the surface - see below. */
    vec3 transmissive = vec3(0.0);
    if (uTransmission.w > 0.0 && termOn(TERM_TRANSMISSION)) {
        float backNDotL = clamp(dot(-N, L), 0.0, 1.0);
        if (backNDotL > 0.0) {
            float backGeometric = clamp(dot(-geometric, L), 0.0, 1.0);

            /* THIS LOOKUP CANNOT BE MADE STABLE ON A RECESSED THIN SURFACE,
             * which is why the window material ships with the amount at zero.
             *
             * A pane sits 0.018 world units inside its frame and a shadow texel
             * is about 0.006, so sampling AT the pane is dominated by a reveal
             * the map cannot resolve: the frame's own shadow switches the glow
             * off in a band along the edges. The obvious repair - move the
             * sample clear of the opening - fails differently. Displacing it
             * toward the sun is a LATERAL shift in the light's view of up to
             * the same distance, twenty-odd texels, so the lookup lands in a
             * neighbouring wall's shadow instead. Both were tried; the band
             * moved to the opposite edges and stayed.
             *
             * There is no offset that is right, because the feature and the
             * texel are the same size. The real answer is that "is this
             * window's exterior in sun" is a PER-PANE question, not a
             * per-pixel one - and SunBaker already computes exactly that, per
             * (cell, face), stably, because it path-traces rather than samples
             * a depth buffer. That is where this should read from when there
             * is stained or frosted glass to justify building it. */
            vec3 visibility = sunVisibility(vWorldPosition, -geometric, backGeometric);

            /* No 1/pi and no Fresnel: this is not a BRDF lobe, it is the
             * fraction of light the material passes, and the amount is
             * authored rather than derived. */
            transmissive = uSunColour * visibility * uTransmission.rgb
                         * uTransmission.w * backNDotL;
        }
    }

    /* ---- the sky ------------------------------------------------------- */
    vec3 ambientDiffuse = termOn(TERM_AMBIENT_DIFFUSE)
                        ? diffuseAlbedo * skyIrradiance(N)
                        : vec3(0.0);

    /* A rough surface reflects a wide cone, so bend the sample back toward the
     * normal as roughness rises rather than sampling a mirror direction the
     * material could not produce. */
    vec3 reflection = normalize(mix(reflect(-V, N), N, roughness * roughness));
    vec3 ambientSpecular = termOn(TERM_AMBIENT_SPECULAR)
                         ? environmentSpecular(reflection, vWorldPosition, N, roughness,
                                               uAmbientIntensity) *
                           environmentBRDF(f0, roughness, nDotV)
                         : vec3(0.0);

    /* TWO OCCLUSIONS, AND THEY ARE NOT THE SAME QUESTION. The mrao map's blue
     * channel is what the surface's own relief hides - baked once, at texel
     * scale. The SSAO buffer is what the surrounding geometry hides - computed
     * per frame, at pixel scale. Neither sees what the other does, so they
     * multiply. Both occlude the SKY only: the shadow map already answered
     * what the sun can reach, and occluding direct light here would darken
     * faces that are genuinely lit. */
    float screenOcclusion = texture(uAmbientOcclusion, gl_FragCoord.xy / uSceneSize).r;

    /* SSAO IS NOT APPLIED TO GLASS, and applying it was wrong twice over.
     *
     * The depth prepass deliberately includes windows - the ribbon has to
     * depth-test against them, so a pane counts as solid geometry there. SSAO
     * reads that prepass, sees a surface sitting inside a wall opening, and
     * does exactly what it is supposed to do for a surface in a recess: darkens
     * the edges where the frame crowds it. On concrete that is a contact
     * shadow. On a sheet of glass it is a dark border tracing the frame, with
     * no physical cause at all - the pane is not a cavity, and nothing is
     * blocking its view of the sky. Read against nearly-clear glass, that
     * border does not look like shading. It looks like the window is missing
     * there.
     *
     * It was always wrong; it only became VISIBLE when the ambient specular
     * stopped being scaled by coverage. Before that the whole ambient term,
     * SSAO border included, was multiplied by an opacity of 0.06 and could not
     * be seen. Fixing the reflection is what surfaced this. */
    if (alphaMode > 1.5) screenOcclusion = 1.0;

    float occlusion = bakedOcclusion * screenOcclusion;

    /* KEPT APART, for the same reason the direct terms are. The sky's
     * REFLECTION off a pane is specular, and on a transparent surface it must
     * not be scaled by opacity — see the output block below. Summing them here
     * and splitting later is impossible, so they stay separate from the start.
     * For an opaque surface the two are added straight back together and
     * nothing changes. */
    /* Only the DIFFUSE takes the intensity here — the specular already had it
     * applied to its sky half inside environmentSpecular, and its probe half
     * must not take it at all. See environment.glsl. */
    vec3 ambientDiffuseLight  = ambientDiffuse  * uAmbientIntensity * occlusion;
    vec3 ambientSpecularLight = ambientSpecular * occlusion;
    vec3 ambient = ambientDiffuseLight + ambientSpecularLight;

    /* PREMULTIPLIED OUTPUT FOR TRANSPARENT SURFACES.
     *
     * Ordinary alpha blending scales the WHOLE fragment by opacity, specular
     * included - so a 6%-opaque pane keeps 6% of its sun glint, and the one
     * cue that most says "this is glass and not a hole" is the first thing
     * thrown away. Alyx's membrane mode fixes it by premultiplying only the
     * diffuse and leaving specular at full strength.
     *
     * THAT APPLIES TO THE SKY'S REFLECTION TOO, and getting it right for the
     * sun while leaving the sky scaled by opacity was worth more than the sun
     * fix was. In daylight the thing that makes a window read as a window is
     * not the sun glint - which only exists at one angle - it is the SKY
     * reflected off the pane, which exists at every angle and is what your eye
     * uses to place a sheet of glass in space at all. Multiplying that by a 6%
     * coverage erased it, and a pane with no reflection is indistinguishable
     * from a hole in the wall.
     *
     * The blender must therefore be ONE / ONE_MINUS_SRC_ALPHA rather than
     * SRC_ALPHA / ONE_MINUS_SRC_ALPHA - the diffuse is already scaled here, so
     * the blender must not scale it again. Application sets that around the
     * transparent pass. */
    if (alphaMode > 1.5) {
        vec3 diffuseLight  = (directDiffuse + ambientDiffuseLight) * edgeTint;

        /* Transmission joins the SPECULAR side of the split, unscaled by
         * coverage - for the same reason the specular does. It is light the
         * pane itself sends toward the eye, not a modulation of whatever is
         * behind the pane, so scaling it by opacity would make a nearly clear
         * window unable to glow no matter how strongly it is lit from
         * behind. How much it glows is uTransmission.w's job. */
        vec3 specularLight = directSpecular + ambientSpecularLight + transmissive;
        finalColor = vec4(diffuseLight * coverage + specularLight, coverage);
    } else {
        /* A SELF-LIT DECAL IS ADDED, NOT MIXED IN, and it is added LAST — after
         * the sun, the sky and the occlusion have all had their say and none of
         * them got to touch it. That is what emissive means: radiance the
         * surface produces rather than reflects, so a glowing rune stays the
         * same brightness in a shadow, which is the one cue that distinguishes
         * it from a pale painted one.
         *
         * Exactly zero wherever no decal set the mask, so every surface in the
         * world pays one multiply and no change at all. Glass never reaches
         * here — decals are switched off for it above. */
        vec3 emissive = decalEmissive(decals, albedo);
        finalColor = vec4(direct + ambient + transmissive + emissive, coverage);
    }
}
