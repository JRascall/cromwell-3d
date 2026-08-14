/* PassContext.hpp — everything a geometry submission needs, and nothing about
 * who is submitting it.
 *
 * SINGLE RESPONSIBILITY: describe ONE pass to whatever is going to fill it —
 * which pass this is, what it draws with, what it is allowed to draw, and the
 * shared shading state a correct submission has to feed.
 *
 * ======================= WHY THIS TYPE EXISTS AT ALL =======================
 *
 * The engine owns the render targets and the order the passes run in; the GAME
 * owns what geometry goes into them. That split needs exactly one thing to
 * cross the boundary — a description of the pass — and this is it. Without it
 * the alternatives are both bad: a submission function per pass (five near
 * copies, and the sixth pass someone adds gets four of them updated), or the
 * game reaching back into the renderer for the material and the layers, which
 * is the coupling the split is being made to remove.
 *
 * ===================== AN AGGREGATE, AND DELIBERATELY =====================
 *
 * This is a one-shot data carrier in the sense CLAUDE.md and HttpClient.hpp set
 * out: filled by one caller immediately before the call, read by one callee,
 * dead when the call returns. No invariant spans the fields, so no setter could
 * validate anything, and a fluent wrapper would be ceremony over a bag of
 * values. FrameView is the same decision for the same reason.
 *
 * The project's rule is private members behind accessors — see any component —
 * and this is a documented exception to it, not an oversight.
 *
 * ============== WHY THE SHADER POINTERS ARE HERE AND NOT FETCHED ===========
 *
 * `materials`, `pbr` and `prepass` are how a submission stays CORRECT rather
 * than merely visible. Drawing a surface means telling the shader what that
 * surface is — its roughness, its factors, its transmission — and the shader
 * belongs to the engine while the answer belongs to the game. Handing them over
 * on the context means the game never holds a renderer reference to get at
 * them, and a pass that has no shader to feed (the shadow map's depth-only
 * submission) simply carries nulls.
 *
 * They are cromwell types throughout, so nothing here reverses the arrow.
 *
 * ==================== THIS FILE STILL NAMES raylib. IT IS WRONG ============
 *
 * `Material` and `Camera3D` below are raylib's, and no interface in cromwell is
 * allowed to name a graphics API — see cromwell/rhi/IRenderDevice.hpp for the
 * rule and the reason (macOS has no GL compute, and console headers cannot be
 * committed here at all).
 *
 * It is left standing rather than cosmetically patched because a MaterialHandle
 * that is a raylib Material in disguise would be worse: it would read as
 * migrated, and it would still be single-backend. This file is FIRST in the
 * migration order — it converts the moment StaticsMesh, PropSet and
 * UnitRenderer draw through IRenderDevice, at which point `material` becomes a
 * PipelineHandle and `camera` becomes the engine's own view type.
 *
 * Do not add anything raylib-typed to it in the meantime.
 */
#pragma once

#include "raylib.h"

#include "cromwell/overlay/ViewLayers.hpp"

namespace cromwell {

class MaterialLibrary;
class PbrShader;
class PrepassShader;
class CustomDepthStencil;

/* WHICH PASS IS ASKING. Five, and that is not a coincidence — it is one per
 * place the frame submits world geometry, and a new value here means a new
 * pass rather than a new variation on an old one.
 *
 * THE GAME IS EXPECTED TO SWITCH ON THIS, in the two or three places where a
 * pass genuinely wants different geometry (the custom-depth target tags movers
 * and skips the static lattice, because an object id is for singling out a
 * thing that moves). It is NOT an invitation to write five separate
 * submissions behind one function — if a case shares nothing with its
 * neighbours, that is the signal the pass wanted its own method on ISceneSource,
 * not a branch in here. */
enum class PassKind {
    /* The sun's depth pass. Depth only, no camera — ShadowMap::Scope installs
     * the light's matrices itself. */
    Shadow,

    /* The G-buffer: depth and normals, plus roughness in alpha, for the
     * screen-space effects unprojected from it. */
    Prepass,

    /* Tagged objects into their own depth/stencil target, each with an id. */
    CustomDepth,

    /* The shaded pass, into a camera's HDR target. */
    Lit,

    /* A reflection probe's cubemap face. Shaded like Lit, but from the probe's
     * point of view and with no camera — capture() installs each face's
     * matrices, exactly as the shadow map does. */
    ProbeCapture,
};

struct PassContext {
    PassKind kind = PassKind::Lit;

    /* WHAT THIS CAMERA HAS SWITCHED ON, already resolved — screen-space
     * features have been gated against whether this view actually owns the
     * buffers they need, so a submission may trust the flags as they stand.
     * Never null. */
    const ViewLayers* layers = nullptr;

    /* THE RIG, or null when the pass installs its own matrices — the sun and a
     * probe face both do. Null is the honest answer there rather than a stale
     * camera nobody is looking through, and a submission that wants to cull or
     * pick an LOD has to handle it, which is correct: neither of those passes
     * has a viewpoint to cull against. */
    const Camera3D* camera = nullptr;

    /* WHAT TO DRAW WITH, for the depth-style passes that shade every surface
     * the same way. Null for Lit and ProbeCapture, where the game picks per
     * surface out of `materials` and there is no single answer. */
    const Material* material = nullptr;

    /* The target's size in pixels. Screen-space work is addressed against it. */
    float width = 0.0f;
    float height = 0.0f;

    /* Drop the surfaces that transmit light rather than block it — the sun's
     * opaque submission sets this, and its glass goes in afterwards through
     * submitTransparent with the transmitter material. */
    bool castersOnly = false;

    /* ---- the shared shading state a submission has to feed --------------- */
    MaterialLibrary* materials = nullptr;
    PbrShader*       pbr       = nullptr;   /* Lit and ProbeCapture only */
    PrepassShader*   prepass   = nullptr;   /* Prepass only              */

    /* WHERE A PER-OBJECT ID GOES, non-null only for CustomDepth. The
     * submission calls setStencil before each object it wants singled out; a
     * pass that is not tagging anything carries null and the game skips it. */
    const CustomDepthStencil* objectIds = nullptr;

    /* ================== WHY THIS IS DERIVED AND NOT A FIELD ==================
     *
     * WHETHER THIS PASS SEES THE WHOLE WORLD OR THE CAMERA'S VIEW OF IT.
     *
     * A field could disagree with `kind`, and that disagreement is precisely
     * the bug the game's CutawayView.hpp documents at length: the shadow map
     * used to read the player's iso level, so hiding a storey deleted the roof
     * from the sun's depth pass and the room below it jumped to full sunlight —
     * the lighting became a function of where the camera was cut. The probe
     * capture has the identical failure, one step further away.
     *
     * The rule is not per-call-site judgement, it is a property of the pass:
     * the sun and the probes ask a question ABOUT THE WORLD, everything else
     * asks a question about what one camera can see. Deriving it means a caller
     * cannot get it wrong and a new pass has to state which kind of question it
     * is asking, which is the decision that actually matters.
     *
     * The game maps this onto its own view type — cromwell has no opinion about
     * storeys or wall facings and must not acquire one. */
    bool worldSpace() const
    {
        return kind == PassKind::Shadow || kind == PassKind::ProbeCapture;
    }

    /* Reads better than `*pass.layers->drawing(...)` at the two dozen sites
     * that ask, and it is the form every submission uses. */
    bool drawing(DrawLayerId layer) const { return layers->drawing(layer); }
};

}  // namespace cromwell
