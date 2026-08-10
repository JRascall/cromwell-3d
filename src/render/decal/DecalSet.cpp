#include "render/decal/DecalSet.hpp"

#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

#include <algorithm>

namespace xcom {
namespace {

const DecalSet::MaterialTextures kNoTextures{};

}  // namespace

DecalSet::~DecalSet()
{
    for (Texture2D& texture : owned_) UnloadTexture(texture);
    if (flatNormal_.id) UnloadTexture(flatNormal_);
}

void DecalSet::createFallbacks()
{
    if (fallbacksReady_) return;

    white_ = Texture2D{ rlGetTextureIdDefault(), 1, 1, 1,
                        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    Image flat = GenImageColor(1, 1, Color{ 128, 128, 255, 255 });
    flatNormal_ = LoadTextureFromImage(flat);
    UnloadImage(flat);

    fallbacksReady_ = true;
}

Texture2D DecalSet::loadOrFallback(const char* path, Texture2D fallback)
{
    if (!FileExists(path)) return fallback;

    Texture2D texture = LoadTexture(path);
    if (texture.id == 0) {
        TraceLog(LOG_WARNING, "DECAL: could not load %s", path);
        return fallback;
    }

    /* CLAMP, NOT REPEAT — the one place a decal texture differs from a surface
     * one, and the reason is the projection. A decal's UVs come from its own
     * box and run 0..1 across it by construction, but a texel fetched at the
     * very edge with bilinear filtering reaches half a texel past it. Under
     * REPEAT that wraps to the opposite side and leaves a thin bleed of the far
     * edge around the whole decal, which on anything with a dark border reads
     * as a hairline frame. */
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_8X);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);

    owned_.push_back(texture);
    return texture;
}

DecalMaterialId DecalSet::findOrLoad(const char* name)
{
    if (!name || !*name) return kInvalidDecalMaterial;

    for (std::size_t i = 0; i < materials_.size(); i++)
        if (materials_[i].name == name) return static_cast<DecalMaterialId>(i);

    createFallbacks();

    const char* root = ShaderLibrary::assetRoot();

    Slot slot;
    slot.name = name;
    slot.textures.albedo =
        loadOrFallback(TextFormat("%s/materials/decals/%s_albedo.png", root, name), white_);
    slot.textures.normal =
        loadOrFallback(TextFormat("%s/materials/decals/%s_normal.png", root, name), flatNormal_);
    slot.textures.packed =
        loadOrFallback(TextFormat("%s/materials/decals/%s_mrao.png", root, name), white_);

    materials_.push_back(std::move(slot));
    return static_cast<DecalMaterialId>(materials_.size()) - 1;
}

DecalMaterialId DecalSet::registerTextures(const char* name, Texture2D albedo,
                                           Texture2D normal, Texture2D packed)
{
    if (!name || !*name) return kInvalidDecalMaterial;

    createFallbacks();

    /* Generated art has no file to reload from, so a name collision here is a
     * caller bug rather than a cache hit — and silently handing back the older
     * material would leak the textures just passed in. */
    for (const Slot& slot : materials_)
        if (slot.name == name) {
            TraceLog(LOG_WARNING, "DECAL: '%s' is already registered", name);
            return kInvalidDecalMaterial;
        }

    const auto adopt = [this](Texture2D texture, Texture2D fallback) {
        if (texture.id == 0) return fallback;
        SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
        owned_.push_back(texture);
        return texture;
    };

    Slot slot;
    slot.name = name;
    slot.textures.albedo = adopt(albedo, white_);
    slot.textures.normal = adopt(normal, flatNormal_);
    slot.textures.packed = adopt(packed, white_);

    materials_.push_back(std::move(slot));
    return static_cast<DecalMaterialId>(materials_.size()) - 1;
}

const DecalSet::MaterialTextures& DecalSet::textures(DecalMaterialId id) const
{
    if (id < 0 || id >= static_cast<DecalMaterialId>(materials_.size())) return kNoTextures;
    return materials_[static_cast<std::size_t>(id)].textures;
}

const char* DecalSet::materialName(DecalMaterialId id) const
{
    if (id < 0 || id >= static_cast<DecalMaterialId>(materials_.size())) return "";
    return materials_[static_cast<std::size_t>(id)].name.c_str();
}

void DecalSet::add(const Decal& decal)
{
    decals_.push_back(decal);
    sorted_ = false;
}

const std::vector<Decal>& DecalSet::inDrawOrder() const
{
    if (!sorted_) {
        /* STABLE, so insertion order survives inside a tier. With std::sort a
         * detonation's twenty scorch marks — all at the same sortOrder — would
         * come back in an arbitrary order that changes every time one is added,
         * and overlapping marks would flicker between frames for no reason a
         * player could see. */
        std::stable_sort(decals_.begin(), decals_.end(),
                         [](const Decal& a, const Decal& b) {
                             return a.sortOrder < b.sortOrder;
                         });
        sorted_ = true;
    }
    return decals_;
}

}  // namespace xcom
