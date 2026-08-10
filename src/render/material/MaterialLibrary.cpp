#include "render/material/MaterialLibrary.hpp"

#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

#include <cstring>

namespace xcom {
namespace {

/* The scalar factors each built-in surface had before any texture existed.
 * They were SurfaceFinish's whole content; now they are the fallback a
 * material uses until its packed map turns up, and the multiplier on that map
 * afterwards. */
struct KindDefaults {
    SurfaceKind kind;
    float roughness;
    float metalness;
    float uvScale;
};

constexpr KindDefaults kDefaults[] = {
    /* worn asphalt is smoother than the concrete slabs it runs between, which
     * is most of what makes a road read as a road here */
    { SurfaceKind::Floor,  0.82f, 0.0f, 1.0f },
    { SurfaceKind::Road,   0.62f, 0.0f, 1.0f },
    { SurfaceKind::Grass,  0.95f, 0.0f, 1.0f },
    { SurfaceKind::Wall,   0.75f, 0.0f, 1.0f },
    /* glass: smooth, and dielectric like everything else — the reflection
     * comes from the 0.04 base reflectance every dielectric has, not from
     * metalness */
    { SurfaceKind::Window, 0.10f, 0.0f, 1.0f },
    { SurfaceKind::Cover,  0.70f, 0.0f, 1.0f },
    { SurfaceKind::Ramp,   0.80f, 0.0f, 1.0f },
    { SurfaceKind::Block,  0.85f, 0.0f, 1.0f },
    { SurfaceKind::Canopy, 0.88f, 0.0f, 1.0f },
    { SurfaceKind::Portal, 0.45f, 0.0f, 1.0f },
    /* galvanised steel: the one genuine conductor in the set */
    { SurfaceKind::Ladder, 0.35f, 1.0f, 2.0f },
    /* painted armour and fatigues alike — the albedo tells a soldier from a
     * hull, not the finish */
    { SurfaceKind::Body,   0.55f, 0.0f, 1.0f },
};

}  // namespace

MaterialLibrary::~MaterialLibrary()
{
    for (Texture2D& texture : owned_) UnloadTexture(texture);
    if (flatNormal_.id) UnloadTexture(flatNormal_);
    /* white_ is rlgl's shared default — unloading it would take the default
     * texture out from under every other material in the program. */
}

void MaterialLibrary::createFallbacks()
{
    white_ = Texture2D{ rlGetTextureIdDefault(), 1, 1, 1,
                        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    /* (0.5, 0.5, 1.0) decodes to +z — no perturbation at all. */
    Image flat = GenImageColor(1, 1, Color{ 128, 128, 255, 255 });
    flatNormal_ = LoadTextureFromImage(flat);
    UnloadImage(flat);
}

Texture2D MaterialLibrary::loadOrFallback(const char* path, Texture2D fallback, bool& foundAny)
{
    if (!FileExists(path)) return fallback;

    Texture2D texture = LoadTexture(path);
    if (texture.id == 0) {
        TraceLog(LOG_WARNING, "MATERIAL: could not load %s", path);
        return fallback;
    }

    /* Repeat, because the UVs of the generated geometry are a world-space
     * planar projection and run well past 1.0 across a map. Mipmaps and
     * anisotropy because those UVs also go near-edge-on to the camera at the
     * far end of the board, which is exactly where an unmipped texture turns
     * into noise.
     *
     * BOTH FILTER CALLS ARE NEEDED, and the order matters. raylib's
     * TEXTURE_FILTER_ANISOTROPIC_* sets only GL_TEXTURE_MAX_ANISOTROPY and
     * leaves the min filter alone — so on its own it requests anisotropic
     * sampling of a mip chain the sampler has been told not to use, and the
     * texture aliases exactly as if there were no mipmaps at all. TRILINEAR
     * sets the min filter to LINEAR_MIPMAP_LINEAR, and it must come after
     * GenTextureMipmaps because it no-ops on a texture with one level. */
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_8X);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);

    owned_.push_back(texture);
    foundAny = true;
    return texture;
}

void MaterialLibrary::loadTexturesFor(Slot& slot)
{
    const char* root = ShaderLibrary::assetRoot();
    const char* name = slot.name.c_str();

    bool foundAny = false;
    slot.description.albedo = loadOrFallback(TextFormat("%s/materials/%s_albedo.png", root, name),
                                             white_, foundAny);
    slot.description.normal = loadOrFallback(TextFormat("%s/materials/%s_normal.png", root, name),
                                             flatNormal_, foundAny);
    slot.description.packed = loadOrFallback(TextFormat("%s/materials/%s_mrao.png", root, name),
                                             white_, foundAny);

    /* A SEPARATE MAP, not the albedo's alpha — the same convention Valve
     * documents, and for the same reason the alpha channel could not be
     * trusted for opacity in the first place. It also means a pane's colour
     * and its grime are authored independently. White = perfectly clear. */
    slot.description.translucency =
        loadOrFallback(TextFormat("%s/materials/%s_translucency.png", root, name),
                       white_, foundAny);

    slot.ownsTextures = foundAny;
    if (foundAny) texturedCount_++;
}

