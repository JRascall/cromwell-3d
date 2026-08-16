/* RhiBodies.hpp — units, as renderables the engine owns.
 *
 * SINGLE RESPONSIBILITY: own one unit cube on the device, and keep one
 * renderable per body part registered in a RenderScene and up to date. It draws
 * nothing.
 *
 * ================ IT USED TO SUBMIT, AND NOW IT SYNCHRONISES ==============
 *
 * The same conversion RhiStatics went through, with one difference that
 * matters: the static world is registered ONCE and never touched again, while a
 * body moves. So this is not "register and forget" — it is a reconciliation
 * that runs every frame and writes a transform per part.
 *
 * WHICH IS STILL FAR LESS WORK THAN SUBMITTING WAS. Submitting meant walking
 * the roster once per PASS — the shadow map, the prepass, the lit pass, the
 * probe capture — building the same matrices four or five times a frame and
 * throwing them away. This walks it once and writes only what changed. The
 * engine then culls the result per view, which the old shape could not do at
 * all.
 *
 * ================ WHY THIS EXISTS BESIDE UnitRenderer =====================
 *
 * The same bargain RhiStatics makes against StaticsMesh: UnitRenderer draws
 * bodies for the raylib renderer and is untouched, this one feeds the device,
 * and one of the two is deleted at parity. Both derive their boxes from the
 * same `drawBody` recipe — the part offsets and sizes are copied, not
 * reinvented — so a soldier cannot end up a different shape in the two
 * renderers while both exist.
 *
 * ==================== ONE CUBE, MOVED, NOT A MESH PER BODY ================
 *
 * The world is baked because it never moves. A body moves every frame, so
 * baking it would mean re-uploading a vertex buffer per unit per frame — a
 * transfer measured in kilobytes to draw a box that is already on the card.
 *
 * So there is exactly ONE 1x1x1 cube here and every part is a renderable
 * referencing it with its own transform. That the cube is shared by every body
 * is also what makes instancing possible later: the engine now knows two
 * renderables carry the same mesh, which is precisely the fact
 * `IGeometrySource` destroyed by handing over finished draws.
 *
 * THE CUBE IS OWNED HERE AND NOT BY THE ASSET LAYER, for now. It is a shared
 * mesh and RenderAssets is where a shared mesh belongs — but it is shared
 * between the BODIES of one world, not between worlds, and moving it before
 * there is a second world would be inventing an asset layer's API against one
 * caller. The rule is written down in RenderScene.hpp; this is the case it does
 * not yet apply to.
 *
 * ================== NO CULLING HERE, AND NOW THAT IS FREE =================
 *
 * This class rejects nothing beyond what the cutaway's filter bits already say.
 * It used to be a deliberate omission with a cost attached; now the engine
 * frustum-culls every renderable per view, so bodies get culling they never had
 * without a line of code here. That is the change in one sentence.
 */
#pragma once

#include "cromwell/render/Renderable.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <array>
#include <vector>

namespace cromwell {
class RenderScene;
namespace rhi { class IRenderDevice; }
}  // namespace cromwell

namespace game {

class Unit;
class UnitRoster;
class World;

class RhiBodies {
public:
    explicit RhiBodies(cromwell::rhi::IRenderDevice& device);
    ~RhiBodies();

    RhiBodies(const RhiBodies&) = delete;
    RhiBodies& operator=(const RhiBodies&) = delete;

    /* Uploads the cube. Call once, after the device exists; false means bodies
     * cannot be drawn and the caller should say so rather than silently render
     * an empty battlefield. */
    bool build();

    /* ONE PASS OVER THE ROSTER, WRITING WHAT MOVED.
     *
     * `animating` (may be null) is the unit walking a path: its logical cell and
     * its drawn position differ while the animation runs, so it is placed at
     * `animated*` rather than at its cell. Passing the position in rather than
     * the animator keeps this class ignorant of how a move is interpolated,
     * which is a question about the game's rules and not about geometry.
     *
     * A DEAD UNIT IS HIDDEN RATHER THAN REMOVED. The roster keeps its entry —
     * `isDead()` is a state, not a deletion — so removing the renderable would
     * mean re-registering it if anything ever revives or replays. Visibility is
     * one bool on a record; a remove recycles a slot and invalidates an id. */
    void sync(cromwell::RenderScene& scene, const UnitRoster& roster, const World& world,
              const Unit* animating, float animatedX, float animatedHeight, float animatedY);

    /* Removes every renderable and destroys the cube. Called by the destructor;
     * separate so a world teardown can do it without one. */
    void release();

    int renderableCount() const { return renderableCount_; }

private:
    /* THE MOST PARTS ANY BODY HAS: a vehicle's hull, turret and barrel. Named so
     * that adding a fourth is a compile error in the one place that fills the
     * array rather than a silent truncation at the call site. */
    static constexpr int kMaxParts = 3;

    /* ONE PART OF ONE BODY, AS THE ENGINE SEES IT. The offsets and sizes are
     * UnitRenderer::drawBody's, verbatim. */
    struct Part {
        cromwell::Vec3 offset;   /* from the body's base, in world units */
        cromwell::Vec3 size;
        cromwell::Vec4 tint = cromwell::Vec4::one();
    };

    /* THE PARTS ONE UNIT IS MADE OF. Returns how many were written. */
    static int partsOf(const Unit& unit, std::array<Part, kMaxParts>& out);

    struct Body {
        /* WHICH UNIT THIS IS, so a roster that changed shape can be noticed
         * rather than assumed. Compared by ADDRESS, which is sound because the
         * roster owns its units by unique_ptr and does not move them — and
         * which is checked rather than trusted, because a stale pointer here
         * would move the wrong soldier. */
        const Unit* unit = nullptr;

        std::array<cromwell::RenderableId, kMaxParts> parts{};
        int partCount = 0;
    };

    /* Tears down every registration and builds one per part of every unit in
     * the roster. Runs on the first sync and whenever the roster's shape
     * changes; the ordinary frame does none of it. */
    void reregister(cromwell::RenderScene& scene, const UnitRoster& roster);

    void removeAll();

    cromwell::rhi::IRenderDevice& device_;
    cromwell::rhi::MeshHandle     cube_;
    cromwell::rhi::BufferHandle   cubeVertices_;

    /* THE SCENE THE BODIES ARE IN, remembered so they can be taken out again —
     * the same arrangement RhiStatics has, and for the same reason: a mesh may
     * not be destroyed while a renderable still names it. */
    cromwell::RenderScene* scene_ = nullptr;

    std::vector<Body> bodies_;
    int               renderableCount_ = 0;
};

}  // namespace game
