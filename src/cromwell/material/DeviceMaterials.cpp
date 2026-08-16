#include "cromwell/material/DeviceMaterials.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/material/MaterialDefinition.hpp"
#include "cromwell/material/PbrMaterial.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <algorithm>

namespace cromwell {
namespace {

/* ==================== WHAT A MATERIAL IS, ON THE DEVICE ==================
 *
 * The whole PbrMaterial description, packed the way the shaders read it at
 * binding 2. Every group below is one of MaterialLibrary's accessors — the
 * expressions are copied from factorsOf, optionsOf, glassParamsOf, glassEdgeOf,
 * transmissionOf, glassGrimeOf and glassRemapOf, in that order — so a material
 * authored once means the same thing to both renderers. That is the point of
 * packing from the shared struct rather than from loose arguments.
 *
 * ONE BLOCK FOR OPAQUE AND TRANSPARENT ALIKE, and that is deliberate rather
 * than wasteful. A transparent surface here is not a different KIND of thing
 * with its own path — it is the same metal/rough material with OPACITY and
 * TRANSMISSION switched on, which is exactly the shape a translucent material
 * has in any modern authoring tool: the same base colour, roughness, metallic
 * and normal inputs, plus an opacity input and a transmittance colour.
 *
 * DIRT IS NOT A FEATURE HERE, AND SHOULD NOT BE. The raylib path grew a bespoke
 * "grime" layer with its own colour and roughness, driven by a translucency
 * texture — and that texture does not exist anywhere in this tree, so the whole
 * layer is inert. It is also the wrong shape: marks on a pane are a TEXTURE
 * feeding the standard inputs, raising opacity and roughness and tinting the
 * base colour where the dirt is. Authored that way it needs no new material
 * concept, no new uniform and no new code — which is the test of whether an
 * input belongs in a PBR model at all.
 *
 * So the opaque shader reads the first two groups and ignores the rest, the
 * transparent one reads all five, and dirtying a window is a map plugged into
 * inputs that already exist. 80 bytes per surface kind is nothing against
 * that.
 *
 * Kept in step with the shaders by hand — there is no reflection on the
 * explicit backends. The static_assert pins the size; the field comments in
 * rhi/transparent.fs.glsl name the same groups in the same order. */
struct MaterialBlockData {
    float factors[4]      = { 0.8f, 0.0f, 1.0f, 1.0f };  /* rough, metal, nrm, uv   */
    float options[4]      = { 0.0f, 0.0f, 0.5f, 0.0f };  /* packing, alphaMode, cut */

    /* OPACITY, AND THE FRESNEL RAMP THAT VARIES IT WITH VIEW ANGLE. See the
     * note above on why this is one standard input and not a stack of effects. */
    float opacity[4]      = { 0.08f, 4.0f, 1.0f, 1.0f };  /* base, falloff, max, ramp */
    float tint[4]         = { 1.0f, 1.0f, 1.0f, 0.0f };   /* edge rgb, thickness    */
    float transmission[4] = { 1.0f, 1.0f, 1.0f, 0.0f };   /* colour rgb, amount     */

    /* WHAT THE SUN BECOMES CROSSING THIS SURFACE, per channel — a different
     * question from the two above, which are what the EYE sees looking at a
     * backlit surface. This is read in the SHADOW pass and is what makes a
     * window cast a coloured patch rather than a hole.
     *
     * The tint and the scalar dimming PbrMaterial carries separately are
     * multiplied together here: that split exists because the raylib path's
     * transmission plane is one channel and cannot hold a colour. Ours can. */
    float sunTransmittance[4] = { 1.0f, 1.0f, 1.0f, 0.0f };

