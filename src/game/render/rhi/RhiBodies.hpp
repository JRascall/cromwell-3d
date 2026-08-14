/* RhiBodies.hpp — units, as device geometry.
 *
 * SINGLE RESPONSIBILITY: own one unit cube on the device, and submit the boxes
 * that make up every visible body under it.
 *
 * ================ WHY THIS EXISTS BESIDE UnitRenderer =====================
 *
 * The same bargain RhiStatics makes against StaticsMesh: UnitRenderer draws
 * bodies for the raylib renderer and is untouched, this one draws them for the
 * device, and one of the two is deleted at parity. Both derive their boxes from
 * the same `drawBody` recipe — the part offsets and sizes are copied, not
 * reinvented — so a soldier cannot end up a different shape in the two
 * renderers while both exist.
 *
 * ==================== ONE CUBE, MOVED, NOT A MESH PER BODY ================
 *
 * The world is baked because it never moves. A body moves every frame, so
 * baking it would mean re-uploading a vertex buffer per unit per frame — a
 * transfer measured in kilobytes to draw a box that is already on the card.
 *
 * So there is exactly ONE 1x1x1 cube here, and each part is a transform pushed
 * before a draw. Four draws for a vehicle, one for a soldier; a hundred bodies
 * is a few hundred draws with an eighty-byte constant each, which is nothing
 * against a pass that already draws the whole lattice.
 *
 * BoxMesh does this for the raylib path and is the direct ancestor. It cannot
 * be shared: it holds a raylib Mesh and calls DrawMesh, and the whole point of
 * the port is that neither exists below the platform layer.
 *
 * ================== NO CULLING, AND THAT IS DELIBERATE ====================
 *
 * Beyond the storey cut the cutaway already implies, nothing here rejects a
 * body the camera cannot see. A frustum test per unit would be the obvious
 * addition and is not worth writing yet: the counts are in the dozens, the
 * geometry is a box, and the cost of getting it wrong — a unit that pops out of
 * existence at the screen edge — is far larger than the cost of drawing it.
 * When bodies are real meshes in the thousands, this is where that goes.
 */
#pragma once

#include "cromwell/rhi/Handles.hpp"

namespace cromwell {
namespace rhi { class ICommandEncoder; class IRenderDevice; }
}  // namespace cromwell

namespace cromwell { class DeviceMaterials; }

namespace game {

class MoveAnimator;
class Unit;
class UnitRoster;
class World;
struct PathPoint;

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

    /* Every living body at or below `maxStorey`, as boxes.
     *
     * `animating` (may be null) is the unit walking a path: its logical cell and
     * its drawn position differ while the animation runs, so it is skipped in
     * the roster sweep and drawn separately at `animatedPosition`. Passing the
     * position in rather than the animator keeps this class ignorant of how a
     * move is interpolated, which is a question about the game's rules and not
     * about geometry. */
    /* `materials` BINDS SurfaceKind::Body ONCE, before anything is drawn, and
     * passing null is the depth-only case — the sun's pass has no material
     * block to bind, exactly as RhiStatics treats it.
     *
     * IT IS NOT OPTIONAL ON A SHADED PASS, and the reason is a real bug rather
     * than tidiness. Bodies are submitted AFTER the statics in the same pass,
     * and the statics bind a material per bucket — so a body that binds none
     * inherits whichever surface kind happened to be drawn last. The cutaway
     * decides which buckets are submitted, so the ISO LEVEL silently changed
     * what every soldier was made of: mirror-smooth at one storey cut and matte
     * at another, with nothing in the material system to explain it. */
    void submit(cromwell::rhi::ICommandEncoder& encoder, const UnitRoster& roster,
                const World& world, int maxStorey,
                const Unit* animating, float animatedX, float animatedHeight,
                float animatedY,
                const cromwell::DeviceMaterials* materials = nullptr) const;

    int drawCalls() const { return drawCalls_; }

private:
    /* One body's boxes, at a world position. The recipe — which parts, what
     * size, what colour — is UnitRenderer::drawBody's, kept in step with it by
     * hand for as long as both exist. */
    void submitBody(cromwell::rhi::ICommandEncoder& encoder, const Unit& unit,
                    float x, float baseHeight, float y) const;

    void submitPart(cromwell::rhi::ICommandEncoder& encoder,
                    float centreX, float centreY, float centreZ,
                    float sizeX, float sizeY, float sizeZ,
                    unsigned char r, unsigned char g, unsigned char b) const;

    void release();

    cromwell::rhi::IRenderDevice& device_;
    cromwell::rhi::MeshHandle     cube_;
    cromwell::rhi::BufferHandle   cubeVertices_;

    /* Counted per submission for the log line, so "the bodies are not drawing"
     * can be told apart from "the bodies are drawing somewhere I cannot see".
     * Mutable because submit() is const — it is a diagnostic, not state the
     * renderer reads. */
    mutable int drawCalls_ = 0;
};

}  // namespace game
