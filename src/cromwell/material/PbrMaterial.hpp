/* PbrMaterial.hpp — one surface's PBR description.
 *
 * SINGLE RESPONSIBILITY: hold the textures and the scalar factors that decide
 * how one class of surface responds to light.
 *
 * A METAL/ROUGH WORKFLOW. Three maps:
 *
 *   albedo   sRGB base colour, decoded to linear in the shader
 *   normal   tangent space, linear, OpenGL handedness (green is up)
 *   packed   metalness, roughness and ambient occlusion in one texture —
 *            none of the three needs colour, so none of them earns a texture
 *
 * FACTOR TIMES TEXTURE, the glTF rule. Every scalar below multiplies its map,
 * so a material with no textures at all is still fully described: the defaults
 * are 1x1 white (and a flat normal), and the factors alone decide the result.
 * That is what lets the placeholder box world and finished textured assets run
 * through exactly the same shader with no branch and no permutation.
 *
 * OWNS NOTHING. MaterialLibrary loads and unloads the textures; this is the
 * description they are loaded against.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

/* Slot indices into Material::maps.
 *
 * raylib's MATERIAL_MAP_* names are irrelevant here — DrawMesh binds maps[i]
 * to texture unit i and sets the sampler at locs[SHADER_LOC_MAP_DIFFUSE + i],
 * so only the INDEX matters. Naming them ourselves is clearer than pretending
 * the shadow map is a roughness map.
 *
 * THE ARRAY IS TWELVE LONG AND THREE OF IT IS POISONED. DrawMesh cares only
 * about the index, with one exception: it special-cases MATERIAL_MAP_CUBEMAP
 * (7), MATERIAL_MAP_IRRADIANCE (8) and MATERIAL_MAP_PREFILTER (9) and binds
 * those through rlEnableTextureCubemap, as GL_TEXTURE_CUBE_MAP. A sampler2D
 * reading a unit with only a cubemap bound gets BLACK — no warning, no GL
 * error, just zero. The transmission plane sat in slot 7 and read 0 everywhere
 * for as long as it existed; the whole world was multiplied by the glass tint
 * as though every surface were behind a window, which looked enough like a
 * cool colour grade to pass unnoticed for weeks.
 *
 * ONLY MATERIAL TEXTURES LIVE HERE. The frame's shared buffers — shadow map,
 * SSAO, lightmap, transmission — used to be copied into every material's array
 * too, which spent five of nine usable slots on state that is identical for
 * every draw in the frame. They are now bound once to fixed high texture units
 * (below), which DrawMesh never touches because no material has a texture at
 * those indices. That is what leaves room here to grow: a material may hold
 * four maps today and seven before anything has to move. */
enum : int {
    kMapAlbedo       = 0,
    kMapPacked       = 1,
    kMapNormal       = 2,
    kMapTranslucency = 3,   /* per-texel opacity: dirt, frost, patterns */
    kMapCount        = 4,
    /* 4, 5 and 6 are free for the next material map — thickness, flow, foam.
     * 7, 8 and 9 are the cubemap slots above and can only ever hold a real
     * cubemap, which is where a reflection probe will go. */
};

/* FRAME-GLOBAL TEXTURE UNITS, bound directly with rlActiveTextureSlot +
 * rlEnableTexture rather than through any material, with the sampler uniforms
 * pointed at them once at load time.
 *
 * Above the material block on purpose. DrawMesh binds and unbinds only the
 * indices where the material being drawn actually has a texture, so as long as
 * materials stay inside 0-6 these units survive untouched across every draw in
 * the frame — one bind each, rather than one per material per frame. */
enum : int {
    kUnitShadow       = 7,
    kUnitOcclusion    = 8,
    kUnitLightmap     = 9,
    kUnitLightIndex   = 10,
    kUnitTransmission = 11,
    /* The reflection probe is a CUBEMAP, and could have gone in material slot
     * 7 where raylib expects one — but that index is spoken for above, and a
     * probe is frame state rather than material state anyway. GL guarantees at
     * least 16 fragment texture units, so 12 is always there. */
    kUnitEnvironment  = 12,

