/* IGeometrySource.hpp — the seam between the engine's passes and the game's world.
 *
 * SINGLE RESPONSIBILITY: let ScenePipeline sequence a frame without knowing what
 * is in it. The engine opens the pass, binds the pipeline and sets the uniforms;
 * this puts geometry in it.
 *
 * ===================== WHY THIS AND NOT ISceneSource ======================
 *
 * ISceneSource is the same idea for the RAYLIB renderer, and its PassContext
 * carries raylib's Material and Camera3D because that is what raylib's passes
 * need to be handed. It cannot serve the device path, which binds a
 * PipelineHandle and a uniform buffer instead — and converting it in place would
 * break the renderer that currently ships.
 *
 * So there are two, for as long as there are two renderers, exactly as there are
 * two of everything else during this migration. ISceneSource is deleted with
 * FrameRenderer; this one remains.
 *
 * ======================= WHAT THE GAME DOES NOT DO ========================
 *
 * It does not open passes, bind pipelines, own targets, or decide the order
 * anything runs in. A submitter is handed an encoder with the pipeline already
 * bound and the pass's uniforms already uploaded, and draws. That is the whole
 * point: the sequence is engine work and is the same for every project, and the
 * meshes are the project's and are the same for none of them.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>

namespace cromwell {

namespace rhi { class ICommandEncoder; class IRenderDevice; }

/* ============ WHERE ONE DRAWN THING IS, AND WHAT COLOUR IT IS ============
 *
 * THE C++ HALF OF assets/shaders/rhi/object.glsl, and the two are one contract
 * written twice — there is no reflection on the explicit backends to check they
 * agree, so the check is that both files say so and the static_assert below
 * pins the size.
 *
 * IT LIVES HERE because it is the seam's payload, not the pipeline's: the
 * engine's shaders read it, the game's submitters write it, and IGeometrySource
 * is precisely the boundary those two meet at. A submitter that had to
 * hand-pack five vec4s in the right order would be a submitter that gets it
 * wrong once and produces geometry at the origin.
 *
 * PUSHED PER DRAW, and cheap enough to: it is eighty bytes into the eighty-byte
 * path every backend reserves for exactly this. Anything bigger, or anything
 * that does not change per draw, belongs in a uniform buffer instead. */
struct ObjectPush {
    /* Column-major, matching Mat4's own layout and GLSL's mat4-from-columns
     * constructor — so the sixteen floats cross unpermuted and nothing
     * transposes anywhere. */
    Mat4 model;

    /* MULTIPLIES the vertex colour. White leaves the mesh's own colours alone,
     * which is what the static world wants; a body's cube is white and takes
     * its whole colour from here. */
    float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    /* Named constructors, because the two callers are "this mesh is already in
     * world space" and "put this box here" and neither should be building a
     * matrix by hand at the call site. */
    static ObjectPush identity() { return ObjectPush{}; }

    static ObjectPush box(Vec3 centre, Vec3 size, float r, float g, float b)
    {
        ObjectPush push;

        /* SCALE THEN TRANSLATE. The other order scales the translation too, and
         * puts every body somewhere out along the ray from the origin through
         * where it should have been — which reads as "the units are in the
         * wrong place" rather than as a matrix order mistake. */
        push.model = Mat4::translation(centre) * Mat4::scaling(size);

        push.tint[0] = r;
        push.tint[1] = g;
        push.tint[2] = b;
        return push;
    }
};
static_assert(sizeof(ObjectPush) == 80, "rhi/object.glsl reads five vec4s");

/* WHICH PASS IS ASKING. The submitter branches on this where the answer
 * genuinely differs — the sun's pass wants casters only and the whole world,
 * a camera pass wants what that camera can see. */
enum class GeometryPass {
    /* The sun's depth pass. Casters only, and the WHOLE world: what casts a
     * shadow is a question about the world, not about where the player is
     * standing, and letting the camera's cutaway reach it makes the lighting
     * change when the player changes floor. */
    Shadow,

    /* The camera's depth prepass — everything OPAQUE, and glass is not.
     *
     * IT USED TO INCLUDE GLASS, on the reasoning that a window still occludes
     * as far as a depth test is concerned. That is true of a depth test taken
     * on its own and false of this pipeline: the lit pass tests Equal against
     * what this wrote, so a pane's depth here means the WALL BEHIND IT fails
     * that test and is never drawn. The result is glass over the background
     * instead of glass over a room — and no amount of blending in the
     * transparent pass can put back geometry that was never shaded. */
    Prepass,

    /* The shaded pass. Opaque surfaces only, for the same reason. */
    Lit,

    /* Everything you can see through, drawn after the opaque scene because it
     * reads what is already in the colour buffer. A blended surface drawn
     * before the wall behind it has nothing to see through to and just looks
     * like a tinted solid. */
    Transparent,

    /* ---- a reflection probe's cube face ---------------------------------
     *
     * The same two buckets as the camera's, and they exist SEPARATELY for one
     * reason: a capture takes the WHOLE world, never the player's cutaway.
     *
     * That is the same rule the sun's pass has and it is worth stating twice,
     * because getting it wrong here produces a subtler picture than getting it
     * wrong there. The iso level hides storeys between the eye and the room
     * being looked into — it is a statement about what the player is allowed to
     * see, not about what the world is made of. A probe capturing under it
     * records the sky and the street where its own ceiling and the floor above
     * should be, so an indoor room's reflections brighten every time the player
     * changes storey, and the cause is nowhere near the symptom.
     *
     * A submitter that ignored the distinction and reused its camera branch
     * would draw a correct-looking board into every cubemap and be wrong only
     * in the reflections, which is the one place nobody looks first.
     *
     * WHAT IS THE SAME as the camera's: the meshes, the materials, and which
     * bucket a surface lands in. Blend mode is a material property either way. */
    ProbeOpaque,
    ProbeTransparent,
};

class IGeometrySource {
public:
    virtual ~IGeometrySource() = default;

    /* Draw whatever this pass wants. The encoder has its pipeline bound and its
     * uniform buffers filled; a submitter that binds either is fighting the
     * pipeline rather than using it. */
    virtual void submit(rhi::ICommandEncoder& encoder, GeometryPass pass) = 0;

    /* THE WORLD'S EXTENT, so the engine can frame the sun's orthographic box
     * without knowing what a lattice is. Called once per shadow pass, which is
     * cheap enough not to cache and cheap enough not to matter.
     *
     * A source with nothing in it returns an empty box and the shadow pass is
     * skipped rather than dividing by zero on the way to a NaN matrix. */
    virtual void worldBounds(Vec3& minimum, Vec3& maximum) const = 0;
};

}  // namespace cromwell
