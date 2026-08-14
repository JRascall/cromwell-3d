/* ISceneSource.hpp — the seam between the engine's passes and the game's world.
 *
 * SINGLE RESPONSIBILITY: let cromwell sequence a frame without knowing what is
 * in it. The engine opens the target, sets the state and names the pass; this
 * is what puts geometry in it.
 *
 * =========================== WHAT THIS BUYS ===============================
 *
 * cromwell is meant to be lifted into an RTS, an FPS and a third-person game.
 * Before this interface, the frame's ORDER — shadow, prepass, decals,
 * occlusion, probes, lit, resolve, display — lived in the game's FrameRenderer
 * alongside `statics_`, `units_` and the movement ribbons, so a second project
 * inherited the sequence only by copying the file that also knew what a soldier
 * was. The sequence is engine work and the soldiers are not; this is the line
 * between them, and it is exactly the line ViewLayers.hpp already draws for
 * switches ("if cromwell owns the pass it is a feature, if the game owns it it
 * is a draw layer").
 *
 * ======================= THREE METHODS, NOT ONE PER PASS ==================
 *
 * There are five passes (PassContext.hpp) and three methods, because passes
 * group by HOW they shade rather than by what they are for:
 *
 *   submitDepth        one material over everything — shadow, G-buffer, ids
 *   submitLit          per-surface shading through the PBR shader
 *   submitTransparent  what has to come after the opaque set, blended
 *
 * A method per pass would be five near-identical overrides in every project,
 * and the sixth pass added later would find four of them updated. The pass's
 * identity still travels, on the context, for the two or three places a
 * submission genuinely wants to differ.
 *
 * ==================== WHY A VIRTUAL IS FREE HERE ==========================
 *
 * These are called a few dozen times a frame at most — once per pass per
 * camera. That is the same granularity CLAUDE.md puts profiler zones at, and
 * for the same reason: a virtual call costs a few nanoseconds, which is nothing
 * beside a render pass and ruinous inside a per-cell loop.
 *
 * So the rule this interface is written to, and the one any future engine
 * interface should follow: DISPATCH AT PASS AND SYSTEM GRANULARITY, NEVER
 * BELOW. An ISceneSource that handed the engine one virtual per drawable would
 * be the same mistake as a profiler zone inside a ray step.
 *
 * ========================= WHAT IT MUST NOT BECOME ========================
 *
 * A place the engine asks the game questions. Everything travels one way: the
 * engine describes a pass, the game draws into it, nothing is returned. The
 * moment this grows a `bool shouldDrawShadows()` the game is deciding the
 * sequence again and the split has been undone — that answer belongs on the
 * camera's ViewLayers, which is data the engine already reads.
 */
#pragma once

#include "cromwell/render/PassContext.hpp"

namespace cromwell {

class ISceneSource {
public:
    virtual ~ISceneSource() = default;

    /* ONE MATERIAL OVER EVERYTHING — the sun's depth pass, the G-buffer, and
     * the custom-depth target. `pass.material` is what to draw with and is
     * never null here.
     *
     * The three differ in what they want submitted, not in how: the shadow map
     * wants casters only (`pass.castersOnly`), the G-buffer wants each surface's
     * roughness pushed through `pass.prepass` first, and the custom-depth pass
     * wants movers tagged through `pass.objectIds` and typically skips static
     * world geometry — an object id exists to single a thing out, and the
     * lattice is not a thing. */
    virtual void submitDepth(const PassContext& pass) = 0;

    /* THE SHADED SUBMISSION — the lit scene and every reflection probe face.
     * `pass.material` is null: shading means picking per surface out of
     * `pass.materials` and feeding `pass.pbr`, and there is no single answer to
     * hand over.
     *
     * A PROBE CAPTURE COMES THROUGH HERE TOO, which is the point. Shading the
     * cubemaps with a second implementation is how a lighting switch comes to
     * half-work — turned off in the scene, still alive in every reflection. One
     * method, one answer, and `pass.worldSpace()` tells the game the probe is
     * asking about the world rather than about a camera. */
    virtual void submitLit(const PassContext& pass) = 0;

    /* WHAT MUST FOLLOW THE OPAQUE SET, blended against what is already there.
     *
     * THE BLEND STATE IS THE SUBMISSION'S, FOR NOW. It would read better as the
     * engine's — the premultiplied convention is a property of what PbrShader
     * writes, not of what any game draws — but bracketing the call from outside
     * means setting that state even on the frames the submission returns
     * early, and one of the callers is a reflection probe capture that
     * deliberately runs with colour blending off. It moves when the shader
     * does.
     *
     * Also the sun's glass, with `kind == Shadow` and the transmitter material:
     * light surviving a pane is the same "after the opaque set" idea, written
     * into a depth target's colour plane instead of a camera's.
     *
     * Optional, because a project may have nothing transparent. */
    virtual void submitTransparent(const PassContext& pass) { (void)pass; }
};

}  // namespace cromwell
