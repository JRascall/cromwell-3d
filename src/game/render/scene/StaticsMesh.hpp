/* StaticsMesh.hpp — the baked static world, one mesh per storey and material.
 *
 * SINGLE RESPONSIBILITY: own the GPU meshes and draw them. What they contain
 * is StoreyGeometryEmitter's business.
 *
 * THREE AXES, FOR THREE DIFFERENT REASONS.
 *   per STOREY, so floor isolation stays a draw-time decision and costs
 *     nothing — the cutaway just stops iterating;
 *   per MATERIAL, because a draw call binds one texture set. Splitting here is
 *     what lets a wall and a window carry different albedo, normal and mrao
 *     maps while the whole world is still submitted in a couple of dozen
 *     calls;
 *   per FACING, so removing the walls between the camera and a building's
 *     interior is the same kind of decision as the storey cut — skip some
 *     buckets — rather than a per-fragment test. See SurfaceFacing.hpp for why
 *     the facing is baked rather than derived per frame.
 *
 * EVERY DRAW TAKES A CutawayView, AND ITS DEFAULT DRAWS EVERYTHING. That is
 * deliberate and it is load-bearing: the sun's depth pass and the probe
 * capture must see the world as it IS, not as the camera has been asked to
 * show it, or the lighting starts changing when the player changes floor. A
 * pass that passes nothing gets the whole lattice. See CutawayView.hpp.
 *
 * RAII: the destructor unloads. The C version needed a matching xcStaticsFree
 * on every exit path.
 */
#pragma once

#include "raylib.h"

#include "game/render/scene/CutawayView.hpp"
#include "game/world/World.hpp"
#include "cromwell/material/MaterialLibrary.hpp"
#include "cromwell/style/SurfaceFacing.hpp"
#include "cromwell/style/SurfaceKind.hpp"

#include <array>
#include <vector>

/* Forward-declared in the namespace they actually live in. These are
 * cromwell's types; declaring them inside game invents a second,
 * never-defined class of the same name. */
namespace cromwell {
class PbrShader;
class PrepassShader;
}  // namespace cromwell

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */


class StaticsMesh {
public:
    StaticsMesh();
    ~StaticsMesh();

    StaticsMesh(const StaticsMesh&) = delete;
    StaticsMesh& operator=(const StaticsMesh&) = delete;

    /* Safe to call repeatedly — rebuild after anything edits the dataset
     * (destruction, reset). That is the whole contract: edit the data, call
     * rebuild, and the world reflects it. */
    void rebuild(const World& world);

    /* Every sub-mesh through ONE shader, for the passes that only want the
     * geometry's silhouette: the sun's shadow map and the scene prepass.
     *
     * `castersOnly` drops the surfaces that transmit light rather than block
     * it — see castsSunShadow. The shadow map wants it; the prepass does not,
     * because a window is still solid geometry as far as the ribbon's depth
     * test and SSAO are concerned. */
    void draw(const CutawayView& cutaway, const Material& material,
              bool castersOnly = false) const;

    /* The same geometry through the prepass shader, but pushing each kind's
     * roughness so the G-buffer's alpha means something. One shared program
     * still, so this is the same single pass — it just stops being blind to
     * which material it is drawing. */
    void drawPrepass(const CutawayView& cutaway, const Material& material,
                     const MaterialLibrary& library, const PrepassShader& shader) const;

    /* Just one material class — used to draw the glass on its own into the
     * shadow map's transmission plane. */
    void drawKind(const CutawayView& cutaway, SurfaceKind kind,
                  const Material& material) const;

    /* Each sub-mesh through its own material and scalar factors.
     *
     * Split opaque from transparent because see-through surfaces have to come
     * last, once there is something behind them to see.
     *
     * `includeTransparent` folds them back in, for the flat geometry view —
     * there the whole point is to see where a surface EXISTS, so glass is
     * drawn solid in the ordinary pass with depth writes on rather than
     * blended afterwards. */
    void drawLit(const CutawayView& cutaway, const MaterialLibrary& library,
                 const PbrShader& shader, bool includeTransparent = false) const;
    void drawTransparentLit(const CutawayView& cutaway, const MaterialLibrary& library,
                            const PbrShader& shader) const;

    int triangleCount() const { return triangleCount_; }
    int drawCallCount() const { return drawCalls_; }

private:
    void unloadMeshes();

    struct StoreyMesh {
        Mesh mesh = { 0 };
        bool built = false;
    };

    /* One bucket per (kind, facing) pair, flattened. Most stay unbuilt —
     * only walls and glass ever carry a facing — and an unbuilt bucket costs
     * a bool in the array and nothing on the GPU. */
    static constexpr int kBucketCount = kSurfaceKindCount * kSurfaceFacingCount;

    static constexpr std::size_t slot(SurfaceKind kind, SurfaceFacing facing)
    {
        return indexOf(kind) * static_cast<std::size_t>(kSurfaceFacingCount) + indexOf(facing);
    }

    static constexpr SurfaceKind   kindOfSlot(int slotIndex)
    {
        return static_cast<SurfaceKind>(slotIndex / kSurfaceFacingCount);
    }
    static constexpr SurfaceFacing facingOfSlot(int slotIndex)
    {
        return static_cast<SurfaceFacing>(slotIndex % kSurfaceFacingCount);
    }

    /* [storey][kind * facings + facing] */
    std::vector<std::array<StoreyMesh, kBucketCount>> storeys_;
    int triangleCount_ = 0;
    int drawCalls_ = 0;
};

}  // namespace game
