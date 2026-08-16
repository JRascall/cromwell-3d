/* RhiStatics.hpp — the static world, as renderables the engine owns.
 *
 * SINGLE RESPONSIBILITY: build one device mesh per (storey, chunk, kind,
 * facing) from the world and REGISTER each in a RenderScene. It draws nothing.
 *
 * ================ IT USED TO SUBMIT, AND NOW IT REGISTERS =================
 *
 * The difference is the whole of rhi/MIGRATION.md §4.12. This class used to be
 * handed a command encoder and a cutaway, and decided per pass which buckets to
 * draw: casters only for the sun, the player's cut for the camera, the whole
 * lattice for a probe. That is a game deciding WHAT and HOW is drawn, and every
 * project on this engine would have had to write it again.
 *
 * Now it says what EXISTS and where, once, and the engine culls it, sorts it
 * and draws it. The three decisions it used to make each moved somewhere they
 * cannot be got wrong:
 *
 *   casters only, for the sun   -> castsShadow on the renderable, which is a
 *                                  property of the surface rather than of the
 *                                  pass asking
 *   opaque vs translucent       -> the material's blend mode, already authored
 *                                  in the `.mat` and already data
 *   lattice vs the player's cut -> filter flags on the renderable and hidden
 *                                  flags on the VIEW, so the sun's view and a
 *                                  probe's cannot inherit a cutaway
 *
 * ================ WHY IT IS CHUNKED, WHICH IS NEW WORK ===================
 *
 * One mesh per (storey, kind, facing) for a whole map has bounds the size of
 * the map: a frustum test on it is always true, so there is nothing to cull.
 * `IGeometrySource` let us not think about that, because the engine did not own
 * the list and could not have culled anyway.
 *
 * A chunk is kChunkTiles square, per storey, and it pays twice: culling gets
 * something to reject, and a world edit re-uploads one chunk rather than a
 * storey. That second half is what §4.3's probe re-placement has been waiting
 * for — "there is nothing to hook to" was true only while the world was built
 * once and never rebuilt.
 *
 * WHAT IT COSTS is draw calls: a bucket that was one draw for the whole map is
 * now one per chunk it appears in. That is the trade a culler is, and it is the
 * right way round — a rejected chunk costs one frustum test, where a submitted
 * one costs vertex work for geometry nobody can see.
 *
 * ==================== WHY THIS EXISTS BESIDE StaticsMesh ==================
 *
 * StaticsMesh does the same job for the raylib renderer and is untouched. Two
 * builders rather than one that feeds both, deliberately: the alternative was
 * StaticsMesh holding raylib meshes AND device meshes together, which doubles
 * the static world's GPU memory for as long as the migration lasts — on the
 * SHIPPING path, to serve a development one. This file replaces it at parity.
 *
 * ======================= AND IT OWNS ITS OWN MESHES =======================
 *
 * A scene REFERENCES device resources and never destroys one; whoever built a
 * mesh destroys it, after removing every renderable naming it. See
 * RenderScene.hpp for the rule and RenderAssets.hpp for where a mesh that two
 * worlds share would live instead. These are built from THIS world's tile data
 * and are useless to any other, so they belong here.
 */
#pragma once

#include "cromwell/geometry/SurfaceBuffers.hpp"
#include "cromwell/render/Renderable.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <vector>

namespace cromwell {
class RenderScene;
namespace rhi { class IRenderDevice; }
}  // namespace cromwell

namespace game {

class World;

class RhiStatics {
public:
    explicit RhiStatics(cromwell::rhi::IRenderDevice& device);
    ~RhiStatics();

    RhiStatics(const RhiStatics&) = delete;
    RhiStatics& operator=(const RhiStatics&) = delete;

    /* HOW BIG A CHUNK IS, IN TILES, PER STOREY.
     *
     * EIGHT IS A GUESS WITH A SHAPE RATHER THAN A MEASUREMENT, and the shape is
     * what matters: too large and the bounds stop rejecting anything, too small
     * and a storey becomes hundreds of draws of a few triangles each. Eight
     * tiles is roughly a building on this board, which is the grain at which
     * "can the camera see any of this" stops being trivially yes.
     *
     * A NAMED CONSTANT IN ONE PLACE, per §4.11's rule for anything that might
     * become a quality setting. If the answer ever needs measuring, this is the
     * number to sweep. */
    static constexpr int kChunkTiles = 8;

    /* Builds every chunk and registers it. Safe to call repeatedly — the same
     * contract StaticsMesh has: edit the data, call rebuild, and the world
     * reflects it. Releases what it held FIRST, including removing its
     * renderables from the scene it was last given. */
    void rebuild(const World& world, cromwell::RenderScene& scene);

    /* Removes every renderable and destroys every mesh. Called by rebuild and
     * by the destructor; separate because a world being torn down wants it
     * without a new one to build. */
    void release();

    int triangleCount() const { return triangleCount_; }

    /* HOW MANY RENDERABLES THE WORLD BECAME — which is what a draw call USED to
     * be here, and is not any more. What actually gets drawn is whatever the
     * frustum keeps, per view, per pass, and only the engine knows that. */
    int renderableCount() const { return renderableCount_; }

private:
    struct Chunk {
        cromwell::rhi::MeshHandle   mesh;
        cromwell::rhi::BufferHandle vertices;

        /* THE REGISTRATION, so it can be removed again. Held rather than
         * re-derived: a rebuild that could not name what it registered would
         * have to clear the whole scene, which would take the bodies and the
         * overlays with it. */
        cromwell::RenderableId      id;
    };

    cromwell::rhi::IRenderDevice& device_;

    /* THE SCENE THESE ARE REGISTERED IN, remembered so release() can take them
     * out again. A raw pointer because the scene outlives this object and this
     * object does not own it — and null whenever nothing is registered, which
     * is what makes release() safe to call twice. */
    cromwell::RenderScene* scene_ = nullptr;

    std::vector<Chunk> chunks_;

    int triangleCount_ = 0;
    int renderableCount_ = 0;
};

}  // namespace game
