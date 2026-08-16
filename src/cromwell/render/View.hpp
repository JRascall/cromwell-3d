/* View.hpp — one eye looking at one scene, and where the picture lands.
 *
 * SINGLE RESPONSIBILITY: say WHICH scene is being drawn, FROM where, WHAT that
 * eye is allowed to see, and INTO what. Four questions, one object, no answers
 * about how anything is drawn.
 *
 * =========================== PUBLIC API — see Renderable.hpp ===============
 *
 * ==================== WHY THE VIEW AND NOT THE FRAME CARRIES THIS =========
 *
 * Because the requirement is N PLAYERS ON N SCREENS, AND EACH PANE MAY BE A
 * DIFFERENT WORLD RUNNING ITS OWN SIMULATION. Not merely a second camera on one
 * world: four players could be in four independent sims, or two, or all in one.
 * Single player is the same machinery at N = 1.
 *
 * The one-line consequence is that NOTHING RENDERER-SIDE MAY BE GLOBAL, and
 * this type is where that becomes concrete. Source's client leaf system is
 * `EXPOSE_SINGLE_INTERFACE_GLOBALVAR` — one instance, one world — and its
 * split-screen renders several views of that single world. Four players in four
 * DIFFERENT worlds cannot be expressed at all. So a view NAMES its scene rather
 * than assuming the only one. See study/topics/rendering/render_scene_architecture.md §5.
 *
 * ================ THE FILTER BELONGS TO THE VIEW, AND THAT IS THE POINT ====
 *
 * This is what makes an entire bug class impossible rather than merely fixed.
 *
 * Today the game gets it right by remembering to: RhiFrameRenderer::submit
 * passes `CutawayView::whole()` for the shadow and probe passes and the
 * player's cut otherwise. Getting that wrong made the lighting change when the
 * player changed floor — the sun's depth pass inherited the camera's storey cut,
 * the roof disappeared from it, and the room below jumped to full sunlight.
 * CutawayView.hpp documents the whole episode.
 *
 * After this the game cannot express the wrong thing. The sun's view and a
 * probe capture's view are constructed by the ENGINE and hide nothing; only the
 * camera's view carries a cutaway, because only the camera's view has one. The
 * default is "hide nothing", so the safe answer is the one you get for free.
 *
 * ======================== THE TARGET IS PART OF THE VIEW ==================
 *
 * §4.12's first draft omitted it and that was an oversight rather than a
 * simplification: the game already renders views into textures today. A camera
 * owns a resolved texture, the minimap is the whole board from above into its
 * own target, and every split-screen pane is one. A view with no target draws
 * to the backbuffer.
 *
 * WHAT `target` MEANS IS THE FINISHED PICTURE, NOT THE PIPELINE'S WORKINGS.
 * The scene colour buffer, the depth and normal planes, the shadow map, the
 * occlusion plane and the probe array are the engine's, and their formats and
 * sizes are what a quality preset moves — §4.11 depends on nobody outside
 * having pinned one. This names where the RESOLVE lands. Minimaps, security
 * cameras, portals, mirrors and UI previews are all that, and they are firmly
 * in the "extend freely" tier.
 *
 * THE VIEWPORT IS SEPARATE FROM THE TARGET, which is what lets four panes be
 * either four targets or four viewports into one. §4.12 ends on exactly that
 * open question — whether four players means four ScenePipelines or one reused
 * four times — and notes it does not have to be decided to start. Carrying both
 * fields is what keeps it undecided: neither answer needs this type to change.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/render/Renderable.hpp"
#include "cromwell/rhi/Handles.hpp"

namespace cromwell {

class RenderScene;

/* WHAT THIS EYE IS FOR. Four, and the list is closed because each one is a
 * different answer to "which renderables are relevant", not a different camera.
 *
 * THE GAME ONLY EVER BUILDS A Camera. The other three are constructed inside the
 * pipeline — the sun's fit is derived from the camera's frustum and the probe
 * faces come off the capture schedule — and they appear in this public enum
 * because a type that describes only two thirds of what exists is a type that
 * gets extended by a licensee who cannot see the rest. */
enum class ViewKind {
    /* A player's eye, or a minimap, or a security camera. Sees everything its
     * filters allow. */
    Camera,

    /* The sun's depth pass. Collects only what CASTS a shadow, which the
     * renderable says of itself — the view does not know what glass is. */
    Sun,

    /* One cube face of one reflection probe. Collects only what a capture
     * should contain: the world, and not one player's interface. */
    ProbeFace,

