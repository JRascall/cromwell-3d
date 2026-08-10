/* MaterialLibrary.hpp — every material in the game, loaded and ready to draw.
 *
 * SINGLE RESPONSIBILITY: own the PBR materials — their textures, their scalar
 * factors, and the raylib Material handles the draw calls need — for the life
 * of the program.
 *
 * MATERIALS ARE NAMED, not enumerated. The twelve SurfaceKinds are simply the
 * built-in names, created up front; anything else — a crate model's own
 * material, a trim sheet, a hero prop — is created on demand by name. Keying
 * on an enum was right while the only geometry was generated boxes and stopped
 * being right the moment a model file could bring its own textures.
 *
 * ASSETS ARE OPTIONAL, AND THAT IS THE POINT. Each name looks for
 *
 *     assets/materials/<name>_albedo.png
 *     assets/materials/<name>_normal.png
 *     assets/materials/<name>_mrao.png
 *
 * and quietly falls back to a shared 1x1 white (or a flat normal) for anything
 * missing. With no files at all the world renders exactly as the untextured
 * placeholder it is today, driven entirely by vertex colour and the scalar
 * factors; drop a texture set in and that surface picks it up with no code
 * change, no shader permutation and no rebuild.
 *
 * COLOUR SPACE. Albedo is authored in sRGB and decoded in the shader; normal
 * and the packed map are data, not colour, and are sampled raw. Loading a
 * normal map through an sRGB decode is the classic way to get subtly wrong
 * lighting that nobody can find, so it is worth being explicit: only albedo is
 * decoded, and only in one place (pbr.fs.glsl).
 */
#pragma once

#include "raylib.h"

#include "render/material/PbrMaterial.hpp"
#include "render/style/SurfaceKind.hpp"

#include <memory>
#include <string>
#include <vector>

namespace xcom {

class MaterialLibrary {
public:
    MaterialLibrary() = default;
    ~MaterialLibrary();

    MaterialLibrary(const MaterialLibrary&) = delete;
    MaterialLibrary& operator=(const MaterialLibrary&) = delete;

    /* Creates the fallbacks and the built-in SurfaceKind materials. `shader`
     * is the lit program every material draws through. */
    bool load(Shader shader);

    /* An opaque, stable handle. Indices never change once issued. */
    using Handle = int;
    static constexpr Handle kInvalid = -1;

    Handle handleOf(SurfaceKind kind) const;

    /* Finds `name`, or creates it and looks for its texture files. Returns a
     * handle that stays valid for the life of the library. */
    Handle findOrCreate(const char* name);

    /* Adopts textures a model file brought with it, rather than looking on
     * disk. Anything passed with id 0 keeps the shared fallback. The library
     * does NOT take ownership — the ModelAsset that loaded them unloads them.
     *
     * `packing` matters: glTF hands over an ORM texture, not an MRAO one. */
    void adoptTextures(Handle handle, Texture2D albedo, Texture2D normal,
                       Texture2D packed, ChannelPacking packing);

    /* Overrides the scalar factors, for a material whose maps do not carry
     * everything (or which has no maps at all). */
    void setFactors(Handle handle, float roughness, float metalness);

    /* Transparency is DECLARED, never inferred from an albedo's alpha channel
     * — see AlphaMode. Materials are opaque until told otherwise. */
    void setAlphaMode(Handle handle, AlphaMode mode, float cutoff = 0.5f);

    /* Light arriving through the surface from behind — coloured glass with the
     * sun on the far side, and eventually foliage and cloth. Not glass-only,
     * so it is its own setter rather than part of setGlass. */
    void setTransmission(Handle handle, Vector3 colour, float amount);

    /* (transmissionColour.rgb, transmissionAmount). */
    Vector4 transmissionOf(Handle handle) const;

    /* Glass is per material, so a grimy warehouse pane, a frosted bathroom
     * window and clean shopfront glass are three materials rather than three
     * shaders. Only consulted when the material is Blend. */
    void setGlass(Handle handle, const PbrMaterial& glass);

    /* (edgeThickness, edgeFalloff, edgeMaxOpacity, opacityScale) and
     * (edgeColour.rgb, baseOpacity) and (remapMin, remapMax) and
     * (grimeColour.rgb, grimeRoughness). */
    Vector4 glassParamsOf(Handle handle) const;
    Vector4 glassEdgeOf(Handle handle) const;
    Vector2 glassRemapOf(Handle handle) const;
    Vector4 glassGrimeOf(Handle handle) const;

    /* (transmissionTint.rgb, paneTransmittance) — what the SUN loses crossing
     * the pane, as opposed to what the eye sees looking at it. Needed in two
     * places at once: the shadow pass writes it, the lit pass decodes it. */
    Vector4 glassTransmissionOf(Handle handle) const;

    /* The translucency map itself, for the shadow pass — dirt and patterns
     * have to dim what passes through, not just how the pane looks. */
    Texture2D translucencyOf(SurfaceKind kind) const;

    /* The raylib handle to draw a mesh of this material with. */
    const Material& material(Handle handle) const;
    const Material& material(SurfaceKind kind) const { return material(handleOf(kind)); }

    /* (roughness, metalness, normalStrength, uvScale) and (packing, 0, 0, 0),
     * the shapes the lit shader wants them in. */
    Vector4 factorsOf(Handle handle) const;
    Vector4 factorsOf(SurfaceKind kind) const { return factorsOf(handleOf(kind)); }
    Vector4 optionsOf(Handle handle) const;
    Vector4 optionsOf(SurfaceKind kind) const { return optionsOf(handleOf(kind)); }

    int materialCount() const { return static_cast<int>(slots_.size()); }
    int texturedCount() const { return texturedCount_; }

private:
    /* Held by pointer so the addresses material() hands out survive a material
     * being created later — a prop manifest can add materials after the
     * built-ins, and a vector that reallocated would dangle every reference
     * already given to a renderer. */
    struct Slot {
        std::string name;
        PbrMaterial description;
        Material    handle{};
        bool        ownsTextures = false;
    };

    void createFallbacks();
    Handle createSlot(const char* name);
    void   loadTexturesFor(Slot& slot);
    void   applyTexturesToHandle(Slot& slot);

    /* Loads `path` if it exists, with mipmaps and repeat wrapping, and returns
     * `fallback` if it does not. */
    Texture2D loadOrFallback(const char* path, Texture2D fallback, bool& foundAny);

    std::vector<std::unique_ptr<Slot>> slots_;
    Shader shader_{};

    /* Shared 1x1 defaults. `white_` is rlgl's own default texture, so it is
     * borrowed rather than owned; `flatNormal_` is ours and is unloaded. */
    Texture2D white_{};
    Texture2D flatNormal_{};

    /* Every texture this class actually loaded, so the destructor can unload
     * exactly those and neither the shared fallbacks nor a model's own. */
    std::vector<Texture2D> owned_;

    int  texturedCount_ = 0;
    bool loaded_ = false;
};

}  // namespace xcom