    /* THE DBUFFER — what the decals wrote, read back by every lit surface. See
     * DecalBuffer.hpp for the plane layout.
     *
     * THESE THREE ARE THE LAST UNITS THAT EXIST. GL guarantees sixteen fragment
     * texture units and this uses 15, so the frame-global block is now full:
     * anything else that wants to be bound once per frame has to either take a
     * material slot (0-6, of which 4-6 are still free) or displace something
     * here. Written down because "there is always another texture unit" is true
     * right up until it is not, and the failure mode is a silent black sample
     * rather than an error — the same one that hid the transmission plane in a
     * cubemap slot for weeks. */
    kUnitDecalAlbedo  = 13,
    kUnitDecalNormal  = 14,
    kUnitDecalSurface = 15,
};

/* WHICH CHANNEL HOLDS WHAT, in the packed map. Two conventions are in the
 * wild and they are NOT the same, which is a genuinely easy way to ship a
 * scene where every metal reads as a dielectric and nobody can say why:
 *
 *   Mrao  metalness R, roughness G, occlusion B
 *         Valve's $mraotexture, and what hand-authored art here uses.
 *   Orm   occlusion R, roughness G, metalness B
 *         glTF's metallicRoughnessTexture, so it is what every model
 *         exported from Blender or Substance arrives carrying.
 *
 * Roughness is green in both, which is exactly why the mistake survives a
 * casual look: the surface roughness is right and only the metalness is
 * wrong. The shader swizzles on this flag rather than requiring assets be
 * repacked on import. */
enum class ChannelPacking : int {
    Mrao = 0,
    Orm  = 1,
};

/* WHAT THE ALBEDO'S ALPHA CHANNEL MEANS — which is NOT automatically
 * "opacity", and assuming it does is how a solid concrete wall ends up
 * see-through.
 *
 * An albedo texture's fourth channel is free real estate, and engines have
 * spent decades filling it with something else: UE3 in particular routinely
 * parks a specular or gloss mask there. The cinderblock wall extracted from
 * the XCOM 2 SDK has alpha ranging 132-216 across a completely opaque
 * surface, and read as opacity that is a ghost.
 *
 * So transparency is something a material DECLARES, exactly as glTF's
 * alphaMode does, and Opaque is the default. */
enum class AlphaMode : int {
    Opaque = 0,   /* alpha ignored entirely */
    Mask   = 1,   /* discard below alphaCutoff, fully opaque above */
    Blend  = 2,   /* alpha is coverage */
};

struct PbrMaterial {
    /* Multiplies the packed map's roughness and metalness channels. With no
     * map, these ARE the values. */
    float roughness = 0.8f;
    float metalness = 0.0f;

    /* Scales the tangent-space normal's xy. 0 flattens the map entirely, which
     * is also what the 1x1 fallback normal gives. */
    float normalStrength = 1.0f;

    /* World units per texture repeat, for geometry that takes its UVs from the
     * world-space planar projection. Loaded models carry their own UVs and
     * should leave this at 1. */
    float uvScale = 1.0f;

    ChannelPacking packing = ChannelPacking::Mrao;

    /* ---- transmission: light that arrives through the surface from behind --
     * Source 2's F_TRANSMISSIVE_BACKFACE_NDOTL, which lights from the BACK
     * face's N.L and adds the result as a separate term. Not a glass feature —
     * it is shared with foliage and skin, and it is the same question in all
     * three cases: what does a thin material look like with the sun on the far
     * side of it. Leaves glow, an ear glows, and a coloured pane glows.
     *
     * Zero by default, so every existing material is untouched and pays only a
     * branch that is never taken. See study/games/valve/source2_rendering.md 12.1. */
    float   transmissionAmount = 0.0f;
    Vector3 transmissionColour{ 1.0f, 1.0f, 1.0f };

    /* ---- emission: radiance the surface PRODUCES -------------------------
     *
     * A DIFFERENT QUESTION FROM EVERY OTHER FIELD IN THIS STRUCT, and the
     * distinction is worth stating because the three are easy to confuse. The
     * BRDF above describes what a surface does to light arriving at it;
     * `transmission` describes light passing THROUGH it from behind; this is
     * light the surface emits on its own account, which no amount of shadowing,
     * occlusion or ambient can reduce. A screen in a dark room is lit by
     * nothing and is still bright.
     *
     * IN LINEAR RADIANCE, AND DELIBERATELY UNBOUNDED. The pipeline is linear
     * and the scene target is RGBA16F precisely so a value can exceed one; a
     * strip light authored at 1.0 is exactly as bright as a fully lit white
     * wall and will not read as a light source. The numbers that look right are
     * several to tens. This is the same reason SunLight's radiance is not a
     * colour off a colour wheel.
     *
     * WHAT MAKES IT VISIBLE AS A GLOW rather than merely a bright patch is the
     * bloom pass, which is the engine's and not a property of any material —
     * see ScenePipeline. A material says how much light it makes; what a lens
     * does with light that bright is a camera question.
     *
     * ZERO BY DEFAULT, so every material already authored is untouched and the
     * shaders pay one add of a constant zero. */
    Vector3 emissiveColour{ 1.0f, 1.0f, 1.0f };
    float   emissiveStrength = 0.0f;

