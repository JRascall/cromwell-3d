#include "render/model/ModelAsset.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/lighting/PbrShader.hpp"

namespace xcom {

ModelAsset::~ModelAsset()
{
    /* UnloadModel frees the meshes and the materials the loader created,
     * including the textures it loaded for them. The library only ever
     * BORROWED those textures, which is why it tracks its own separately. */
    if (loaded_) UnloadModel(model_);
}

bool ModelAsset::conditionMeshes()
{
    bool usable = true;

    for (int i = 0; i < model_.meshCount; i++) {
        Mesh& mesh = model_.meshes[i];

        if (mesh.normals == nullptr) {
            TraceLog(LOG_WARNING, "MODEL: %s mesh %d has no normals - it cannot be lit. "
                                  "Re-export with normals.", name_.c_str(), i);
            usable = false;
            continue;
        }

        if (mesh.tangents != nullptr) continue;

        if (mesh.texcoords == nullptr) {
            /* No UVs means no tangents and no textures either. The mesh still
             * lights from its normals and its material's scalar factors, which
             * is a perfectly reasonable untextured prop - so this is a note,
             * not a failure. */
            TraceLog(LOG_INFO, "MODEL: %s mesh %d has no texcoords - untextured, "
                               "flat-shaded from its material factors", name_.c_str(), i);
            continue;
        }

        /* raylib 5.5's GenMeshTangents updates the already-uploaded vertex
         * array as well as the CPU copy, so this is safe after LoadModel. */
        GenMeshTangents(&mesh);
    }

    return usable;
}

/* raylib fills a loaded material's slots using ITS enum, which is not our slot
 * layout - so the textures are read out by raylib's names and handed to the
 * library, which puts them where our shader expects them. */
void ModelAsset::adoptMaterials(MaterialLibrary& library, MaterialLibrary::Handle fallback)
{
    meshMaterials_.assign(static_cast<std::size_t>(model_.meshCount), fallback);

    for (int i = 0; i < model_.meshCount; i++) {
        const int materialIndex = model_.meshMaterial[i];
        if (materialIndex < 0 || materialIndex >= model_.materialCount) continue;

        const Material& source = model_.materials[materialIndex];

        const Texture2D albedo = source.maps[MATERIAL_MAP_ALBEDO].texture;
        const Texture2D normal = source.maps[MATERIAL_MAP_NORMAL].texture;
        const Texture2D packed = source.maps[MATERIAL_MAP_METALNESS].texture;

        /* raylib points every unset slot at its 1x1 default, so "has a
         * texture" means "has one that is not the default". */
        const unsigned int fallbackId = rlGetTextureIdDefault();
        const bool hasAlbedo = albedo.id != 0 && albedo.id != fallbackId;
        const bool hasNormal = normal.id != 0 && normal.id != fallbackId;
        const bool hasPacked = packed.id != 0 && packed.id != fallbackId;

        if (!hasAlbedo && !hasNormal && !hasPacked) continue;

        /* One library material per (model, file-material) pair. */
        const std::string slotName = name_ + "#" + std::to_string(materialIndex);
        const MaterialLibrary::Handle handle = library.findOrCreate(slotName.c_str());

        library.adoptTextures(handle,
                              hasAlbedo ? albedo : Texture2D{},
                              hasNormal ? normal : Texture2D{},
                              hasPacked ? packed : Texture2D{},
                              /* a mesh file's packed map is glTF's ORM */
                              ChannelPacking::Orm);

        /* glTF's factors live on the material, and raylib parks them in the
         * map colours/values rather than anywhere typed. A model that carries
         * a metallic-roughness TEXTURE wants its factors at 1 so the texture
         * is not scaled down by a placeholder default. */
        library.setFactors(handle, hasPacked ? 1.0f : 0.8f, hasPacked ? 1.0f : 0.0f);

        meshMaterials_[static_cast<std::size_t>(i)] = handle;
    }
}

bool ModelAsset::load(const char* path, const char* name, MaterialLibrary& library,
                      MaterialLibrary::Handle fallbackMaterial)
{
    if (!FileExists(path)) {
        TraceLog(LOG_WARNING, "MODEL: %s not found", path);
        return false;
    }

    model_ = LoadModel(path);
    if (model_.meshCount == 0) {
        TraceLog(LOG_WARNING, "MODEL: %s loaded no meshes", path);
        UnloadModel(model_);
        model_ = Model{};
        return false;
    }

    name_   = name;
    loaded_ = true;

    conditionMeshes();
    adoptMaterials(library, fallbackMaterial);
    bounds_ = GetModelBoundingBox(model_);

    TraceLog(LOG_INFO, "MODEL: %s loaded (%d mesh%s)", path, model_.meshCount,
             model_.meshCount == 1 ? "" : "es");
    return true;
}

void ModelAsset::draw(Matrix transform, const Material& material) const
{
    if (!loaded_) return;
    for (int i = 0; i < model_.meshCount; i++)
        DrawMesh(model_.meshes[i], material, transform);
}

void ModelAsset::drawLit(Matrix transform, const MaterialLibrary& library,
                         const PbrShader& shader) const
{
    if (!loaded_) return;

    for (int i = 0; i < model_.meshCount; i++) {
        const MaterialLibrary::Handle handle = meshMaterials_[static_cast<std::size_t>(i)];
        shader.setMaterialFactors(library.factorsOf(handle));
        shader.setMaterialOptions(library.optionsOf(handle));
        shader.setMaterialTransmission(library.transmissionOf(handle));
        DrawMesh(model_.meshes[i], library.material(handle), transform);
    }
}

}  // namespace xcom
