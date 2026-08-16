/* RenderAssets.hpp — what the DEVICE owns, as opposed to what a WORLD owns.
 *
 * SINGLE RESPONSIBILITY: hold the render resources that are shared by every
 * scene and every view, and outlive all of them.
 *
 * =========================== PUBLIC API — see Renderable.hpp ===============
 *
 * ============================= WHY THIS EXISTS ============================
 *
 * Because "who owns the materials" turned out to have a wrong answer that had
 * already been shipped, and the render scene is what made it visible.
 *
 * `DeviceMaterials` lived on `ScenePipeline`. A pipeline is a VIEWPOINT — it
 * owns the render targets, the pass order and the formats — and materials are
 * none of those things: they are loaded from `.mat` files at startup and shared
 * by everything that draws. With one pipeline in the process nothing was wrong.
 * With the requirement this design is FOR — N players, N screens, possibly N
 * worlds, possibly N pipelines at different quality settings — the same table
 * would exist once per pipeline, be loaded once per pipeline, and drift.
 *
 * The reflection probes had the same smell for the OPPOSITE reason and moved
 * the other way: a probe set describes a WORLD, so it belongs to the scene. See
 * RenderScene.hpp. The two corrections together are rhi/MIGRATION.md §4.12's
 * first open problem, closed.
 *
 * ======================= THE THREE LIFETIMES, NAMED ======================
 *
 * This is the useful thing to take away, and it is why the class is worth
 * having even while it holds one member:
 *
 *   DEVICE  — RenderAssets.  Materials, and later meshes, textures and shaders.
 *             One per device. Outlives every world.
 *   WORLD   — RenderScene.   The renderables, and the reflection probes that
 *             describe those rooms. One per world.
 *   VIEW    — ScenePipeline. Targets, formats, sizes, the pass order. One per
 *             distinct quality configuration.
 *
 * Every ownership question this renderer has had was an object filed under the
 * wrong one of those three. Naming them is what makes the next one answerable
 * before it ships rather than after.
 *
 * ==================== AND IT IS WHERE THE MESH RULE POINTS ================
 *
 * RenderScene states that a scene REFERENCES device resources and never owns
 * one, and that shared geometry belongs to the asset layer. This is the asset
 * layer. Two worlds drawing the same unit cube put that cube here, and tearing
 * down one world cannot dangle the other's renderables — which is the failure
 * that rule exists to prevent and which needed somewhere concrete to point at.
 *
 * ========================= WHAT IT IS NOT, YET ===========================
 *
 * Not a mesh cache, not a shader cache and not an asset database. §4.7 is the
 * material system — authored `.mat` with textures, shading models,
 * per-material shaders and instances — and §4.9 is the shader toolchain that
 * makes the second half of that possible. This class exists so that when they
 * land they have an owner that is already the right shape, rather than a
 * pipeline they have to be prised out of afterwards. **Do not grow it ahead of
 * them.**
 *
 * ============ IT IS NOW A TEXTURE CACHE, AND THAT RULE STILL HOLDS =========
 *
 * The line above said "not a texture cache" and it grew one, which is worth
 * explaining rather than quietly editing: it grew one BECAUSE A CALLER ARRIVED,
 * not in anticipation of §4.7. The decals (§4.6) need an albedo map — a decal's
 * ALPHA is its shape, so it is the one map that cannot fall back to a constant
 * — and a decal system without one inks a solid rectangle.
 *
 * So the cache is the smallest thing that serves that: name in, device texture
 * out, one copy per name for the life of the device. What it deliberately is
 * NOT is a streaming system, a residency manager, a format negotiator or a
 * mip-chain policy. Those are §4.7's questions and answering them here, before
 * anything asks, is the growth this header warns against.
 */
#pragma once

#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <string>
#include <unordered_map>

namespace cromwell {

class IFileSystem;
class IImageDecoder;
struct DecodedImage;

namespace rhi { class IRenderDevice; }

class RenderAssets {
public:
    /* THREE REFERENCES RATHER THAN AN IPlatform, so the header states what this
     * actually needs: a device to make textures on, somewhere to read bytes
     * from, and something that turns those bytes into pixels. Taking the whole
     * platform would let it quietly acquire a window, a clock and an input
     * device, none of which an asset layer has any business with. */
    RenderAssets(rhi::IRenderDevice& device, IFileSystem& files, IImageDecoder& images);
    ~RenderAssets();

    RenderAssets(const RenderAssets&) = delete;
    RenderAssets& operator=(const RenderAssets&) = delete;

    /* Brings up everything that needs the device. False means nothing can be
     * drawn correctly; the failing stage has already logged which it was. */
    bool initialise();

    /* THE MATERIALS. Non-const because what a surface is made of is authored
     * rather than fixed — the loader writes them at startup and the dev panel
     * may write them again. What a material IS stays the engine's; the values
     * in it are the project's.
     *
     * RenderScene takes the const half of this as its IMaterialQuery. */
    DeviceMaterials&       materials() { return materials_; }
    const DeviceMaterials& materials() const { return materials_; }

    /* ---- ONE TEXTURE, BY ASSET NAME, CACHED FOR THE LIFE OF THE DEVICE ----
     *
     * `name` is a StorageKind::Asset path — "materials/decals/example_albedo.png".
     *
     * AN INVALID HANDLE MEANS "NOT THERE", AND THE CALLER MUST DECIDE. There is
     * deliberately no fallback baked in here, because the right fallback is the
     * caller's question and getting it wrong is silent: a missing NORMAL map
     * should become a flat normal and cost nothing, while a missing decal
     * ALBEDO must abandon the decal entirely, since its alpha is the decal's
     * shape and a white stand-in would ink a solid rectangle over the world.
     * One fallback cannot be both. `white()` and `flatNormal()` are here for
     * callers that want the cheap one.
     *
     * A FAILED LOAD IS CACHED TOO, as an invalid handle. Otherwise a missing
     * file is a filesystem hit and a decode attempt every frame something asks
     * for it, which is the shape of performance bug that only appears on the
     * machine where the asset is absent. */
    rhi::TextureHandle texture(const char* name);

    /* THE TWO STAND-INS EVERY MATERIAL SYSTEM NEEDS, 1x1 and made once.
     *
     * WHY A TEXTURE AND NOT A BRANCH IN THE SHADER. A pipeline's bindings are
     * the same every frame, so a slot left unbound reads as whatever was there
     * before — and a shader variant per missing map is exactly the permutation
     * explosion §4.7 is designed against. One texel costs a bind. */
    rhi::TextureHandle white() const { return white_; }
    rhi::TextureHandle flatNormal() const { return flatNormal_; }

private:
    /* Uploads one decoded image as an RGBA8 texture. */
    rhi::TextureHandle upload(const char* label, const DecodedImage& image);

    rhi::IRenderDevice& device_;
    IFileSystem&        files_;
    IImageDecoder&      images_;

    DeviceMaterials materials_;

    /* KEYED BY NAME, WHICH IS THE ONLY IDENTITY A CALLER HAS. The map owns
     * nothing the device does not; release() destroys every value.
     *
     * `std::string` keys rather than pointers, because a caller naming a
     * literal and a caller naming a composed path must hit the same entry —
     * see §5 on borrowing an address as a cache key and inheriting the owner's
     * invalidation with it. */
    std::unordered_map<std::string, rhi::TextureHandle> textures_;

    rhi::TextureHandle white_;
    rhi::TextureHandle flatNormal_;
};

}  // namespace cromwell
