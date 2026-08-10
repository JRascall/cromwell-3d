/* ModelAsset.hpp — one mesh file, loaded and made fit to light.
 *
 * SINGLE RESPONSIBILITY: own a raylib Model, guarantee the vertex data this
 * renderer's shaders require, and say which material each of its meshes draws
 * with.
 *
 * WHAT "FIT TO LIGHT" MEANS. raylib will happily load a mesh with no normals
 * and no tangents; this renderer will happily light it as a black silhouette
 * with a normal map that does nothing. So loading is not the whole job:
 *
 *   normals    must exist. Without them there is no N to dot with anything,
 *              and no amount of texturing rescues it. Reported, not faked —
 *              a mesh exported without normals is broken art, and inventing
 *              smooth ones would hide that.
 *   tangents   generated from the UVs when the file did not carry them. OBJ
 *              never does; glTF usually does, but only if the exporter was
 *              asked. Without them the tangent-space normal map is nonsense.
 *   texcoords  required for tangents to be generatable at all.
 *
 * MATERIALS COME FROM THE FILE WHERE THE FILE HAS THEM. A glTF arrives with
 * its own base colour, normal and metallic-roughness textures already loaded
 * by raylib; those are adopted into a MaterialLibrary slot rather than being
 * reloaded from disk under a different name. Note the packing difference:
 * glTF's metallicRoughness texture is ORM, not Valve's MRAO — see
 * PbrMaterial.hpp. Getting that wrong turns every metal into a dielectric.
 */
#pragma once

#include "raylib.h"

#include "cromwell/material/MaterialLibrary.hpp"

#include <string>
#include <vector>

namespace cromwell {

class ModelAsset {
public:
    ModelAsset() = default;
    ~ModelAsset();

    ModelAsset(const ModelAsset&) = delete;
    ModelAsset& operator=(const ModelAsset&) = delete;

    /* Loads `path` and registers a material per mesh in `library`.
     *
     * `fallbackMaterial` is used for any mesh whose file carried no usable
     * material of its own — which is every mesh of an OBJ, and any glTF mesh
     * the exporter left untextured. */
    bool load(const char* path, const char* name, MaterialLibrary& library,
              MaterialLibrary::Handle fallbackMaterial);

    bool valid() const { return loaded_; }
    const std::string& name() const { return name_; }

    int meshCount() const { return model_.meshCount; }

    /* Draws every mesh with the material the file (or the fallback) gave it. */
    void drawLit(Matrix transform, const MaterialLibrary& library,
                 const class PbrShader& shader) const;

    /* Draws every mesh through ONE shader, for the shadow and prepass passes
     * that want only the silhouette. */
    void draw(Matrix transform, const Material& material) const;

    /* The mesh's own bounding box, before any instance transform — so a
     * placement can sit a prop ON the ground rather than through it. */
    BoundingBox bounds() const { return bounds_; }

private:
    /* Returns false and logs when a mesh cannot be lit at all. */
    bool conditionMeshes();

    /* Pulls a glTF material's textures into a library slot. */
    void adoptMaterials(MaterialLibrary& library, MaterialLibrary::Handle fallback);

    Model       model_{};
    std::string name_;
    BoundingBox bounds_{};
    bool        loaded_ = false;

    /* one material handle per mesh, parallel to model_.meshes */
    std::vector<MaterialLibrary::Handle> meshMaterials_;
};

}  // namespace cromwell
