/* IScenePass.hpp — THE HATCH. A game's own pass, at a named point in the frame.
 *
 * SINGLE RESPONSIBILITY: let a game run its own rendering work at a point the
 * engine names, over resources the engine names, without letting it change what
 * those resources ARE or the order anything runs in.
 *
 * =========================== PUBLIC API — see Renderable.hpp ===============
 *
 * ===================== READ THIS BEFORE REACHING FOR IT ==================
 *
 * **THIS IS THE RARE CASE, NOT THE EXTENSION POINT.** rhi/MIGRATION.md §4.7's
 * direction is that MATERIALS are the main extensibility surface: once a
 * material can read sun direction, the shadow map, scene depth and time, almost
 * nothing a game wants needs a custom pass at all. A hologram, a cel-shaded
 * unit, a scrolling detail normal, water — those are surfaces, and a surface
 * authored as a `.mat` costs no C++, no engine knowledge and nothing on the
 * console port.
 *
 * Reach here for what is genuinely a PASS: a scanner sweep across the whole
 * frame, a bespoke composite, an outline built from scene depth. If what you
 * are reaching for is a SURFACE, you are in the wrong file.
 *
 * **AND HATCH USE IS LOGGED, DELIBERATELY.** ScenePipeline prints how many
 * custom passes are registered and where, at startup. Not to police it: **a
 * hatch used for something ordinary is a signal that the engine is missing a
 * feature**, and that signal is worthless if nobody can see it.
 *
 * ==================== WHAT IT GIVES YOU, AND WHAT IT DOES NOT =============
 *
 * **THE FRAME'S RESOURCES, NOT THE ENGINE'S CONSTRUCTION.** That is the line,
 * and everything below follows from it.
 *
 * YOU GET: the device, the view being drawn, the frame's matrices, and READ AND
 * WRITE access to the named targets that already exist — scene colour, scene
 * depth, the normal plane, the occlusion plane, the shadow map, the shadow
 * transmission plane.
 *
 * READ AND WRITE, not read-only, and the first draft of this design got that
 * wrong. Blitting over scene colour at a named point is how custom post, an
 * outline composite and decals-over-scene are all done, and Unity permits
 * exactly that on `_CameraOpaqueTexture`. What threatens portability is
 * changing WHICH targets exist, their formats and their sizes, and the ORDER
 * passes run in — not writing into one that already exists.
 *
 * YOU DO NOT GET: to add or remove a pipeline pass, to change a target's format
 * or size, or to reorder the frame. §4.11's quality presets depend on those
 * staying the engine's — a preset is a value only as long as nobody outside has
 * pinned a format.
 *
 * ============== WHY AN ENCODER IS NOT HANDED OVER, AND THIS MATTERS ========
 *
 * The obvious shape is to hand the pass a live `ICommandEncoder` inside a pass
 * the engine has already opened. It is rejected for two reasons and both are
 * load-bearing:
 *
 * 1. **AN OPEN PASS HAS ALREADY CHOSEN ITS ATTACHMENTS.** A custom composite
 *    that wants to write scene colour while READING it cannot do that inside
 *    one pass on any backend — it is undefined in GL, forbidden in Vulkan, and
 *    needs a different encoder in Metal. A hatch that appeared to allow it
 *    would be a hatch that works on the development machine and not on the
 *    console.
 * 2. **IT IS EXACTLY WHAT IS BEING REMOVED.** `IGeometrySource` put game code
 *    inside a render pass holding an encoder, and every cost §4.12 lists came
 *    from that. Rebuilding it here under a nicer name would be the same seam in
 *    a smaller hole.
 *
 * So a hatch pass OPENS ITS OWN PASSES through the device, targeting the named
 * resources it was given. That is more typing and it is honest typing: the copy
 * a read-and-write composite needs is visible in the caller's own code, where a
 * tiler's bill can be seen. The UI's backdrop blur already works exactly this
 * way and the reasoning is written out in §4.2.
 *
 * ============================ AND WHEN IT IS NOT ENOUGH ==================
 *
 * Say so rather than working around it. While cromwell's source is in the room
 * an inadequate hatch costs an afternoon editing the pipeline; once the engine
 * ships to studios as headers and a library, the only answers are a support
 * ticket and a bespoke build, and every case this does not cover becomes a fork
 * somebody maintains. That is why the missing-feature signal above is worth
 * more than the hatch itself.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/render/View.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>

namespace cromwell {

namespace rhi { class IRenderDevice; }

/* WHERE IN THE FRAME. Four, and each is a genuinely different state of the
 * frame rather than a convenient label — which is the test a fifth would have
 * to pass. The names describe WHAT HAS HAPPENED, not which engine method runs
 * next, so they survive the pipeline being reordered around them. */