    /* THE CUSTOM DEPTH / STENCIL PASS. Collects only what carries a non-zero
     * stencil value, which the renderable says of itself — Unreal's
     * bRenderCustomDepth, and the same "asked of the renderable rather than of
     * the pass" rule the two above follow.
     *
     * ITS EYE IS THE CAMERA'S, not a derived one. The whole value of the buffer
     * is that its depth is comparable with the frame's, so a consumer can ask
     * "is this object behind what is on screen" — from any other viewpoint that
     * question has no answer. */
    CustomDepth,
};

class View {
public:
    /* ---- which scene, and what for --------------------------------------
     *
     * A REFERENCE-SHAPED SETTER RATHER THAN A CONSTRUCTOR ARGUMENT, so a view
     * can be built in one chained expression like everything else in this
     * engine. A view with no scene draws nothing and says so once; it is not a
     * crash, because "the world has not been built yet" is an ordinary state on
     * the first frames of a level load. */
    View& withScene(RenderScene& scene)
    {
        scene_ = &scene;
        return *this;
    }

    View& withKind(ViewKind kind)
    {
        kind_ = kind;
        return *this;
    }

    /* ---- where the eye is -----------------------------------------------
     *
     * THE TWO MATRICES AND THE POSITION TOGETHER. The position is derivable by
     * inverting the view matrix, and it is carried anyway for the same reason
     * SceneFrame already carries it: the lit pass needs it every fragment for
     * the view vector, and the translucent sort needs it per renderable per
     * frame. Inverting a matrix to recover a number the caller already had is
     * work done to lose information.
     *
     * ONE CALL, because a projection updated without its view — or a position
     * left over from last frame — is a frustum that culls against one eye while
     * the picture is drawn from another. That failure looks like geometry
     * popping in and out at the screen edge as the camera turns, which reads as
     * a culling bug in the engine rather than as two eyes in the caller. */
    View& withEye(const Mat4& view, const Mat4& projection, Vec3 position)
    {
        view_ = view;
        projection_ = projection;
        position_ = position;
        viewProjection_ = projection * view;
        return *this;
    }

    /* ---- what it may see — see Renderable.hpp on the two senses ---------
     *
     * WHICH OF THE GAME'S CATEGORIES THIS VIEW REFUSES TO DRAW. Zero — the
     * default — hides nothing. The engine never learns what a bit means. */
    View& withHiddenFlags(FilterFlags hidden)
    {
        hiddenFlags_ = hidden;
        return *this;
    }

    /* ---- AND THE ONES THAT SURVIVE `derived` ----------------------------
     *
     * HIDDEN HERE **AND IN EVERY VIEW THE ENGINE DERIVES FROM THIS ONE** — the
     * sun's depth pass, the transmission plane, every probe face.
     *
     * TWO WORDS BECAUSE THERE ARE GENUINELY TWO KINDS OF HIDING, and collapsing
     * them is a bug in one direction or the other:
     *
     *   - A CUTAWAY IS ABOUT THE EYE. What a player can see past is a fact
     *     about where they are standing, and it must NOT reach the sun —
     *     letting it do so is what made the lighting change when the player
     *     changed floor, which CutawayView.hpp documents at length. That is
     *     `withHiddenFlags`, and `derived` drops it.
     *   - A CATEGORY SWITCHED OFF IS ABOUT THE WORLD. "Do not draw the units"
     *     has to mean the units are not there at all, including in the shadow
     *     map — a unit hidden from the camera that still lays a shadow across
     *     the floor is a WORSE debugging tool than no switch, because it
     *     answers "not the units" when the units are still in the picture.
     *     ViewLayers.hpp states that requirement; this is what satisfies it.
     *
     * THE ENGINE STILL LEARNS NOTHING ABOUT WHAT A BIT MEANS. Both words are
     * the game's vocabulary; what differs is SCOPE, which is a property of
     * views and therefore the engine's to know. A game that wants its cutaway
     * everywhere and its layers per-eye can say exactly that.
     *
     * WHY NOT A FLAG ON THE RENDERABLE INSTEAD ("this is a unit, never shadow
     * it"): because the switch is a VIEW's opinion that changes at runtime, and
     * baking it into every renderable would mean walking the scene to flip a
     * checkbox. `castsShadow` is the renderable-side answer and it is a
     * different question — a permanent property of the surface. */
    View& withAlwaysHiddenFlags(FilterFlags hidden)
    {
        alwaysHiddenFlags_ = hidden;
        return *this;
    }

    /* WHOSE EYE THIS IS. A renderable is drawn when its `viewers` intersects
     * this. The default is every viewer, which is what a single-player camera,
     * the sun and a probe capture all want; a split-screen pane sets its own
     * bit so that the other player's overlays stay out of it. */
    View& withViewerMask(ViewerMask viewers)
    {
        viewerMask_ = viewers;
        return *this;
    }