void MaterialLibrary::applyTexturesToHandle(Slot& slot)
{
    slot.handle.maps[kMapAlbedo].texture = slot.description.albedo;
    slot.handle.maps[kMapPacked].texture = slot.description.packed;
    slot.handle.maps[kMapNormal].texture = slot.description.normal;
    slot.handle.maps[kMapTranslucency].texture = slot.description.translucency;
}

MaterialLibrary::Handle MaterialLibrary::createSlot(const char* name)
{
    auto slot = std::make_unique<Slot>();
    slot->name = name;

    slot->handle = LoadMaterialDefault();
    slot->handle.shader = shader_;
    /* White, so albedo comes from the texture and the vertex colour. The
     * bodies overwrite this per draw to tint one shared mesh. */
    slot->handle.maps[kMapAlbedo].color = WHITE;

    slot->description.albedo = white_;
    slot->description.normal = flatNormal_;
    slot->description.packed = white_;
    slot->description.translucency = white_;

    loadTexturesFor(*slot);
    applyTexturesToHandle(*slot);

    slots_.push_back(std::move(slot));
    return static_cast<Handle>(slots_.size()) - 1;
}

bool MaterialLibrary::load(Shader shader)
{
    if (shader.id == 0) return false;
    shader_ = shader;

    createFallbacks();

    /* Built-ins first and in enum order, so handleOf(kind) is a cast. */
    for (int i = 0; i < kSurfaceKindCount; i++)
        createSlot(nameOf(static_cast<SurfaceKind>(i)));

    for (const KindDefaults& entry : kDefaults) {
        PbrMaterial& description = slots_[indexOf(entry.kind)]->description;
        description.roughness = entry.roughness;
        description.metalness = entry.metalness;
        description.uvScale   = entry.uvScale;
    }

    /* Glass is the one built-in that is genuinely see-through. Opacity lives
     * on the material rather than in the palette so the palette keeps meaning
     * "what colour is this", and the shader's Blend path reads it through
     * colDiffuse. */
    const Handle glass = handleOf(SurfaceKind::Window);
    setAlphaMode(glass, AlphaMode::Blend);

    PbrMaterial pane;
    pane.baseOpacity    = 0.06f;   /* nearly clear looking straight through   */
    pane.edgeFalloff    = 4.0f;    /* and turning opaque fast off-axis        */
    pane.edgeMaxOpacity = 0.95f;
    pane.edgeColour     = Vector3{ 0.80f, 0.90f, 1.00f };   /* cool grazing sheen */

    /* The remap is the dirt's master dial: the map is authored across its full
     * range and squeezed into a narrower one here, so the same texture serves
     * a lightly weathered pane and a filthy one without a second asset.
     *
     * A high floor deliberately — glass reads as glass first and dirty second,
     * and the roughness half of the grime layer stays legible long after the
     * opacity half has stopped being obvious. Drop this towards 0 for a
     * derelict. */
    pane.translucencyRemap = Vector2{ 0.72f, 1.0f };
    setGlass(glass, pane);

    /* The same hue the sun picks up crossing a pane, so a window lit from
     * behind glows the colour it casts.
     *
     * SMALL, deliberately. The term is added UNSCALED by opacity — for the
     * same reason the specular is — so on a 6%-opaque pane it competes with
     * the background showing through rather than with the pane's own diffuse.
     * At 0.25 it washed the glass out to a white sheet.
     *
     * ZERO ON WINDOWS, after two attempts, and the reason is a limit rather
     * than a bug to find. The term is gated on a shadow lookup, and on a pane
     * recessed 0.018 units inside its frame with a 0.006-unit shadow texel
     * there is no place to take that lookup that is correct: at the pane it
     * catches the frame's own reveal, and clear of the pane it catches the
     * neighbouring wall, because moving the sample toward the sun displaces it
     * sideways in the light's view by the same amount. Both were tried and
     * both produce a band along two edges of the glass — which, since the glow
     * was the only thing making clear glass visible at all, reads as a hole in
     * the window rather than as a lighting artefact.
     *
     * The effect is worth having and the machinery is kept. What it needs is a
     * PER-PANE answer to "is this window's exterior in sun" rather than a
     * per-pixel one, and SunBaker already computes precisely that per
     * (cell, face) by path tracing. Wire it there when stained or frosted
     * glass makes the effect worth more than a clean pane's near-zero scatter
     * — that glass is also opaque enough for a frame shadow to read as shading
     * rather than as absence. See pbr.fs.glsl for the geometry of the trap. */
    setTransmission(glass, Vector3{ 0.72f, 0.86f, 1.00f }, 0.0f);


    TraceLog(LOG_INFO, "MATERIAL: %d built-in materials, %d with textures; the rest "
                       "run on vertex colour and scalar factors",
             kSurfaceKindCount, texturedCount_);

    loaded_ = true;
    return true;
}

MaterialLibrary::Handle MaterialLibrary::handleOf(SurfaceKind kind) const
{
    return static_cast<Handle>(indexOf(kind));
}

