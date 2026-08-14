/* DeviceMaterials.hpp — one material block per surface kind, on the device.
 *
 * SINGLE RESPONSIBILITY: own a std140 uniform buffer per material and bind the
 * right one before a draw.
 *
 * ================= WHY THIS EXISTS BESIDE MaterialLibrary =================
 *
 * MaterialLibrary does the same job for the raylib renderer and is untouched.
 * The same bargain RhiStatics makes against StaticsMesh: two while the
 * migration lasts, one at parity.
 *
 * It is a much SMALLER class than the one it will replace, and deliberately so.
 * MaterialLibrary also owns textures, and there are none to own yet — the
 * material PNGs it looks for do not exist in this tree, and the ones that DO
 * exist arrive attached to model files through ModelAsset. So this holds the
 * scalar factors and nothing else, and grows a texture per slot on the day
 * something has one. Building the texture half first would be building it
 * against no art.
 *
 * ==================== A BUFFER PER MATERIAL, NOT ONE REWRITTEN ============
 *
 * The obvious shape is one uniform buffer updated between draws. That is a
 * pipeline stall on every backend that means it, and on a console it is the
 * kind that does not show up until the frame is already built around it.
 *
 * Material factors change when someone edits them, which is approximately
 * never, so each slot owns its own buffer written once. Binding is then a pure
 * bind with no upload, which is what makes it safe to do per bucket inside a
 * pass. Twelve blocks of sixteen bytes is not memory worth economising.
 *
 * ====================== WHAT THE GAME DECIDES, AND WHAT THIS DOES ==========
 *
 * The game says WHICH material a bucket draws with; this says what a material
 * IS and how it binds. That split is what keeps the binding index — 2, the
 * per-material frequency in CONVENTIONS.md — a fact stated once in the engine
 * beside the shader that reads it, rather than a number the game has to know.
 */
#pragma once

#include "cromwell/rhi/Handles.hpp"
#include "cromwell/style/SurfaceKind.hpp"

#include <array>

namespace cromwell {

namespace rhi { class ICommandEncoder; class IRenderDevice; }

/* FORWARD-DECLARED, NOT INCLUDED, and that is the whole reason the setters
 * below take one by reference rather than this class holding one.
 *
 * PbrMaterial is the shared description — the same struct MaterialLibrary
 * stores per slot, with the same defaults — and reusing it is what stops the
 * two renderers disagreeing about what an 8% opaque pane means. But its header
 * pulls in raylib for Vector3 and Texture2D, and this one is included by
 * ScenePipeline, which must not name raylib at all.
 *
 * So the definition stays in the .cpp: the packing happens once, there, from
 * the shared description, and the header carries only the packed bytes. */
struct PbrMaterial;

/* FORWARD-DECLARED TOO, and legal because it is a scoped enum with an explicit
 * underlying type. It lives in PbrMaterial.hpp, which this header cannot
 * include for the reason above. */
enum class AlphaMode : int;

class DeviceMaterials {
public:
    explicit DeviceMaterials(rhi::IRenderDevice& device);
    ~DeviceMaterials();

    DeviceMaterials(const DeviceMaterials&) = delete;
    DeviceMaterials& operator=(const DeviceMaterials&) = delete;

    /* Creates one buffer per surface kind, filled with PbrMaterial's defaults.
     * False means no material can be bound and the caller should say so. */
    bool initialise();

    /* Overrides one kind's response. Uploads immediately — this is a cold path
     * called at startup or from a dev panel, never per frame.
     *
     * `roughness` is clamped away from zero: a perfectly smooth surface makes
     * the GGX denominator collapse to a division by nearly nothing, which
     * prints as a single blown-out pixel where the highlight should be. */
    void setFactors(SurfaceKind kind, float roughness, float metalness);

    /* THE WHOLE DESCRIPTION, packed the way the shaders want it — and packed
     * with the same expressions MaterialLibrary::factorsOf, glassParamsOf and
     * glassEdgeOf use, so a material authored once means the same thing to both
     * renderers. Prefer this over setFactors wherever a real material exists;
     * setFactors is the convenience for the two numbers most surfaces vary. */
    void setMaterial(SurfaceKind kind, const PbrMaterial& material);

    /* WHICH PASS THIS MATERIAL BELONGS IN, and it is the MATERIAL that decides
     * — not a hardcoded list of surface kinds, and not the renderer.
     *
     * That is the whole point of blend mode being a material property: a
     * surface becomes see-through by saying `blend translucent` in its .mat
     * file, and the submitter asks here rather than testing an enum. Water is
     * then a material, not a feature.
     *
     * It replaces isTransparent(SurfaceKind), which answered the same question
     * from a fixed switch in a header and could only ever be changed by
     * editing and rebuilding. */
    bool isTranslucent(SurfaceKind kind) const;

    /* Binds this kind's block at the per-material slot. A pipeline must already
     * be bound; this has no opinion about which pass is running, which is what
     * lets the prepass and the lit pass share it. */
    void bind(rhi::ICommandEncoder& encoder, SurfaceKind kind) const;

private:
    void release();

    rhi::IRenderDevice& device_;

    /* Indexed by SurfaceKind. An array rather than a map because the set is
     * closed and known at compile time — see SurfaceKind. */
    std::array<rhi::BufferHandle, kSurfaceKindCount> blocks_;

    /* Which pass each material belongs in, latched when it was loaded. Held
     * here rather than read back off the GPU block, which is write-only from
     * the CPU's point of view once uploaded. */
    std::array<AlphaMode, kSurfaceKindCount> blendModes_{};
};

}  // namespace cromwell