    AlphaMode alphaMode = AlphaMode::Opaque;
    float     alphaCutoff = 0.5f;

    /* ---- glass -------------------------------------------------------------
     * CS2 does not treat glass as a separate shading model: it is the ordinary
     * PBR material with a FRESNEL OPACITY RAMP, plus the usual specular and
     * cubemap reflection on top. A pane is nearly clear head-on and turns
     * opaque and tinted at grazing angles, because a dielectric reflects ~4%
     * facing you and approaches 100% edge-on — and since R + T = 1, driving
     * alpha from Fresnel *is* transmittance.
     *
     * Notably there is no refraction, and that is correct rather than cheap:
     * a flat pane has two parallel interfaces whose bends cancel, leaving a
     * displacement of about a pixel that is uniform across the surface — and
     * uniform displacement has no reference to be seen against. Curved or
     * thick glass would need it; windows do not. See study/games/valve/source2_rendering.md
     * §12.1. */
    float   baseOpacity     = 0.08f;   /* head-on, before the Fresnel ramp   */
    float   edgeThickness   = 0.0f;    /* offsets N·V — opacity facing you   */
    float   edgeFalloff     = 4.0f;    /* Fresnel exponent                   */
    float   edgeMaxOpacity  = 1.0f;    /* ceiling on the Fresnel term        */
    float   opacityScale    = 1.0f;    /* blend plain opacity -> Fresnel     */
    Vector3 edgeColour{ 1.0f, 1.0f, 1.0f };   /* tint picked up at grazing   */

    /* Squeezes a translucency map's range, so one greyscale texture can serve
     * as subtle grime on one material and heavy frosting on another. */
    Vector2 translucencyRemap{ 0.0f, 1.0f };

    /* GRIME IS A LAYER, NOT A STAIN IN THE ALBEDO. The translucency map is
     * that layer's opacity; these are the other two channels it needs.
     *
     * Roughness is the one that matters. Dirt does not so much darken glass as
     * make it locally ROUGH, which breaks the mirror reflection into a haze —
     * and a hazed reflection reads as a dirty window far more strongly than
     * any colour change does. It is also the cheap half: one mix, no extra
     * texture fetch. See study/games/valve/source2_rendering.md §12.1, "Grime as a layer,
     * not a texture". */
    float   grimeRoughness = 0.62f;
    Vector3 grimeColour{ 0.30f, 0.29f, 0.26f };   /* dulled road dust        */

    /* WHAT THE SUN BECOMES AFTER A FULL PANE, which is a different question
     * from how the pane looks: this is read in the shadow pass and written
     * into the shadow map's colour plane.
     *
     * The two are deliberately split. `transmissionTint` is a pure hue —
     * normalised so its largest channel is 1 — and `paneTransmittance` carries
     * all of the dimming, because the colour plane is a single channel and has
     * to encode "how much light survived" on its own. Folding the dimming into
     * the tint as well would count it twice. Grime multiplies the
     * transmittance, so a filthy window genuinely darkens the patch of floor
     * behind it rather than only looking dirty. */
    float   paneTransmittance = 0.86f;
    Vector3 transmissionTint{ 0.72f, 0.86f, 1.00f };

    /* Loaded from assets/materials/<name>_albedo|normal|mrao|translucency.png
     * when present, or adopted from a model file's own material, otherwise the
     * library's shared fallbacks. */
    Texture2D translucency{};

    /* Loaded from assets/materials/<name>_albedo|normal|mrao.png when present,
     * or adopted from a model file's own material, otherwise the library's
     * shared fallbacks. */
    Texture2D albedo{};
    Texture2D normal{};
    Texture2D packed{};
};

}  // namespace cromwell