    /* THE COMMON CASE OF THE ABOVE: this pane belongs to player N. Separate
     * because `withViewerMask(viewerBit(1))` is the kind of correct line that
     * gets written as `withViewerMask(1)` — which is viewer ZERO's bit, draws
     * the wrong player's overlays, and compiles. */
    View& withViewer(int viewer)
    {
        viewerMask_ = viewerBit(viewer);
        return *this;
    }

    /* ---- where the picture lands ----------------------------------------
     *
     * An invalid handle — the default — means the backbuffer. */
    View& withTarget(rhi::TextureHandle target)
    {
        target_ = target;
        return *this;
    }

    /* A RECTANGLE OF THE TARGET, in pixels. Zero width or height — the default
     * — means the whole thing. Four panes into one target is four views
     * differing only here. */
    View& withViewport(float x, float y, float width, float height)
    {
        viewportX_ = x;
        viewportY_ = y;
        viewportWidth_ = width;
        viewportHeight_ = height;
        return *this;
    }

    /* ---- what the pipeline reads --------------------------------------- */
    /* NON-CONST, AND EXACTLY ONE THING IS WRITTEN THROUGH IT: the reflection
     * probe set's capture schedule, which advances as faces are drawn and marks
     * a probe current once its chain is built. That is genuinely world state —
     * the cubemaps belong to the rooms — rather than a pass reaching into the
     * scene it is drawing. Everything else the pipeline touches is const.
     *
     * The alternative was a second, mutable handle on the scene passed beside
     * the view, which is two answers to "which world is this" and the shape of
     * bug this whole design exists to remove. */
    RenderScene* scene() const { return scene_; }
    ViewKind           kind() const { return kind_; }
    const Mat4&        viewMatrix() const { return view_; }
    const Mat4&        projectionMatrix() const { return projection_; }
    const Mat4&        viewProjection() const { return viewProjection_; }
    Vec3               position() const { return position_; }
    /* BOTH WORDS, UNIONED, because a collector asking "may this view draw it"
     * wants one answer. The split is about what `derived` carries and nothing
     * downstream of collection has any reason to know there were two. */
    FilterFlags        hiddenFlags() const { return hiddenFlags_ | alwaysHiddenFlags_; }
    FilterFlags        alwaysHiddenFlags() const { return alwaysHiddenFlags_; }
    ViewerMask         viewerMask() const { return viewerMask_; }
    rhi::TextureHandle target() const { return target_; }
    bool               hasViewport() const { return viewportWidth_ > 0.0f && viewportHeight_ > 0.0f; }
    float              viewportX() const { return viewportX_; }
    float              viewportY() const { return viewportY_; }
    float              viewportWidth() const { return viewportWidth_; }
    float              viewportHeight() const { return viewportHeight_; }

    /* ---- what the ENGINE derives from a camera view ---------------------
     *
     * A VIEW OF THE SAME SCENE FROM SOMEWHERE ELSE, keeping every filter. The
     * sun's depth pass and a probe face are built with this rather than from
     * scratch, and building them from scratch is the mistake it exists to
     * prevent: a derived view that forgot to carry `viewerMask` would put one
     * player's overlays into the shadow map of a four-player frame, and a
     * derived view that copied `hiddenFlags` would put the camera's cutaway
     * back into the sun's pass — which is the exact bug CutawayView.hpp
     * documents.
     *
     * SO IT CARRIES THE VIEWER AND DROPS THE CUTAWAY, and it is one function
     * rather than a rule. The kind gate does the rest: a Sun view collects only
     * casters and a ProbeFace only what belongs in a capture, both asked of the
     * renderable rather than of the pass.
     *
     * AND IT CARRIES `alwaysHiddenFlags`, which is the whole reason that field
     * exists — see it above. A category the game has switched off is switched
     * off in the shadow map too; a cutaway is not. Those are the two behaviours
     * and this line is where the difference is expressed. */
    View derived(ViewKind kind, const Mat4& view, const Mat4& projection, Vec3 position) const
    {
        View result;
        result.scene_ = scene_;
        result.viewerMask_ = viewerMask_;
        result.alwaysHiddenFlags_ = alwaysHiddenFlags_;
        result.withKind(kind).withEye(view, projection, position);
        return result;
    }

private:
    RenderScene* scene_ = nullptr;
    ViewKind           kind_ = ViewKind::Camera;

    Mat4 view_;
    Mat4 projection_;
    Mat4 viewProjection_;
    Vec3 position_;

    FilterFlags hiddenFlags_ = kNoFilterFlags;
    FilterFlags alwaysHiddenFlags_ = kNoFilterFlags;
    ViewerMask  viewerMask_ = kAllViewers;

    rhi::TextureHandle target_;
    float viewportX_ = 0.0f;
    float viewportY_ = 0.0f;
    float viewportWidth_ = 0.0f;
    float viewportHeight_ = 0.0f;
};

}  // namespace cromwell
