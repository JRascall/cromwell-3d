#include "game/render/rhi/RhiFrameRenderer.hpp"

#include "cromwell/debug/DebugDraw.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/platform/IPlatform.hpp"
#include "cromwell/platform/ISurface.hpp"
#include "cromwell/post/ToneMapPass.hpp"
#include "game/lattice/Constants.hpp"
#include "game/light/RoomPartition.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/render/FrameView.hpp"
#include "game/render/Palette.hpp"
#include "game/render/scene/ProbePlacement.hpp"
#include "game/render/scene/UnitRenderer.hpp"
#include "game/render/ui/GameUi.hpp"
#include "game/render/ui/SplashOverlay.hpp"
#include "game/state/GameState.hpp"
#include "game/world/World.hpp"

namespace game {

namespace {

/* raylib's Vector3 to the engine's Vec3.
 *
 * ONE LINE, AT ONE BOUNDARY, and that is the point. SunLight still speaks
 * raylib because the whole raylib renderer reads it; SceneFrame cannot, because
 * it crosses into cromwell_base where raylib does not exist. Converting here
 * keeps the raylib-shaped half on the game's side of the line rather than
 * dragging the type into the engine's frame description — which is the same
 * bargain every other interface in the port makes. */
cromwell::Vec3 toVec3(Vector3 value)
{
    return cromwell::Vec3{ value.x, value.y, value.z };
}

}  // namespace

RhiFrameRenderer::RhiFrameRenderer(cromwell::IPlatform& platform,
                                   const cromwell::SunLight& sun,
                                   GameUi& ui)
    : platform_(platform), sun_(sun), ui_(ui), pipeline_(platform.device()),
      uiPainter_(platform.device()),
      statics_(platform.device()), bodies_(platform.device())
{
    /* NO MSDF SAMPLE. Every other section of the gallery is draw-list commands
     * and runs on any painter; that one is raylib geometry in the scene and
     * would be a raylib draw inside a device frame. See WidgetGallery's header
     * — it is off here and nowhere else. */
    uiGallery_.setWorldTextSample(false);
}

RhiFrameRenderer::~RhiFrameRenderer()
{
    /* HAND THE UI BACK BEFORE ANYTHING IS DESTROYED. GameUi outlives this
     * object and holds a bare pointer to the painter below it; leaving that
     * pointer in place would give the raylib path — or a shutdown frame — a
     * painter whose device has already gone. */
    ui_.setPainter(nullptr);
}

void RhiFrameRenderer::submit(cromwell::rhi::ICommandEncoder& encoder,
                              cromwell::GeometryPass pass)
{
    /* THE TWO RULES THE SUN'S PASS HAS, and the reason GeometryPass exists at
     * all rather than one undifferentiated "draw the world":
     *
     * THE WHOLE LATTICE, NOT THE PLAYER'S CUTAWAY. The iso level hides storeys
     * between the eye and the room being looked into. It says nothing about
     * what the world is made of, and the sun does not care what the camera was
     * asked to skip — letting it through is what made the lighting change when
     * the player pressed 1.
     *
     * CASTERS ONLY. A window transmits light rather than blocking it, so it
     * belongs in the transmission plane and not in the depth one. */
    if (pass == cromwell::GeometryPass::Shadow) {
        statics_.submit(encoder, CutawayView::whole(), /*castersOnly=*/true);

        /* BODIES CAST, and the sun does not care about the cutaway here either
         * — a soldier on the floor above still throws a shadow into the street.
         * The whole lattice's storey count is the limit, not the player's. */
        submitBodies(encoder, CutawayView::whole().maxStorey, nullptr);
        return;
    }

    /* A REFLECTION PROBE'S CUBE FACE, and it takes the WHOLE lattice for the
     * same reason the sun's pass does — with a sharper consequence.
     *
     * The iso level is a statement about what the PLAYER is allowed to see, not
     * about what the world is made of. A probe capturing under it records the
     * sky and the street where its own ceiling and the floor above should be,
     * so every indoor room's reflections brighten when the player changes
     * storey — and the cause is nowhere near the symptom, because the frame
     * that changed is one nobody is looking at.
     *
     * CASTERS ARE NOT THE FILTER HERE, unlike the shadow pass: a capture is a
     * SHADED view, so it wants everything the camera would draw, glass
     * included. Only the cutaway differs. */
    if (pass == cromwell::GeometryPass::ProbeOpaque ||
        pass == cromwell::GeometryPass::ProbeTransparent) {
        const bool probeTranslucent = pass == cromwell::GeometryPass::ProbeTransparent;

        statics_.submit(encoder, CutawayView::whole(), /*castersOnly=*/false,
                        &pipeline_.materials(), probeTranslucent);

        /* BODIES IN THE REFLECTIONS, at the whole lattice's storey count. A
         * soldier standing in a window's room belongs in what that window
         * reflects — and the staleness is the answer to the obvious objection:
         * a body that moved is wrong in the cubemap for the fraction of a
         * second it takes the sweep to come back round, at a resolution and a
         * roughness where nobody can tell which soldier it was. */
        if (!probeTranslucent) submitBodies(encoder, CutawayView::whole().maxStorey,
                                            &pipeline_.materials());
        return;
    }

    /* Every camera pass draws what the camera can see, and glass is solid to a
     * depth test.
     *
     * THE MATERIALS GO WITH THE CAMERA PASSES ONLY. The shadow map's shader
     * reads position and writes depth, so it has no material block to bind and
     * binding one would be work for a stage that cannot see it. */
    const bool translucent = pass == cromwell::GeometryPass::Transparent;

    statics_.submit(encoder, cutaway_, /*castersOnly=*/false, &pipeline_.materials(),
                    translucent);

    /* BODIES ARE OPAQUE. Nothing on this board is a translucent unit, and a
     * soldier submitted into the blended pass would be drawn twice — once
     * solid and once over itself. When a cloaked unit or a ghost preview
     * arrives it becomes a material question like every other. */
    if (!translucent) submitBodies(encoder, cutaway_.maxStorey, &pipeline_.materials());
}

void RhiFrameRenderer::submitBodies(cromwell::rhi::ICommandEncoder& encoder,
                                    int maxStorey,
                                    const cromwell::DeviceMaterials* materials) const
{
    if (roster_ == nullptr || world_ == nullptr) return;

    bodies_.submit(encoder, *roster_, *world_, maxStorey,
                   animating_, animatedX_, animatedHeight_, animatedY_, materials);
}

void RhiFrameRenderer::drawUi(const FrameView& view)
{
    /* BROUGHT UP ON FIRST USE, not in the constructor: it loads a shader off
     * disk, and a renderer that failed to find one should say so once rather
     * than every frame. */
    if (!uiReady_ && !uiFailed_) {
        if (uiPainter_.initialise()) {
            uiReady_ = true;
            ui_.setPainter(&uiPainter_);
        } else {
            uiFailed_ = true;
            LOGGER.error("rhi: the ui painter could not start - no HUD will be drawn");
        }
    }
    if (!uiReady_) return;

    int width = 0;
    int height = 0;
    platform_.surface().size(width, height);

    /* THE SURFACE, NOT THE SCENE TARGET. The UI draws onto the backbuffer at
     * one pixel per pixel — it is already the resolution a designer laid it out
     * in, and supersampling it would blur text for nothing. */
    uiPainter_.setSurfaceSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    /* THE GALLERY BRACKETS THE FRAME ITSELF, so it is one or the other rather
     * than both — GameUi::begin may be called once per frame. It is a
     * full-screen scrim over everything anyway, so there is nothing underneath
     * it worth drawing. */
    if (uiGallery_.visible()) {
        if (view.camera != nullptr) uiGallery_.draw(ui_, view.ui, *view.camera);
    } else {
        /* ONE SCREEN AT A TIME — the front end and the in-game HUD are
         * alternatives, never both, which is the contract GameUi::begin
         * states. */
        cromwell::ui::UiContext& context = ui_.begin(view.ui);

        if (view.uiState == UIState::SplashScreen)
            drawSplashOverlay(context, view.splashSeconds, view.splashProgress);

        ui_.end();
    }

    static bool reported = false;
    if (!reported && uiPainter_.skippedCommands() > 0) {
        reported = true;

        /* SAID ONCE, because it is a statement about what is CONVERTED rather
         * than about this frame — backdrop blur is not, and a HUD missing its
         * frosting should say why rather than look broken. A text run counts
         * here too when no weight could be rasterised at all. */
        LOGGER.warn("rhi: {} ui command(s) skipped - backdrop blur is not converted "
                    "yet, and text needs a font that rasterised",
                    uiPainter_.skippedCommands());
    }
}

/* NO authorMaterials() ANY MORE, and its absence is the point.
 *
 * It used to sit here and say `setFactors(Ladder, 0.35f, 1.0f)` — a surface's
 * roughness and metalness, in C++, in the renderer. That meant a new kind of
 * glass, a puddle of water or a polished floor was a code change and a
 * compile, which is exactly the shape a material system exists to remove.
 *
 * Every material is now `assets/materials/<name>.mat`, loaded by
 * DeviceMaterials at startup — see MaterialDefinition. Blend mode is one of the
 * values in it, so whether a surface is even DRAWN in the transparent pass is
 * an authoring decision rather than a branch in this file. Water is a .mat.
 *
 * What is left on the game's side of the seam is which mesh goes in which
 * bucket, which is geometry rather than material.
 */
void RhiFrameRenderer::worldBounds(cromwell::Vec3& minimum, cromwell::Vec3& maximum) const
{
    minimum = boundsMinimum_;
    maximum = boundsMaximum_;
}

void RhiFrameRenderer::render(const FrameView& view)
{
    CW_PROFILE_ZONE_N("rhi render");

    if (view.state == nullptr) return;

    if (!ready_ && !failed_) {
        if (pipeline_.initialise()) {
            ready_ = true;
        } else {
            failed_ = true;
            LOGGER.error("rhi: the scene pipeline could not start - nothing will be drawn");
        }
    }
    if (!ready_) return;

    if (!staticsBuilt_) {
        const World& world = view.state->world();
        statics_.rebuild(world);
        staticsBuilt_ = true;

        /* THE CUBE, ONCE. It never changes, so it is built beside the world
         * rather than in initialise() — this is simply the first frame that
         * knows there is anything to draw. A failure here is logged and not
         * fatal: a lit board with no bodies on it is still worth looking at,
         * and it says exactly what went wrong. */
        if (!bodies_.build())
            LOGGER.error("rhi: the unit cube could not be built - no bodies will draw");

        /* A TILE OF MARGIN, the same the raylib path uses: geometry sits inside
         * the grid but a low sun throws its shadow past the edge, and clipping
         * there would cut those shadows off in mid air. */
        constexpr float kMargin = 1.0f;
        const Lattice& lattice = world.lattice();
        boundsMinimum_ = cromwell::Vec3{ -kMargin, -kMargin, -kMargin };
        boundsMaximum_ = cromwell::Vec3{
            static_cast<float>(lattice.width()) + kMargin,
            static_cast<float>(lattice.storeys()) * kStoreyHeight + kMargin,
            static_cast<float>(lattice.height()) + kMargin,
        };

        /* ONE PROBE PER ROOM, PLACED THE MOMENT THERE IS A WORLD TO FLOOD.
         *
         * HERE RATHER THAN IN THE CONSTRUCTOR because a room is a flood fill
         * over a lattice, and there is no lattice until the first frame — the
         * same reason the static mesh is built here. The engine's set owns the
         * array and the schedule; which volumes exist is this game's answer and
         * ProbePlacement is where it lives.
         *
         * THE FLOOD IS FRESH RATHER THAN INCREMENTALLY PATCHED. It is one pass
         * over a few thousand cells with a queue — cheaper than reasoning about
         * which rooms a detonation could have merged, and correct by
         * construction where the incremental version would be correct by
         * argument.
         *
         * WHAT IS NOT WIRED YET: re-placement after destruction. The raylib
         * path sets probesDirty_ when the world is edited and re-floods on the
         * next frame; here the world is built once and never rebuilt, so there
         * is nothing to hook that to. It belongs with whatever tells this
         * renderer the statics have changed, and inventing a second answer to
         * "has the world changed" before that exists is how the two end up
         * disagreeing. */
        const RoomPartition rooms(world);
        placeProbes(pipeline_.probes(), rooms, lattice, boundsMinimum_, boundsMaximum_);

        LOGGER.info("rhi: static world built - {} triangles in {} draws",
                    statics_.triangleCount(), statics_.drawCalls());
    }

    cutaway_ = view.cutaway;

    /* ---- what the pipeline's callback will need -------------------------
     *
     * Latched here because submit() is called from inside ScenePipeline and is
     * handed an encoder and a pass, nothing more. See the fields' note. */
    roster_ = &view.state->roster();
    world_  = &view.state->world();

    /* THE WALKING BODY. `animator` is a pointer on FrameView and may be null in
     * a harness that has no animation to play, so both halves of the condition
     * are real rather than defensive. */
    animating_ = nullptr;
    if (view.animator != nullptr && view.animator->isRunning() && view.preview != nullptr) {
        animating_ = &view.state->selectedUnit();

        const PathPoint position = view.animator->positionOn(*view.preview);

        /* THE SAME HALF-TILE NUDGE FrameRenderer::submitBodies applies, and for
         * the same reason: a path point is a CELL CORNER, while a 2x2 hull is
         * drawn about the centre of its footprint. A 1x1 body needs no nudge
         * because its centre offset already is half a tile. */
        const float offset = UnitRenderer::centreOffset(*animating_);
        const float nudge  = offset > 0.5f ? 0.5f : 0.0f;

        animatedX_      = position.x + nudge;
        animatedHeight_ = position.height;
        animatedY_      = position.y + nudge;
    }

    cromwell::SceneFrame frame;

    /* THE REAL SUN, the same object the dev panel's sliders and the sun-nudge
     * keys write to — so moving it moves both renderers' shadows and both
     * renderers' shading together, and an A/B between them is comparing two
     * pictures of one world rather than two worlds.
     *
     * ALL SIX NUMBERS, not just the direction. The colours are what a time of
     * day actually is: a low sun is warm and dim because its light has crossed
     * more atmosphere, while the sky above it stays cool. Taking the direction
     * and leaving the colour white would move the shadows correctly and keep
     * the scene lit at noon forever, which is the version of this that looks
     * almost right and is the harder one to notice. */
    frame.sunDirection    = toVec3(sun_.travelDirection());
    frame.sunRadiance     = toVec3(sun_.radiance());
    frame.skyZenith       = toVec3(sun_.zenithColour());
    frame.skyHorizon      = toVec3(sun_.horizonColour());
    frame.skyGround       = toVec3(sun_.groundColour());
    frame.ambientIntensity = sun_.ambientIntensity();

    /* HOW SOFT EVERY SHADOW IS, and it has to be the SAME number the bake uses
     * or the two paths disagree about the same sun — which is why it is a dial
     * on SunLight rather than a constant in either renderer. */
    frame.sunAngularRadius = sun_.angularRadius();

    /* The resolve's, and the same default the raylib path's ToneMapPass uses —
     * named from it rather than repeated as a number, because the two curves
     * are the same file and an exposure that drifted between them would show up
     * as one renderer simply being darker. */
    frame.exposure = cromwell::ToneMapPass::kDefaultExposure;

    /* THE DIAGNOSTIC VIEW THE PLAYER CYCLED WITH F, so one key drives both
     * renderers. `settings` is borrowed and live — see FrameView. */
    if (view.settings != nullptr) frame.debugView = view.settings->debugView;

    if (view.camera != nullptr) {
        /* THE DEV PANEL'S REFLECTIONS SWITCH, which lives on the camera's own
         * layers because it is a property of what THIS view draws. It turns off
         * both halves — the captures and the sampling — because a switch that
         * left the captures running would cost the same and answer nothing. See
         * ViewLayers on why a feature toggle that cannot answer "is this the
         * reflections?" is worse than no toggle at all. */
        frame.reflections = view.camera->layers().features.reflections;

        int width = 0;
        int height = 0;
        platform_.surface().size(width, height);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                                        : 1.0f;

        frame.view           = view.camera->viewMatrix();
        frame.projection     = view.camera->projectionMatrix(aspect, 0.1f, 1000.0f);
        frame.cameraPosition = view.camera->position();


        /* The prepass and everything unprojected from it are sized to the

         * surface. Returns immediately when nothing changed. */

        pipeline_.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    frame.clearColour[0] = palette::kBackground.r / 255.0f;
    frame.clearColour[1] = palette::kBackground.g / 255.0f;
    frame.clearColour[2] = palette::kBackground.b / 255.0f;
    frame.clearColour[3] = 1.0f;

    /* THE PROCESS-WIDE DEBUG QUEUE. Borrowed, not consumed — Application ages it
     * once at the top of the frame, so handing it to a second renderer or a
     * second pass would not eat it. Gated by the same view layer the raylib
     * path uses, so one switch hides debug geometry on both. */
    if (view.camera != nullptr && view.camera->layers().features.debugDraw)
        frame.debug = &cromwell::DebugDraw::get();

    pipeline_.render(frame, *this);

    /* AFTER THE RESOLVE, and that is the whole reason it is a separate call
     * rather than a pass in the pipeline: everything before the tone map is
     * linear radiance, and the UI is display colour a designer picked. */
    drawUi(view);

    /* THE BODY COUNT, ONCE. It cannot be logged beside the static world's,
     * which is what the shape of this asks for: nothing is submitted until the
     * pipeline calls back, so the number does not exist until a frame has run.
     *
     * WORTH THE ONE LINE because zero and "drawn somewhere I cannot see" look
     * identical on screen and have completely different causes — an empty
     * roster, a storey cut, a transform putting every unit at the origin
     * underneath the floor. One of those is answered here and the other three
     * are not, which is exactly why it is the first thing to check. */
    if (!bodiesReported_) {
        bodiesReported_ = true;
        LOGGER.info("rhi: bodies - {} boxes drawn on the first frame", bodies_.drawCalls());
    }

    /* SUBMIT AND SWAP. Only this renderer calls it — the raylib path's
     * EndDrawing already swaps, and doing both would present twice. */
    platform_.endFrame();
}

}  // namespace game