enum class ScenePassPoint {
    /* DEPTH AND NORMALS EXIST; THERE IS NO COLOUR YET. The prepass has run and
     * the occlusion plane has not. This is where anything that writes into the
     * G-buffer belongs — decals are the canonical case, and the reason it is
     * before occlusion is that a decal's normal should affect the ambient
     * occlusion that follows it. */
    AfterDepthPrepass,

    /* THE OPAQUE SCENE IS SHADED. Scene colour holds the sky and every opaque
     * surface, scene depth is complete for them, and nothing translucent has
     * been drawn. Outline composites and anything that wants the world without
     * glass over it goes here. */
    AfterOpaque,

    /* EVERYTHING LINEAR IS FINISHED — translucent surfaces and debug geometry
     * included — and the tone map has not run. Custom post-processing belongs
     * here, because this is the last point at which the buffer holds RADIANCE.
     * A bloom, a heat haze or a colour grade written after the tone map is
     * working on display colour and will not match. */
    BeforeToneMap,

    /* DISPLAY COLOUR, ON THE FINAL TARGET. The resolve has run. Anything that
     * is deliberately not part of the lit world — a scanline effect, a
     * letterbox, a bespoke HUD element that has to be under the UI — goes here.
     * Note that the engine's own UI painter draws after this. */
    AfterToneMap,
};

/* THE FRAME'S NAMED RESOURCES. Handles, not descriptors: a pass may sample
 * these and may render into them, and may not ask what format or size they are
 * in order to make a decision that would break under a quality preset.
 *
 * WHY A STRUCT OF HANDLES AND NOT ACCESSORS ON THE PIPELINE. Because
 * `ScenePipeline` is internal and must stay so — publishing it would publish
 * every private member, every target and the pass order with it, and §4.12 is
 * explicit that every header published is supported forever. This is the
 * narrow, deliberately chosen slice of it that is safe to promise. */
struct SceneResources {
    rhi::TextureHandle sceneColour;

    /* THE DEPTH THE OPAQUE SCENE WROTE. Note the G-buffer's ALPHA IS ROUGHNESS
     * rather than coverage — anything sampling `sceneNormals` should know that
     * before reading the fourth channel. */
    rhi::TextureHandle sceneDepth;
    rhi::TextureHandle sceneNormals;

    /* THE BLURRED occlusion plane, which is what the lit pass samples. The raw
     * one is noise by construction and is deliberately not offered — looking at
     * it invites the conclusion that the occlusion pass is broken. */
    rhi::TextureHandle occlusion;

    rhi::TextureHandle shadowMap;
    rhi::TextureHandle shadowTransmission;

    /* THE SCENE TARGETS' SIZE IN PIXELS, which is the surface's times the
     * supersample factor. Given because a screen-space pass must address the
     * plane it is reading and not the window — getting this wrong puts every
     * sample at half the intended coordinate, which does not look like a
     * resolution mistake, it looks like the effect being noisy. That exact bug
     * is recorded in §5 against the occlusion pass. */
    std::uint32_t sceneWidth = 0;
    std::uint32_t sceneHeight = 0;

    /* AND THE FINAL TARGET'S, which is the surface's. Different from the above
     * whenever supersampling is on, which is always today. */
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
};

/* WHAT A HATCH PASS IS HANDED. Everything it may legitimately need and nothing
 * it may not. Borrowed for the duration of the call and valid for no longer:
 * holding on to the device is fine, holding on to the resources across frames
 * is not, because a resize rebuilds them. */
struct ScenePassContext {
    rhi::IRenderDevice* device = nullptr;

    /* WHICH EYE IS BEING DRAWN, including its matrices and — for a game that
     * cares — which viewer it belongs to. A pass registered once runs for every
     * view; a per-player effect reads this rather than assuming there is one
     * player, which is the same requirement the whole scene design exists for. */
    const View* view = nullptr;

    SceneResources resources;
};

class IScenePass {
public:
    virtual ~IScenePass() = default;

    /* FOR THE STARTUP LINE, so the log can say WHAT is registered rather than
     * only how many. A pass that cannot name itself is one nobody can attribute
     * a cost to. */
    virtual const char* name() const = 0;

    /* RUN. The pass opens its own passes on `context.device` and targets the
     * resources it was given — see the header on why no encoder is handed over.
     *
     * IT MAY BE CALLED SEVERAL TIMES PER FRAME, once per view. A pass that
     * accumulates state across calls without keying it on the view is a pass
     * that works in single player and doubles up in split-screen. */
    virtual void execute(const ScenePassContext& context) = 0;
};

}  // namespace cromwell