MaterialLibrary::Handle MaterialLibrary::findOrCreate(const char* name)
{
    if (!name || !*name) return kInvalid;

    for (std::size_t i = 0; i < slots_.size(); i++)
        if (slots_[i]->name == name) return static_cast<Handle>(i);

    return createSlot(name);
}

void MaterialLibrary::adoptTextures(Handle handle, Texture2D albedo, Texture2D normal,
                                    Texture2D packed, ChannelPacking packing)
{
    if (handle < 0 || handle >= static_cast<Handle>(slots_.size())) return;
    Slot& slot = *slots_[static_cast<std::size_t>(handle)];

    /* Only replace what the model actually brought. A model with a base colour
     * and no normal map should keep the flat fallback, not a null texture. */
    if (albedo.id != 0) slot.description.albedo = albedo;
    if (normal.id != 0) slot.description.normal = normal;
    if (packed.id != 0) slot.description.packed = packed;
    slot.description.packing = packing;

    /* A model's UVs are authored, not projected — retiling them would be
     * wrong. */
    slot.description.uvScale = 1.0f;

    applyTexturesToHandle(slot);
}

void MaterialLibrary::setFactors(Handle handle, float roughness, float metalness)
{
    if (handle < 0 || handle >= static_cast<Handle>(slots_.size())) return;
    PbrMaterial& description = slots_[static_cast<std::size_t>(handle)]->description;
    description.roughness = roughness;
    description.metalness = metalness;
}

void MaterialLibrary::setAlphaMode(Handle handle, AlphaMode mode, float cutoff)
{
    if (handle < 0 || handle >= static_cast<Handle>(slots_.size())) return;
    PbrMaterial& description = slots_[static_cast<std::size_t>(handle)]->description;
    description.alphaMode   = mode;
    description.alphaCutoff = cutoff;
}

void MaterialLibrary::setTransmission(Handle handle, Vector3 colour, float amount)
{
    if (handle < 0 || handle >= static_cast<Handle>(slots_.size())) return;
    PbrMaterial& description = slots_[static_cast<std::size_t>(handle)]->description;
    description.transmissionColour = colour;
    description.transmissionAmount = amount;
}

Vector4 MaterialLibrary::transmissionOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& d = slots_[index]->description;
    return Vector4{ d.transmissionColour.x, d.transmissionColour.y,
                    d.transmissionColour.z, d.transmissionAmount };
}

void MaterialLibrary::setGlass(Handle handle, const PbrMaterial& glass)
{
    if (handle < 0 || handle >= static_cast<Handle>(slots_.size())) return;
    PbrMaterial& description = slots_[static_cast<std::size_t>(handle)]->description;

    description.baseOpacity      = glass.baseOpacity;
    description.edgeThickness    = glass.edgeThickness;
    description.edgeFalloff      = glass.edgeFalloff;
    description.edgeMaxOpacity   = glass.edgeMaxOpacity;
    description.opacityScale     = glass.opacityScale;
    description.edgeColour       = glass.edgeColour;
    description.translucencyRemap = glass.translucencyRemap;
    description.grimeRoughness   = glass.grimeRoughness;
    description.grimeColour      = glass.grimeColour;
    description.paneTransmittance = glass.paneTransmittance;
    description.transmissionTint  = glass.transmissionTint;
}

Vector4 MaterialLibrary::glassParamsOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& d = slots_[index]->description;
    return Vector4{ d.edgeThickness, d.edgeFalloff, d.edgeMaxOpacity, d.opacityScale };
}

Vector4 MaterialLibrary::glassEdgeOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& d = slots_[index]->description;
    return Vector4{ d.edgeColour.x, d.edgeColour.y, d.edgeColour.z, d.baseOpacity };
}

Vector2 MaterialLibrary::glassRemapOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    return slots_[index]->description.translucencyRemap;
}

Vector4 MaterialLibrary::glassGrimeOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& d = slots_[index]->description;
    return Vector4{ d.grimeColour.x, d.grimeColour.y, d.grimeColour.z, d.grimeRoughness };
}

Vector4 MaterialLibrary::glassTransmissionOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& d = slots_[index]->description;
    return Vector4{ d.transmissionTint.x, d.transmissionTint.y, d.transmissionTint.z,
                    d.paneTransmittance };
}

Texture2D MaterialLibrary::translucencyOf(SurfaceKind kind) const
{
    return slots_[indexOf(kind)]->description.translucency;
}

const Material& MaterialLibrary::material(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    return slots_[index]->handle;
}

Vector4 MaterialLibrary::factorsOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& description = slots_[index]->description;
    return Vector4{ description.roughness, description.metalness,
                    description.normalStrength, description.uvScale };
}

Vector4 MaterialLibrary::optionsOf(Handle handle) const
{
    const std::size_t index = (handle >= 0 && handle < static_cast<Handle>(slots_.size()))
                            ? static_cast<std::size_t>(handle) : 0;
    const PbrMaterial& description = slots_[index]->description;
    return Vector4{ static_cast<float>(description.packing),
                    static_cast<float>(description.alphaMode),
                    description.alphaCutoff,
                    0.0f };
}

}  // namespace xcom
