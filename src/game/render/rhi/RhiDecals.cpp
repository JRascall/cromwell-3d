#include "game/render/rhi/RhiDecals.hpp"

#include "cromwell/decal/Decal.hpp"
#include "cromwell/decal/DecalSet.hpp"
#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/render/RenderAssets.hpp"
#include "cromwell/render/RenderScene.hpp"

#include <cstdio>

namespace game {
namespace {

/* raylib's Matrix to the engine's Mat4, AND IT IS A TRANSPOSE.
 *
 * THIS IS THE ONE LINE IN THE WHOLE CONVERSION THAT CAN BE SILENTLY WRONG, so
 * it is written out rather than assumed. Both types are column-major
 * MATHEMATICALLY — raylib's m12/m13/m14 are the translation and so are Mat4's
 * elements 12, 13 and 14 — but they disagree about MEMORY: raylib declares its
 * struct `m0, m4, m8, m12` first, so index 3 of its bytes is the translation's
 * x, where Mat4 puts that at index 12.
 *
 * Copying the sixteen floats straight across therefore transposes the matrix,
 * which for a decal means a box that is rotated and scaled by the wrong axes
 * and lands somewhere plausible but wrong — a projector that "works" on an
 * axis-aligned decal at the origin and drifts on every other one, which is the
 * hardest version of this bug to notice. The loop below is the transpose that
 * makes the two agree.
 *
 * Checked against a translation: Mat4's element 12 is column 3, row 0, which
 * reads raylib's byte index 0*4+3 = 3 — and byte 3 is m12. */
cromwell::Mat4 toMat4(const Matrix& matrix)
{
    const float* source = reinterpret_cast<const float*>(&matrix);

    cromwell::Mat4 out;
    for (int column = 0; column < 4; column++)
        for (int row = 0; row < 4; row++)
            out.m[column * 4 + row] = source[row * 4 + column];

    return out;
}

/* One projector, from one decal. Every field is a copy; the only conversions
 * are the matrix above and the colour below. */
cromwell::DeviceDecalSet::Projector toProjector(const cromwell::Decal& decal,
                                                cromwell::DeviceDecalMaterialId material)
{
    cromwell::DeviceDecalSet::Projector out;
    out.transform = toMat4(decal.transform);
    out.material  = material;

    /* CARRIED ACROSS, not re-derived. The device set is rebuilt wholesale every
     * frame and its order is the DRAW order, so the only stable name a decal has
     * is the one the authoritative list stamped on it. The preview has none — it
     * was never added to a set — and -1 is exactly right there: it moves with the
     * cursor, so nothing about it is worth caching. */
    out.id = decal.id;

    /* BYTE OVER 255 WITH NO sRGB DECODE, matching what RhiBodies does for a
     * body's tint and what the static world's vertex colours already do. The
     * tint multiplies an albedo map the hardware has already decoded, so
     * decoding this as well would darken every tinted decal by its own curve. */
    constexpr float kToUnit = 1.0f / 255.0f;
    out.tint[0] = static_cast<float>(decal.tint.r) * kToUnit;
    out.tint[1] = static_cast<float>(decal.tint.g) * kToUnit;
    out.tint[2] = static_cast<float>(decal.tint.b) * kToUnit;
    out.tint[3] = static_cast<float>(decal.tint.a) * kToUnit;

    out.opacity        = decal.opacity;
    out.emissive       = decal.emissive;
    out.roughness      = decal.roughness;
    out.metalness      = decal.metalness;
    out.normalStrength = decal.normalStrength;
    out.wrap           = decal.wrap;
    out.angleFadeStart = decal.angleFadeStart;
    out.angleFadeEnd   = decal.angleFadeEnd;
    out.depthFade      = decal.depthFade;

    return out;
}

}  // namespace

cromwell::DeviceDecalMaterialId RhiDecals::materialFor(cromwell::DeviceDecalSet& set,
                                                       cromwell::RenderAssets& assets,
                                                       const cromwell::DecalSet& decals,
                                                       int id)
{
    if (id < 0 || id >= static_cast<int>(decals.materialCount()))
        return cromwell::kInvalidDeviceDecalMaterial;

    const char* name = decals.materialName(id);
    if (name == nullptr || name[0] == '\0') return cromwell::kInvalidDeviceDecalMaterial;

    /* ALREADY ANSWERED, including when the answer was "no". Caching the refusal
     * is what stops a procedural material retrying its three file loads and
     * re-logging its warning on every frame of the run.
     *
     * AN EMPTY OPTIONAL IS NOT A REFUSAL — see the member's note. Decals arrive
     * in draw order rather than material order, so ids are asked for out of
     * sequence and an unasked slot must stay unasked. */
    const std::size_t slot = static_cast<std::size_t>(id);
    if (slot < deviceIds_.size() && deviceIds_[slot].has_value())
        return *deviceIds_[slot];

    if (slot >= deviceIds_.size()) deviceIds_.resize(slot + 1);

    /* ---- first sight: load its maps --------------------------------------
     *
     * THE SAME THREE NAMES DecalSet::findOrLoad USES, because they describe the
     * same files on disk. Two conventions for one directory is how a material
     * loads on one renderer and not the other. */
    char path[512];

    cromwell::DeviceDecalSet::Material material;

    std::snprintf(path, sizeof path, "materials/decals/%s_albedo.png", name);
    material.albedo = assets.texture(path);

    std::snprintf(path, sizeof path, "materials/decals/%s_normal.png", name);
    material.normal = assets.texture(path);

    std::snprintf(path, sizeof path, "materials/decals/%s_mrao.png", name);
    material.packed = assets.texture(path);

    const cromwell::DeviceDecalMaterialId registered = set.addMaterial(material);

    /* RECORDED EITHER WAY, valid or not — see the member's note on why the two
     * tables cannot be assumed to line up, and why the slot is written at its
     * own index rather than appended. */
    deviceIds_[slot] = registered;

    if (registered == cromwell::kInvalidDeviceDecalMaterial) {
        /* TWO CAUSES, AND THE MESSAGE HAS TO NAME BOTH, because from here they
         * are indistinguishable and they need opposite responses.
         *
         * Either the file is genuinely missing — an authoring slip — or the
         * material was BUILT rather than loaded. `DecalSet::registerTextures`
         * exists for procedural art and DecalDemo uses it for three of its
         * four marks, and a mirror keyed by NAME cannot reproduce a texture
         * that never had a file. That is a real limit of this converter rather
         * than a bug, and it goes when the two decal sets become one at parity;
         * until then the device path draws the authored materials and skips the
         * generated ones.
         *
         * A decal's alpha IS its shape, so there is no stand-in that would not
         * ink a solid rectangle over the world — which is why this is a skip
         * rather than a fallback. */
        LOGGER.warn("rhi decals: no albedo for '{}' - either the file is missing "
                    "or the material was built procedurally, which a mirror keyed "
                    "by name cannot follow. Decals using it are not drawn", name);
    }
    return registered;
}

void RhiDecals::sync(cromwell::RenderScene& scene, const cromwell::DecalSet& decals,
                     cromwell::RenderAssets& assets, const cromwell::Decal* preview)
{
    /* ONE ZONE FOR THE SYSTEM, per CLAUDE.md. It is a walk over tens of POD
     * structs and earns exactly one row — and it earns that one because an
     * unzoned per-frame system does not show up as a zero, it shows up as
     * nothing at all and inflates whatever encloses it. */
    CW_PROFILE_ZONE_N("decal sync");

    cromwell::DeviceDecalSet& set = scene.decals();
    set.clear();

    placedCount_ = 0;
    droppedCount_ = 0;

    /* IN DRAW ORDER, which is the set's own sort — later decals blend over
     * earlier ones and that order is authored rather than incidental. Taking
     * the unsorted list would put a scorch mark over the blood it was meant to
     * sit under, which reads as the blend being wrong. */
    for (const cromwell::Decal& decal : decals.inDrawOrder()) {
        const cromwell::DeviceDecalMaterialId material =
            materialFor(set, assets, decals, decal.material);

        if (material == cromwell::kInvalidDeviceDecalMaterial) {
            droppedCount_++;
            continue;
        }

        set.add(toProjector(decal, material));
        placedCount_++;
    }

    /* THE GHOST, LAST, so it composites over everything committed. */
    if (preview != nullptr) {
        const cromwell::DeviceDecalMaterialId material =
            materialFor(set, assets, decals, preview->material);

        if (material != cromwell::kInvalidDeviceDecalMaterial)
            set.add(toProjector(*preview, material));
    }
}

}  // namespace game