    /* RADIANCE THE SURFACE PRODUCES, in linear units, PREMULTIPLIED — the
     * authored colour times the authored strength.
     *
     * PACKED AS A PRODUCT rather than carried as a colour and a scalar, which
     * is the same choice sunTransmittance above makes and for the same reason:
     * the shader wants the radiance, the two halves never vary per fragment,
     * and splitting them would be a multiply in every pixel of every emissive
     * surface to preserve a distinction only the .mat file cares about.
     *
     * ZERO BY DEFAULT, so a material that says nothing about emission adds a
     * constant zero and nothing changes. w spare. */
    float emissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(MaterialBlockData) == 112, "std140: seven vec4");

/* THE PER-MATERIAL BINDING, from the frequency table in CONVENTIONS.md: 0 is
 * the frame, 1 the pass, 2 the material, 3 the object. Named here because this
 * is the one place that binds it. */
constexpr uint32_t kMaterialSlot = 2;

}  // namespace

DeviceMaterials::DeviceMaterials(rhi::IRenderDevice& device) : device_(device) {}

DeviceMaterials::~DeviceMaterials() { release(); }

void DeviceMaterials::release()
{
    for (rhi::BufferHandle& block : blocks_) {
        if (block.valid()) device_.destroy(block);
        block = {};
    }
}

bool DeviceMaterials::initialise()
{
    release();

    for (int i = 0; i < kSurfaceKindCount; i++) {
        rhi::BufferDesc desc;
        desc.name  = nameOf(static_cast<SurfaceKind>(i));
        desc.bytes = sizeof(MaterialBlockData);
        desc.usage = rhi::BufferUsageUniform;

        /* WRITTEN AT STARTUP AND ON AN EDIT, never per frame — so the backend
         * is free to put it somewhere the CPU cannot reach between writes. */
        desc.access = rhi::BufferAccess::CpuToGpuOnce;

        blocks_[static_cast<std::size_t>(i)] = device_.createBuffer(desc);
        if (!blocks_[static_cast<std::size_t>(i)].valid()) {
            LOGGER.error("DeviceMaterials: could not create the block for '{}'", desc.name);
            return false;
        }

        /* WHATEVER THE MATERIAL FILE SAYS, or PbrMaterial's defaults where
         * there is none. This is what makes a new material a file rather than a
         * rebuild — see MaterialDefinition. Nothing in C++ names a surface's
         * roughness, its opacity or its blend mode. */
        PbrMaterial material;
        loadMaterialDefinition(desc.name, material);

        blendModes_[static_cast<std::size_t>(i)] = material.alphaMode;
        setMaterial(static_cast<SurfaceKind>(i), material);
    }

    return true;
}

bool DeviceMaterials::isTranslucent(SurfaceKind kind) const
{
    const std::size_t index = indexOf(kind);
    if (index >= blendModes_.size()) return false;
    return blendModes_[index] == AlphaMode::Blend;
}

void DeviceMaterials::setFactors(SurfaceKind kind, float roughness, float metalness)
{
    PbrMaterial material;
    material.roughness = roughness;
    material.metalness = metalness;
    setMaterial(kind, material);
}

void DeviceMaterials::setMaterial(SurfaceKind kind, const PbrMaterial& material)
{
    const std::size_t index = indexOf(kind);
    if (index >= blocks_.size() || !blocks_[index].valid()) return;

    MaterialBlockData block;

    /* ---- factorsOf ------------------------------------------------------
     *
     * ROUGHNESS FLOORED WELL ABOVE ZERO. A perfectly smooth surface collapses
     * the GGX denominator towards nothing and the highlight becomes one
     * blown-out pixel that aliases into a crawling sparkle as the camera
     * moves. 0.045 is the same floor pbr.fs.glsl clamps to. */
    block.factors[0] = std::clamp(material.roughness, 0.045f, 1.0f);
    block.factors[1] = std::clamp(material.metalness, 0.0f, 1.0f);
    block.factors[2] = material.normalStrength;
    block.factors[3] = material.uvScale;

    /* ---- optionsOf ------------------------------------------------------
     *
     * ALPHA MODE IS DECLARED, NEVER INFERRED from an albedo's alpha channel —
     * see AlphaMode. It reaches the shader as a float because a std140 block
     * has no integers worth the trouble, and the shader compares against 1.5
     * exactly as pbr.fs.glsl does. */
    block.options[0] = static_cast<float>(static_cast<int>(material.packing));
    block.options[1] = static_cast<float>(static_cast<int>(material.alphaMode));
    block.options[2] = material.alphaCutoff;

    /* ---- opacity, and the Fresnel ramp over it --------------------------
     *
     * WHY OPACITY VARIES WITH VIEW ANGLE AT ALL. A dielectric reflects about 4%
     * of what hits it head-on and approaches 100% edge-on, and since what is
     * not reflected is transmitted, that Fresnel curve IS the opacity curve.
     * A pane is nearly clear looking through it and turns to a bright sheet at
     * a grazing angle — which is most of what makes glass read as a surface
     * rather than a hole, and it falls out of the physics rather than being
     * authored.
     *
     * Regrouped from MaterialLibrary's glassParamsOf/glassEdgeOf so that the
     * BASE OPACITY sits at the head of the opacity group where a reader looks
     * for it, rather than in the fourth channel of the tint. Same numbers, same
     * meanings, better order. */
    block.opacity[0] = material.baseOpacity;
    block.opacity[1] = material.edgeFalloff;
    block.opacity[2] = material.edgeMaxOpacity;
    block.opacity[3] = material.opacityScale;

    block.tint[0] = material.edgeColour.x;
    block.tint[1] = material.edgeColour.y;
    block.tint[2] = material.edgeColour.z;
    block.tint[3] = material.edgeThickness;

    /* ---- transmissionOf -------------------------------------------------
     *
     * NOT GLASS-ONLY. This is light arriving through a thin surface from
     * behind, which is the same question for a leaf, an ear and a coloured
     * pane — see PbrMaterial. Zero by default, so every opaque material pays a
     * branch that is never taken. */
    block.transmission[0] = material.transmissionColour.x;
    block.transmission[1] = material.transmissionColour.y;
    block.transmission[2] = material.transmissionColour.z;
    block.transmission[3] = material.transmissionAmount;

    /* THE PRODUCT, because the plane holds a colour. See the field. */
    block.sunTransmittance[0] = material.transmissionTint.x * material.paneTransmittance;
    block.sunTransmittance[1] = material.transmissionTint.y * material.paneTransmittance;
    block.sunTransmittance[2] = material.transmissionTint.z * material.paneTransmittance;

    /* THE PRODUCT AGAIN, for the same reason: the shader wants radiance, and
     * neither half varies per fragment. A material that authored no emission
     * leaves this at zero and the surface shaders add nothing. */
    block.emissive[0] = material.emissiveColour.x * material.emissiveStrength;
    block.emissive[1] = material.emissiveColour.y * material.emissiveStrength;
    block.emissive[2] = material.emissiveColour.z * material.emissiveStrength;

    /* NOTHING FOR GRIME, DELIBERATELY — see the note on the block. Marks on a
     * pane belong in a texture feeding opacity, roughness and base colour,
     * which are all above. PbrMaterial still carries the raylib path's grime
     * fields and they are ignored here. */

    device_.updateBuffer(blocks_[index], &block, sizeof block, 0);
}

void DeviceMaterials::bind(rhi::ICommandEncoder& encoder, SurfaceKind kind) const
{
    const std::size_t index = indexOf(kind);
    if (index >= blocks_.size() || !blocks_[index].valid()) return;

    encoder.bindUniformBuffer(kMaterialSlot, blocks_[index]);
}

/* ---- the same table, addressed by opaque id — see the header -------------*/

MaterialId DeviceMaterials::idOf(SurfaceKind kind)
{
    return MaterialId{ static_cast<std::uint32_t>(indexOf(kind)) + 1u };
}

int DeviceMaterials::slotOf(MaterialId material)
{
    if (!material.valid()) return -1;

    const int slot = static_cast<int>(material.value) - 1;
    return slot < kSurfaceKindCount ? slot : -1;
}

bool DeviceMaterials::isTranslucent(MaterialId material) const
{
    const int slot = slotOf(material);

    /* AN ID THIS TABLE CANNOT ANSWER FOR IS OPAQUE, which is the safe half of
     * the guess — see IMaterialQuery.hpp. An unknown surface in the blended
     * pass would draw without depth writes over whatever was behind it, which
     * reads as a transparency bug rather than as a missing material. */
    if (slot < 0) return false;

    return blendModes_[static_cast<std::size_t>(slot)] == AlphaMode::Blend;
}

void DeviceMaterials::bind(rhi::ICommandEncoder& encoder, MaterialId material) const
{
    const int slot = slotOf(material);
    if (slot < 0) return;

    const std::size_t index = static_cast<std::size_t>(slot);
    if (!blocks_[index].valid()) return;

    encoder.bindUniformBuffer(kMaterialSlot, blocks_[index]);
}

}  // namespace cromwell
