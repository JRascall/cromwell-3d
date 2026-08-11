#include "game/render/FrameRenderer.hpp"

#include "game/render/DrawLayers.hpp"

#include "raymath.h"
#include "rlgl.h"
#include "rlImGui.h"
#include "imgui.h"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"
#include "cromwell/gpu/GL.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "game/border/band/BandExtractor.hpp"
#include "game/lattice/Lattice.hpp"
#include "game/render/Palette.hpp"
#include "game/render/dev/DecalDemo.hpp"
#include "game/render/scene/ProbePlacement.hpp"
#include "game/light/RoomPartition.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/state/RingSelector.hpp"
#include "game/units/kinds/Unit.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace game {
namespace {

/* Custom depth stencil values. 0 stays reserved for "nothing was drawn", and
 * units are numbered from 1 upward so a consumer can outline one soldier, or
 * test a range for the whole squad. */
constexpr int kPropStencil      = 200;
constexpr int kFirstUnitStencil = 1;

std::string format(const char* text) { return text ? text : ""; }

}  // namespace

bool FrameRenderer::resizeSceneTarget(int windowWidth, int windowHeight)
{
    sceneWidth_  = windowWidth * ToneMapPass::kSupersampleFactor;
    sceneHeight_ = windowHeight * ToneMapPass::kSupersampleFactor;
    return scene_.create(sceneWidth_, sceneHeight_, true);
}

bool FrameRenderer::initialise(int width, int height, const CliOptions& options,
                               const GameState& state)
{
    statics_  = std::make_unique<StaticsMesh>();
    units_    = std::make_unique<UnitRenderer>(state.world());
    overlays_ = std::make_unique<OverlayRenderer>(state.world(), state.roster());

    ribbonShader_ = std::make_unique<RibbonShader>();
    if (!ribbonShader_->load()) return false;

    ribbonMeshes_   = std::make_unique<RibbonMeshSet>();
    ribbonRenderer_ = std::make_unique<RibbonRenderer>(*ribbonShader_, *ribbonMeshes_);

    glow_ = std::make_unique<GlowPass>();
    glow_->loadShader();
    glow_->resize(width, height);

    /* The main view's screen-space set — the same ScenePassBuffers a capture
     * owns, at the window's resolution. Only the depth prepass inside it is
     * hard-required; occlusion and decals degrade per part, exactly as they
     * do for a capture. */
    if (!mainBuffers_.create(width, height)) {
        std::fprintf(stderr, "FATAL: no depth prepass target - "
                             "screen-space passes cannot run\n");
        return false;
    }

    /* Custom depth/stencil. Window resolution rather than supersampled: its
     * consumers are screen-space effects and none of them wants subpixel
     * precision on an object id. */
    if (customDepth_.load()) customDepth_.resize(width, height);

    previews_.load();

    /* ---- lighting. The lit shader is the one hard requirement; everything
     * else here degrades rather than fails, because a missing shadow map or a
     * missing sky is a worse-looking frame and a missing surface shader is a
     * black one. */
    if (!pbr_.load()) return false;
    if (!materials_.load(pbr_.shader())) return false;
    if (!resizeSceneTarget(width, height)) {
        std::fprintf(stderr, "FATAL: no RGBA16F render target - "
                             "the linear pipeline cannot run\n");
        return false;
    }

    if (!tonemap_.load()) return false;
    sky_.load();
    overlayShader_.load();

    shadows_.load();
    depthMaterial_ = shadows_.valid() ? shadows_.casterMaterial() : LoadMaterialDefault();

    /* The two ends of the transmission plane, set from one material so they
     * cannot drift apart: the shadow pass encodes what survives a pane, the
     * lit pass decodes it. The uvScale has to be the window's own, or a streak
     * of grime would darken a patch of floor the streak is not over. */
    {
        const MaterialLibrary::Handle glass = materials_.handleOf(SurfaceKind::Window);
        const Vector2 remap = materials_.glassRemapOf(glass);
        const Vector4 transmission = materials_.glassTransmissionOf(glass);
        shadows_.setTransmitter(materials_.translucencyOf(SurfaceKind::Window),
                                Vector4{ remap.x, remap.y,
                                         materials_.factorsOf(glass).w,
                                         transmission.w });
        pbr_.setGlassTransmission(transmission);
    }

    /* SSAO and the decals both read what the PREPASS writes — normals to
     * orient the sampling hemisphere, depth to unproject onto a receiving
     * surface — so both passes are gated on its shader loading. The BUFFERS
     * live in mainBuffers_ and degrade per part on their own; what is gated
     * here is the passes that fill them. */
    if (prepass_.load()) {
        if (decalRenderer_.load()) {
            /* Scaffolding, and the only thing that puts a decal on the board
             * today — see DecalDemo.hpp. Inside the decalRenderer_ guard so a
             * run with no decal shader does not build textures nothing will
             * ever sample. Materials always, instances only on request — the
             * dev panel's decal tool has to have something to place even when
             * the scatter is off. */
            if (options.decalDemo) populateDemoDecals(decals_, state.world());
            else                    registerDemoMaterials(decals_);
        }
    }
    mainBuffers_.occlusion().setEnabled(options.ambientOcclusion);
    minimapRealtime_ = options.minimapRealtime;

    /* After the material library, because every model registers its own
     * materials there as it loads. */
    props_.loadManifest(materials_);

    /* ONE PROBE PER ROOM, not one per board. A single probe in the middle of
     * the lattice parallax-corrects every surface against the world's bounds,
     * which re-aims a wall's reflection ray from a point on the far side of
     * that wall and returns geometry the wall blocks — the wall reads as
     * transparent. See ReflectionProbeSet.hpp. */
    if (probes_.create()) rebuildEnvironmentProbes(state);
    probeSpheres_.load();

    /* 64 texels per tile face — about 2.3cm at XCOM's 96uu tile.
     *
     * THIS NUMBER IS THE WHOLE BALLGAME. A baked shadow can be no sharper than
     * its texels, and it is replacing a 4096 shadow map whose effective
     * density over this lattice is ~109 texels per tile. The first version
     * shipped 16 and looked markedly worse than what it replaced, which is a
     * bake's one unforgivable failure mode: it is supposed to be the
     * high-quality path. Anything that lowers this needs to be checked against
     * the shadow map, not against the previous bake. */
    lightBake_ = std::make_unique<SunBaker>(state.world(), 64, 8);
    const SunBakeStats bake = lightBake_->bakeAll(currentSun());
    TraceLog(LOG_INFO, "LIGHTMAP: baked %d patches (%d texels) in %.0f ms",
             bake.patches, bake.texels, bake.milliseconds);
    uploadLightmap();

    statics_->rebuild(state.world());
    probesDirty_ = true;  /* the world the probes captured no longer exists */

    /* The UI's font atlases, HERE rather than on first use. First use is the
     * splash's opening frame, and rasterising five faces there would stall the
     * one screen whose entire job is to look composed while something else is
     * slow. This runs before the window is revealed, with everything else that
     * loads. */
    gameUi_.setup();


    return true;
}

/* The renderer's sun, in the form the baker wants. One source of truth for
 * where the sun is; SunLight owns it and this converts. */
SunSample FrameRenderer::currentSun() const
{
    const Vector3 travel = sun_.travelDirection();

    SunSample sample;
    sample.directionX = travel.x;
    sample.directionY = travel.y;
    sample.directionZ = travel.z;

    sample.angularRadius = sun_.angularRadius();
    return sample;
}

void FrameRenderer::uploadLightmap()
{
    if (!lightBake_) return;

    lightBake_->refreshAtlas();
    const SunLightmapLayout& layout = lightBake_->layout();

    /* The atlas grows when destruction changes the patch count, so the texture
     * is recreated rather than updated whenever the layout moves. */
    const bool sizeChanged = lightmapTexture_.id != 0 &&
                             (lightmapTexture_.width != layout.width ||
                              lightmapTexture_.height != layout.height);
    if (sizeChanged) {
        UnloadTexture(lightmapTexture_);
        lightmapTexture_ = Texture2D{};
    }

    if (lightmapTexture_.id == 0) {
        Image image = { 0 };
        image.data    = const_cast<unsigned char*>(lightBake_->atlas().data());
        image.width   = layout.width;
        image.height  = layout.height;
        image.mipmaps = 1;
        image.format  = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;

        lightmapTexture_ = LoadTextureFromImage(image);   /* copies; does not adopt */
        SetTextureFilter(lightmapTexture_, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(lightmapTexture_, TEXTURE_WRAP_CLAMP);
    } else {
        UpdateTexture(lightmapTexture_, lightBake_->atlas().data());
    }

    /* NEAREST, always: this is an integer slot number, and interpolating
     * between two patch indices produces a third that means nothing. */
    if (lightIndexTexture_.id == 0) {
        Image image = { 0 };
        image.data    = const_cast<unsigned char*>(lightBake_->indexMap().data());
        image.width   = layout.indexWidth;
        image.height  = layout.indexHeight;
        image.mipmaps = 1;
        image.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

        lightIndexTexture_ = LoadTextureFromImage(image);
        SetTextureFilter(lightIndexTexture_, TEXTURE_FILTER_POINT);
        SetTextureWrap(lightIndexTexture_, TEXTURE_WRAP_CLAMP);
    } else {
        UpdateTexture(lightIndexTexture_, lightBake_->indexMap().data());
    }

    pbr_.setLightmapLayout(
        Vector4{ static_cast<float>(layout.texelsPerTile),
                 static_cast<float>(layout.patchesPerRow),
                 static_cast<float>(layout.width),
                 static_cast<float>(layout.height) },
        Vector4{ static_cast<float>(layout.gridWidth),
                 static_cast<float>(layout.gridHeight),
                 static_cast<float>(layout.gridDepth),
                 kCellHeight },
        Vector2{ static_cast<float>(layout.indexWidth),
                 static_cast<float>(layout.indexHeight) });
}

/* The whole answer to "does baking survive destructible terrain". The geometry
 * changed, so the patch set is re-derived (surviving texels carried across by
 * their stable cell/face key) and only what the change could have altered is
 * re-baked: the blast, plus everything its cells were shading. */
void FrameRenderer::rebakeAfterChange(const GameState& state, const Cell& centre,
                                      float radiusTiles)
{
    if (!lightBake_) return;
    lightBake_->refreshGeometry();
    lightBake_->bakeRegion(currentSun(), centre, radiusTiles);
    uploadLightmap();
}

const Material& FrameRenderer::prepassMaterial() const
{
    return prepass_.valid() ? prepass_.material() : depthMaterial_;
}

void FrameRenderer::worldBounds(const GameState& state, Vector3& minimum,
                                Vector3& maximum) const
{
    const Lattice& lattice = state.world().lattice();

    /* A tile of margin on every side: geometry sits inside the grid, but a low
     * sun throws its shadow past the edge, and clipping the projection there
     * would cut those shadows off in mid air. */
    constexpr float kMargin = 1.0f;

    minimum = Vector3{ -kMargin, -kMargin, -kMargin };
    maximum = Vector3{ static_cast<float>(lattice.width()) + kMargin,
                       static_cast<float>(lattice.storeys()) * kStoreyHeight + kMargin,
                       static_cast<float>(lattice.height()) + kMargin };
}

/* ------------------------------------------------------ the second cameras
 *
 * WHAT ADDING A CAMERA COSTS, which is the only real measure of whether the
 * abstraction is right: describe it and hand it over. No target to allocate, no
 * lambda forwarding a camera's own settings back to the renderer, no profiler
 * name to keep distinct, no phase to hand-pick so it does not redraw on the
 * same frame as its neighbours. CameraSet does all four.
 *
 * These two are a plan and a view of the same board, which between them
 * exercise everything a caller can vary — projection, layers, whether the
 * camera gets its own screen-space buffers — so the third one somebody adds has
 * a worked example of each. */
void FrameRenderer::captureOverview(float deltaSeconds)
{
    if (view_.state == nullptr) return;

    /* Framed to the LONGER side, so a rectangular map fits whole in a square
     * picture rather than being cropped along its long axis. */
    Vector3 minimum;
    Vector3 maximum;
    worldBounds(*view_.state, minimum, maximum);
    const float span = std::max(maximum.x - minimum.x, maximum.z - minimum.z);
    const Vec3 centre{ (minimum.x + maximum.x) * 0.5f, 0.0f, (minimum.z + maximum.z) * 0.5f };

    if (cameras_.empty()) {
        /* A PLAN. Orthographic, so parallel walls stay parallel and two units a
         * tile apart are a tile apart wherever they stand — the thing that makes
         * a map readable as a map.
         *
         * FIVE TIMES A SECOND. Nothing at board scale moves fast enough to need
         * more, and the gap between that and every-frame is the gap between a
         * second camera that is nearly free and one that halves the frame rate.
         * The set spreads their phases, so the two never land together.
         * --minimap-realtime overrides it to every-frame, so a test can rule
         * the schedule out; the interval stays the shipped answer. */
        CameraDesc plan;
        plan.name = "plan view";
        plan.width = plan.height = 384;
        plan.schedule = minimapRealtime_ ? CaptureSchedule::everyFrame()
                                         : CaptureSchedule::interval(0.2f);
        plan.camera = Camera::orthographic(span);
        plan.camera.overlooking(centre, span, maximum.y + 20.0f).withLayers(planView());
        planView_ = cameras_.add(std::move(plan));

        /* THE SAME SUBJECT, EVERYTHING ELSE DIFFERENT — which is the point of
         * having both. Perspective rather than orthographic, the overlays left
         * on, and its OWN depth prepass, occlusion and decal buffers so that its
         * ssao and decals features left ON — which is all it takes: a camera
         * that asks for a screen-space feature is given the depth prepass it
         * needs, so the map-versus-security-camera distinction is just whether
         * the preset kept those two switches. */
        CameraDesc feed;
        feed.name = "camera feed";
        feed.width = feed.height = 384;
        feed.schedule = CaptureSchedule::interval(0.2f);
        feed.camera = Camera::perspective(50.0f);
        feed.camera.overlooking(centre, span).withLayers(worldOnly());
        feed.camera.layers().show(drawLayer::kOverlays);
        cctvView_ = cameras_.add(std::move(feed));
    }

    /* Re-aimed every frame, because the board can be rebuilt under them. Moving
     * a camera afterwards is what the handle is for — and it is the same method
     * that placed it, on the same type the main view uses. */
    if (Camera* plan = cameras_.find(planView_)) plan->overlooking(centre, span, maximum.y + 20.0f);
    if (Camera* feed = cameras_.find(cctvView_)) feed->overlooking(centre, span);

    /* ONE DRAW FUNCTION FOR EVERY CAMERA, however many there are. The camera
     * arrives as an argument and carries its own layers and buffers, so nothing
     * has to be threaded in per camera. */
    const CutawayView cutaway = view_.cutaway;
    cameras_.renderAll(deltaSeconds, [this, cutaway](Camera& camera, Camera::ScenePhase phase,
                                                     float width, float height) {
        drawCameraScene(camera, phase, width, height, cutaway);
    });
}

void FrameRenderer::drawCameraScene(Camera& camera, Camera::ScenePhase phase, float width,
                                    float height, const CutawayView& cutaway)
{
    /* The camera unpacked and handed to the ONE scene function. layers()
     * FIRST, deliberately: the non-const call settles the screen-space
     * buffers against the layers — which can allocate or free the very object
     * buffers() then hands out — so the fetch order is the correctness. */
    const ViewLayers& requested = camera.layers();
    drawSceneForView(camera.toRaylib(), requested, camera.buffers(), phase, width, height,
                     cutaway);
}

void FrameRenderer::drawSceneForView(const Camera3D& rig, const ViewLayers& requested,
                                     ScenePassBuffers* buffers, Camera::ScenePhase phase,
                                     float width, float height, const CutawayView& cutaway)
{
    /* THE VIEW'S LAYERS, MADE ACTIVE FOR THE DURATION OF THE PASS.
     *
     * Every layer test in this file reads layers(), and there are a couple of
     * dozen of them. Threading a second ViewLayers through drawGeometryLit and
     * everything under it would mean touching all of them and would leave the
     * next one written to be found later; repointing the one slot they already
     * read means any view's flags are honoured by passes that were never told
     * more than one view exists.
     *
     * RAII rather than a restore at the end, because this function can return
     * early and leaving the main view drawing with the minimap's layers is a
     * whole-frame corruption for a one-line mistake. */
    class ScopedLayers {
    public:
        ScopedLayers(const ViewLayers*& slot, const ViewLayers& replacement)
            : slot_(slot), saved_(slot) { slot_ = &replacement; }
        ~ScopedLayers() { slot_ = saved_; }
        ScopedLayers(const ScopedLayers&) = delete;
        ScopedLayers& operator=(const ScopedLayers&) = delete;
    private:
        const ViewLayers*& slot_;
        const ViewLayers*  saved_;
    };

    ViewLayers effective = requested;

    /* SCREEN-SPACE EFFECTS NEED THIS VIEW'S OWN BUFFERS, and whether they
     * exist is what decides these two flags — not some rule that a capture
     * cannot have them.
     *
     * Occlusion and the DBuffer are both reconstructed from a depth prepass:
     * SSAO orients its sampling hemisphere from the prepass normals, and the
     * DBuffer is written by unprojecting the prepass depth. Rendered from a
     * different camera at a different size, those are not degraded, they are
     * WRONG — occlusion smeared in the shape of a view nobody is looking
     * through. So a capture WITHOUT its own set gets them switched off; one
     * WITH a set runs the real passes below and gets them properly.
     *
     * That is the minimap/CCTV split, and it is per capture. See
     * Camera::hasScreenSpaceEffects. */
    if (buffers == nullptr || !buffers->valid()) {
        effective.features.ambientOcclusion = false;
        effective.features.decals = false;
    } else {
        effective.features.ambientOcclusion =
            effective.features.ambientOcclusion && buffers->occlusionAvailable();
        effective.features.decals = effective.features.decals && buffers->decalsAvailable();
    }

    const ScopedLayers scoped(activeLayers_, effective);

    /* ---- over the finished picture, after this camera's tone map ---------
     *
     * THE SAME PASSES THE MAIN VIEW DRAWS AFTER ITS RESOLVE, in display
     * colour — which is the whole point of the phase: a capture, a pane and
     * the screen are one system, and a layer switch means the same thing on
     * all of them. The rings fade against a depth texture from THIS camera,
     * so like the screen-space features they need its own prepass; a camera
     * without one skips them rather than drawing them wrong. Their GLOW stays
     * main-view-only for now — bloom needs per-camera blur targets nobody has
     * asked to pay for — so kRingGlow should stay hidden on capture cameras. */
    if (phase == Camera::ScenePhase::Display) {
        if (buffers != nullptr && buffers->valid()
            && layers().drawing(drawLayer::kMovementRings)) {
            RibbonPassSettings ribbon = view_.ribbon;
            ribbon.camera = rig;
            BeginMode3D(rig);
            ribbonRenderer_->submit(ribbon, 1.0f, width, height,
                                    buffers->depth().depthTexture());
            EndMode3D();
        }
        return;
    }

    /* ---- this camera's own prepass, decals and occlusion -----------------
     *
     * THE OFFSCREEN PHASE, and it runs with nothing bound. Each of these opens
     * a render target of its own, and raylib's BeginTextureMode does not nest —
     * EndTextureMode binds framebuffer zero, so a target opened inside this
     * camera's colour buffer would never hand it back and every later draw
     * would land on the backbuffer. That produced exactly one symptom: a black
     * minimap. See Camera::ScenePhase. */
    if (phase == Camera::ScenePhase::Offscreen) {
        if (buffers == nullptr || !buffers->valid()) return;
        /* THE SAME THREE PASSES THE MAIN VIEW RUNS, in the same order and for
         * the same reasons — the prepass first because both of the others are
         * unprojected from it, decals before occlusion because the DBuffer is a
         * material write and occlusion is not.
         *
         * Sized to the capture's HDR buffer rather than its resolve, so the
         * occlusion buffer is sampled at the coordinates the lit pass fragments
         * actually have. */
        {
            CW_GPU_ZONE("prepass");
            DepthTarget::Scope scope(buffers->depth());
            ClearBackground(BLANK);
            BeginMode3D(rig);

            /* Blending OFF, for the reason the main prepass documents at
             * length: this buffer's ALPHA IS ROUGHNESS, not coverage, and
             * blending into it mixes two surfaces per pixel. */
            rlDisableColorBlend();
            drawGeometryPrepass();
            rlEnableColorBlend();

            EndMode3D();
        }

        if (effective.features.decals) {
            CW_GPU_ZONE("decals");
            decalRenderer_.render(decals_, rig, buffers->decals(),
                                  buffers->depth().depthTexture(),
                                  buffers->depth().colourTexture(),
                                  view_.decalGhost ? &*view_.decalGhost : nullptr);
        }

        {
            CW_GPU_ZONE("ssao");
            buffers->occlusion().setEnabled(effective.features.ambientOcclusion);
            buffers->occlusion().render(rig, buffers->depth().depthTexture(),
                                        buffers->depth().colourTexture());
        }
        return;
    }

    /* ---- the lit pass, INSIDE this camera's colour target ---------------- */

    /* THE ENVIRONMENT, SET UP FOR THIS PASS'S SIZE AND CAMERA. Everything here
     * is overwritten by the main pass immediately afterwards, which is the
     * ordering the call site depends on. */
    pbr_.updateEnvironment(sun_, shadows_, rig.position);
    pbr_.setSceneSize(width, height);

    /* SHADOWS AND REFLECTIONS ARE GENUINELY AVAILABLE, and that is worth
     * knowing: unlike the two above, both are WORLD-space. The sun's shadow map
     * is an orthographic projection over the whole board and the probes are
     * cubemaps parallax-corrected against it, so they are as valid from this
     * camera as from any other. Which makes them real per-camera options rather
     * than ones that only work in the off direction. */
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1,
                           PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    const bool shadowed = effective.features.shadows && shadows_.valid();

    /* THIS CAPTURE'S occlusion, not the main view's. AmbientOcclusion::texture()
     * hands back 1x1 white when it is disabled or absent, which the lit shader
     * multiplies by and never has to know the effect is missing — so the two
     * cases need no branch here. */
    const Texture2D occlusion = (buffers != nullptr && buffers->valid())
                                    ? buffers->occlusion().texture()
                                    : white;

    pbr_.setShadowsEnabled(shadowed);
    pbr_.bindFrameTextures(shadowed ? shadows_.depthTexture() : white,
                           occlusion,
                           lightmapTexture_, lightIndexTexture_,
                           shadowed ? shadows_.transmissionTexture() : white);

    pbr_.setDecalsEnabled(effective.features.decals);
    /* `decals` cannot survive the gate above without buffers existing, but that
     * correlation lives across a branch merge the analyzer will not carry —
     * so the dependency is restated here rather than suppressed. */
    if (buffers != nullptr && effective.features.decals) pbr_.bindDecalBuffer(buffers->decals());

    if (effective.features.reflections && probes_.valid()) pbr_.setEnvironmentProbes(probes_);
    else                                          pbr_.clearEnvironmentProbes();

    /* THE SKY, BEFORE THE 3D MODE AND INSIDE THE TARGET — the exact ordering
     * ScenePhase::Main exists to make sayable (see Camera.hpp). Without this a
     * capture with sky on rendered a world floating in transparency, and a
     * splitscreen pane is a whole player's view: it needs the sky the main
     * pass has. */
    if (layers().features.sky) sky_.draw(sun_, rig, width, height);

    /* The projection lives on the camera and BeginMode3D honours it, so this
     * one function draws both the orthographic and the perspective capture
     * without knowing which it is doing. */
    BeginMode3D(rig);
    drawGeometryLit(cutaway);

    /* THE PROBE BALLS, and deliberately NOT inside drawGeometryLit: that
     * function is also what the probe capture draws with, so a ball added
     * there would be captured into every cubemap — each probe would see the
     * others as chrome spheres hanging in the room, and then see those
     * reflections reflected. A debug overlay has no business inside the data
     * it is there to inspect. Every view shows them under debug view 2, the
     * main one included — one scene function, one rule. */
    if (view_.settings->debugView == 2)
        probeSpheres_.draw(probes_, sun_, rig.position, sun_.ambientIntensity());

    /* THE GAME'S ANNOTATIONS, honoured identically for every view — this is
     * what makes any camera's `show(kOverlays)` real. Same shader scope, same
     * gate, same pass, whoever is looking. */
    if (layers().drawing(drawLayer::kOverlays)) {
        OverlayShader::Scope unlit(overlayShader_);
        drawOverlays();
    }

    /* Debug geometry, inside the 3D mode so it depth-tests against this
     * camera's scene — the same reasoning as the main pass. */
    if (layers().features.debugDraw) debugRenderer_.draw(DebugDraw::get());

    EndMode3D();

    /* Not here: the movement ribbons. They draw in DISPLAY colour after the
     * tone map — drawing them in this phase would tone-map them like radiance
     * and wash them out. They run in ScenePhase::Display above. */
}

/* --------------------------------------------------------------- the minimap
 *
 * THE ORTHOGRAPHIC CAPTURE, ON SCREEN. Everything above it produced a texture;
 * this is the ten lines that put it in front of the player, and they are
 * separate from the capture on purpose — where a picture is shown is a HUD
 * decision, and the camera that made it should not have an opinion about screen
 * corners.
 *
 * DRAWN AFTER THE RESOLVE, so it is composited over the finished frame at its
 * own exposure rather than being tone-mapped a second time. */
void FrameRenderer::drawMinimap()
{
    if (!view_.settings->minimap) return;

    const Camera* plan = cameras_.find(planView_);
    if (plan == nullptr || !plan->rendersToTexture()) return;
    if (!plan->schedule().everCaptured()) return;  /* nothing in it yet */

    CW_PROFILE_ZONE_N("minimap");

    /* Reference pixels times the display scale, the same arrangement the widget
     * kit uses — a fixed pixel size is a different physical size on every
     * monitor. See ui/core/UiContext.hpp. */
    const float scale = GetWindowScaleDPI().y > 0.0f ? GetWindowScaleDPI().y : 1.0f;
    const float size = 200.0f * scale;
    const float margin = 16.0f * scale;

    const Rectangle destination{ static_cast<float>(GetScreenWidth()) - size - margin, margin,
                                 size, size };

    /* A plate behind it, because the capture clears to transparent and a map
     * with holes in it over a bright floor is unreadable. */
    DrawRectangleRec(destination, Color{ 0, 0, 0, 160 });

    /* drawTo, NOT DrawTexture: a render texture's rows are stored bottom-up, so
     * a straight blit shows the board mirrored — and on a roughly symmetrical
     * map that is invisible until somebody walks north and the marker goes
     * south. See Camera::drawTo. */
    plan->drawTo(destination);

    DrawRectangleLinesEx(destination, std::max(1.0f, scale), Color{ 255, 255, 255, 60 });
}

/* ------------------------------------------------------------- splitscreen
 *
 * PANES ARE ORDINARY CAPTURES — same set, same scene pass, same schedule
 * machinery — which is why this whole feature is three short functions. The
 * layouts and the cost argument live in cromwell/camera/SplitScreen.hpp. */

void FrameRenderer::syncSplitPanes()
{
    const int count = paneCount(split_);
    const float windowW = static_cast<float>(GetScreenWidth());
    const float windowH = static_cast<float>(GetScreenHeight());

    /* The stand-in viewpoints: one per corner of the board, the way four
     * players sit around a table. Real multiplayer replaces these poses with
     * the other players' cameras; nothing else here would change. */
    Vector3 minimum, maximum;
    worldBounds(*view_.state, minimum, maximum);
    const Vec3 centre{ (minimum.x + maximum.x) * 0.5f, 0.0f,
                       (minimum.z + maximum.z) * 0.5f };
    const float span = std::max(maximum.x - minimum.x, maximum.z - minimum.z);
    constexpr float kCornerX[4] = { 1.0f, -1.0f, -1.0f, 1.0f };
    constexpr float kCornerZ[4] = { -1.0f, -1.0f, 1.0f, 1.0f };

    for (int i = 0; i < count; ++i) {
        const Rectangle rect = paneRect(split_, i, windowW, windowH);

        if (panes_[i] == 0) {
            static const char* kNames[4] = { "pane 1", "pane 2", "pane 3", "pane 4" };
            CameraDesc pane;
            pane.name = kNames[i];
            pane.width = static_cast<int>(rect.width);
            pane.height = static_cast<int>(rect.height);
            pane.schedule = CaptureSchedule::everyFrame();
            pane.camera = Camera::perspective(50.0f);
            pane.camera
                .at(centre
                    + Vec3{ kCornerX[i] * span * 0.9f, span * 0.8f, kCornerZ[i] * span * 0.9f })
                .lookingAt(centre);
            pane.camera.withLayers(worldOnly());
            panes_[i] = cameras_.add(std::move(pane));
        }

        Camera* pane = cameras_.find(panes_[i]);
        if (pane == nullptr) continue;

        /* Tracks layout and window changes; a no-op at the same size. */
        pane->renderingToTexture(static_cast<int>(rect.width), static_cast<int>(rect.height));

        /* THE PANE'S SOURCE, MIRRORED EVERY FRAME — pose, lens, projection,
         * layers. Pane 0's source is the view camera by definition, so the
         * director's cuts and the panel's layer switches show up in it
         * exactly as they would fullscreen; any other pane mirrors whatever
         * setPaneSource gave it — a second player's rig, a replay camera —
         * or keeps its corner stand-in when nothing has. Rings included: the
         * Display phase draws them over the pane's resolve, provided the
         * source's layers keep a screen-space feature on so the pane has the
         * depth prepass they fade against. Only the ring GLOW is hidden —
         * bloom is still a main-view pass; see drawSceneForView. */
        const Camera* source = (i == 0) ? view_.camera : paneSources_[i];
        if (source != nullptr) {
            if (pane->projection() != source->projection())
                pane->switchTo(source->projection());
            pane->at(source->position()).lookingAt(source->target(), source->up());
            pane->setLens(source->lens());

            ViewLayers mirrored = source->layers();
            mirrored.hide(drawLayer::kRingGlow);
            pane->withLayers(mirrored);
        }
    }

    /* The layout shrank: free the panes past the count. */
    for (int i = count; i < 4; ++i) {
        if (panes_[i] != 0) {
            cameras_.remove(panes_[i]);
            panes_[i] = 0;
        }
    }
}

void FrameRenderer::releaseSplitPanes()
{
    for (CameraId& id : panes_) {
        if (id != 0) {
            cameras_.remove(id);
            id = 0;
        }
    }
}

void FrameRenderer::drawSplitScreen()
{
    CW_PROFILE_ZONE_N("split composite");

    const float windowW = static_cast<float>(GetScreenWidth());
    const float windowH = static_cast<float>(GetScreenHeight());
    const int count = paneCount(split_);

    for (int i = 0; i < count; ++i) {
        const Camera* pane = cameras_.find(panes_[i]);
        if (pane == nullptr || !pane->rendersToTexture()) continue;

        const Rectangle rect = paneRect(split_, i, windowW, windowH);
        pane->drawTo(rect);

        /* A hairline seam, so two panes of similar scenery read as two panes
         * rather than one continuous, subtly wrong picture. */
        DrawRectangleLinesEx(rect, 1.0f, Color{ 0, 0, 0, 180 });
    }
}

void FrameRenderer::rebuildRibbons(const GameState& state, const RibbonTuning& tuning)
{
    ribbonMeshes_->clear();

    BandExtractor extractor(state.world());
    Band    moveBand, sprintBand;
    LoopSet loops;

    /* BOTH rings are built here; which one displays is a hover decision, not
     * a rebuild — see RingSelector. */
    state.buildBand(state.moveBudget(), moveBand);
    extractor.extract(moveBand, loops);
    ribbonStats_.moveLoops = loops.loopCount();
    ribbonStats_.moveEdges = loops.edgeCount();
    ribbonMeshes_->append(state.world(), loops, tuning.moveColour, Ring::Move,
                          tuning.width, tuning.lift);

    /* AMBER MINUS BLUE, and the move band is kept alive for exactly that.
     *
     * The sprint band CONTAINS the move band, so the two frontiers coincide
     * wherever what stops you is the ground running out rather than the
     * budget — the lip of an upper storey's floor plate being the case that
     * shows it, where a whole room's perimeter is shared and neither budget is
     * the reason you cannot walk off the edge. Both ribbons then land on the
     * same grid line, amber is drawn second and wins, and the blue ring
     * disappears under it.
     *
     * Suppressing amber there is not a tie-break, it is the meaning: amber
     * says "sprinting takes you PAST walking", and on a shared edge it does
     * not. The suppressed edges come back as gaps, so the amber loops arrive
     * cut into open runs. (The TAB debug cycle's sprint-only view inherits
     * those gaps — with blue hidden they read as holes, which is the honest
     * picture of a frontier the two rings share.) */
    state.buildBand(state.sprintBudget(), sprintBand);
    extractor.extract(sprintBand, &moveBand, loops);
    ribbonStats_.sprintLoops = loops.loopCount();
    ribbonStats_.sprintEdges = loops.edgeCount();
    ribbonMeshes_->append(state.world(), loops, tuning.sprintColour, Ring::Sprint,
                          tuning.width, tuning.lift);

    ribbonBuilt_ = tuning;
}

/* The render targets that track the window. */
void FrameRenderer::resizeForWindow()
{
    /* One resize for the whole screen-space set — prepass, occlusion and
     * DBuffer stay the same size as each other by construction, which used to
     * be three calls and a comment asking them to agree. */
    mainBuffers_.resize(GetScreenWidth(), GetScreenHeight());
    glow_->resize(GetScreenWidth(), GetScreenHeight());
    resizeSceneTarget(GetScreenWidth(), GetScreenHeight());
}

void FrameRenderer::drawGeometry(const CutawayView& cutaway, const Material& material,
                                 bool castersOnly)
{
    if (layers().drawing(drawLayer::kStatics)) statics_->draw(cutaway, material, castersOnly);
    if (layers().drawing(drawLayer::kProps))   props_.draw(material);
    if (!layers().drawing(drawLayer::kUnits))  return;

    const Unit* animating = (*view_.animator).isRunning() ? &view_.state->selectedUnit() : nullptr;
    units_->drawRoster(view_.state->roster(), cutaway.maxStorey, animating, material);

    if (animating) {
        const PathPoint position = (*view_.animator).positionOn((*view_.preview));
        const float offset = UnitRenderer::centreOffset(*animating);
        units_->drawAt(*animating,
                       position.x + (offset > 0.5f ? 0.5f : 0.0f),
                       position.height,
                       position.y + (offset > 0.5f ? 0.5f : 0.0f),
                       material);
    }
}

void FrameRenderer::drawGeometryLit(const CutawayView& cutaway)
{
    /* The lattice is baked; everything else is not. Props carry no lightmap
     * UVs and units move, so both stay on the shadow map — which is exactly
     * Source 2's split between lightmapped world and mesh entities. */
    pbr_.setDebugView(view_.settings->debugView);

    /* HERE RATHER THAN ONCE PER FRAME, so the probe capture shades with the
     * same terms the scene does — this function is what draws both. Switching
     * the sun off and seeing it survive in the reflections would be a switch
     * that half works, which is worse than one that does not. */
    pbr_.setLightingSuppress(view_.settings->effects.suppressMask());
    pbr_.setLightmapEnabled(view_.settings->useBakedSun);
    if (layers().drawing(drawLayer::kStatics))
        statics_->drawLit(cutaway, materials_, pbr_,
                          /*includeTransparent=*/view_.settings->flatShading());

    pbr_.setLightmapEnabled(false);
    if (layers().drawing(drawLayer::kProps)) props_.drawLit(materials_, pbr_);

    /* Every body is one material, so its factors go up once rather than per
     * unit; only the albedo tint changes between them, and that travels in the
     * material's diffuse colour. */
    pbr_.setMaterialFactors(materials_.factorsOf(SurfaceKind::Body));
    pbr_.setMaterialOptions(materials_.optionsOf(SurfaceKind::Body));
    pbr_.setMaterialTransmission(
        materials_.transmissionOf(materials_.handleOf(SurfaceKind::Body)));
    const Material& bodyMaterial = materials_.material(SurfaceKind::Body);

    if (layers().drawing(drawLayer::kUnits)) {
        const Unit* animating = (*view_.animator).isRunning() ? &view_.state->selectedUnit() : nullptr;
        units_->drawRoster(view_.state->roster(), cutaway.maxStorey, animating, bodyMaterial);

        if (animating) {
            const PathPoint position = (*view_.animator).positionOn((*view_.preview));
            const float offset = UnitRenderer::centreOffset(*animating);
            units_->drawAt(*animating,
                           position.x + (offset > 0.5f ? 0.5f : 0.0f),
                           position.height,
                           position.y + (offset > 0.5f ? 0.5f : 0.0f),
                           bodyMaterial);
        }
    }

    /* Glass LAST. It blends against whatever is already in the colour buffer,
     * so everything it should be seen through — walls, floors, props, the
     * bodies standing behind it — has to be there first.
     *
     * PREMULTIPLIED blending, not ordinary alpha. The shader has already
     * scaled the diffuse by opacity and deliberately left specular alone, so
     * the blender must add what it is given rather than scale it again — that
     * is what keeps a nearly-clear pane's sun glint at full strength instead
     * of at six percent. */
    /* Already drawn solid, in the pass above — but only by the views that
     * replace surface shading. The probe view is an ordinary frame with balls
     * on top, so its glass blends normally. */
    if (view_.settings->flatShading() || !layers().drawing(drawLayer::kStatics)) return;

    pbr_.setLightmapEnabled(view_.settings->useBakedSun);

    /* BLENDING ON EXPLICITLY, because this function also runs inside a probe
     * capture and the capture turns it OFF — "the capture replaces, nothing
     * composites" is right for the opaque pass and wrong for this one.
     *
     * Unblended, a window REPLACES the world already drawn behind it with its
     * own premultiplied output: near-black rgb and a coverage alpha of about
     * 0.06. The shader reads that alpha as "almost entirely open sky" and
     * mixes in 94% of the analytic sky gradient — so every window in an
     * interior cubemap becomes a bright sky-coloured hole, and a wall
     * reflecting it looks precisely like a wall you can see daylight through.
     *
     * Premultiplied blending keeps the coverage right instead: over an opaque
     * background alpha resolves to 0.06 + 1 x 0.94 = 1, so the texel stays
     * "there is world here" and carries the pane's tint over the street behind
     * it. In the scene pass blending is already on and this changes nothing. */
    rlEnableColorBlend();
    rlSetBlendFactors(RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    statics_->drawTransparentLit(cutaway, materials_, pbr_);
    EndBlendMode();
}

void FrameRenderer::drawOverlays()
{
    if (view_.state->losMode()) overlays_->drawVisibility(view_.state->visibility(), view_.state->isoLevel());

    if (view_.settings->showCover) {
        const Unit& selected = view_.state->selectedUnit();
        if (selected.showsCoverShields()) overlays_->drawCoverShields(selected.position());

        if (view_.hovered) {
            const Cell hoverCell = view_.state->world().lattice().cellAt(*view_.hovered);
            if (hoverCell != selected.position()) overlays_->drawCoverShields(hoverCell);
        }
    }

    if (!(*view_.animator).isRunning() && view_.hovered) {
        const Cell hoverCell = view_.state->world().lattice().cellAt(*view_.hovered);
        const bool ok = view_.grenadeArmed ||
                        (view_.hoverRestOk &&
                         view_.state->reach().cost(*view_.hovered) <= view_.state->sprintBudget());

        const bool wideHull = view_.state->selectedUnit().footprint().isMultiTile() && !view_.grenadeArmed;
        const Color colour = view_.grenadeArmed ? palette::kHoverGrenade
                           : ok ? palette::kHoverValid : palette::kHoverInvalid;

        overlays_->drawHoverPlate(hoverCell, view_.state->hoverPlateHeight(hoverCell) + 0.03f,
                                  wideHull ? 1.96f : 0.96f, colour);
    }

    overlays_->drawPathPreview((*view_.preview));
    flashes_.draw();
}

DevModel FrameRenderer::buildDevModel() const
{
    DevModel model;
    const Unit& selected = view_.state->selectedUnit();

    model.selectedName = selected.hudLabel();
    model.selectedCell = selected.position();
    model.isoLevel     = view_.state->isoLevel();
    model.ringOverrideName = (*view_.rings).overrideName();
    model.softCutaway  = view_.settings->softCutaway;
    model.losMode      = view_.state->losMode();
    model.showCover    = view_.settings->showCover;
    model.grenadeArmed = view_.grenadeArmed;

    model.moveLoops   = ribbonStats_.moveLoops;
    model.moveEdges   = ribbonStats_.moveEdges;
    model.sprintLoops = ribbonStats_.sprintLoops;
    model.sprintEdges = ribbonStats_.sprintEdges;

    if (view_.hovered && !(*view_.animator).isRunning()) {
        model.hoverCell = view_.state->world().lattice().cellAt(*view_.hovered);
        if (view_.state->reach().isReachable(*view_.hovered)) model.hoverCost = view_.state->reach().cost(*view_.hovered);
        model.hoverRestOk = view_.hoverRestOk;
    }

    model.sunAzimuth      = sun_.azimuthDegrees();
    model.sunElevation    = sun_.elevationDegrees();
    model.shadowsActive   = shadows_.valid();
    model.occlusionActive = mainBuffers_.occlusion().active();
    model.bakedSun        = view_.settings->useBakedSun;
    model.debugView       = view_.settings->debugView;
    model.probeCount      = probes_.probeCount();
    model.cameraArgs      = view_.cameraArgs;

    model.status = view_.status;
    return model;
}

/* FOCUS THE SHADOW MAP ON WHAT THE CAMERA CAN SEE.
 *
 * Spreading one 4096 map over the whole 24x24 board spends its resolution
 * evenly on ground the player is not looking at. At a tactical camera the view
 * covers a fraction of the lattice, and concentrating the same texels there
 * multiplies the density — which is precisely what narrow apertures like
 * window and door frames need, because their shadow edges are long, thin and
 * shallow to the texel grid, the worst case for a staircase.
 *
 * The box is fitted to the view frustum INTERSECTED WITH THE WORLD, so looking
 * at the sky does not blow the fit up. Casters outside the frustum still cast
 * into it: the depth range extends well up-sun, and the shadow pass submits
 * the whole map regardless of the projection. */
void FrameRenderer::shadowFocus(const Camera3D& camera, Vector3& centre, float& radius) const
{
    Vector3 worldMin, worldMax;
    worldBounds(*view_.state, worldMin, worldMax);

    const float aspect = static_cast<float>(GetScreenWidth()) /
                         static_cast<float>(GetScreenHeight());
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const Vector3 right   = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    const Vector3 up      = Vector3CrossProduct(right, forward);

    /* Far enough to hold the board at any sane zoom, near enough that the fit
     * stays tight when the camera is pushed in. */
    const float shadowDistance =
        Vector3Length(Vector3Subtract(worldMax, worldMin)) * 1.2f;

    Vector3 boxMin{ 1e30f, 1e30f, 1e30f };
    Vector3 boxMax{ -1e30f, -1e30f, -1e30f };

    for (int step = 0; step < 2; step++) {
        const float distance = (step == 0) ? rlGetCullDistanceNear() : shadowDistance;
        const float halfHeight = std::tan(camera.fovy * 0.5f * DEG2RAD) * distance;
        const float halfWidth  = halfHeight * aspect;
        const Vector3 middle = Vector3Add(camera.position, Vector3Scale(forward, distance));

        for (int corner = 0; corner < 4; corner++) {
            const float sx = (corner & 1) ? 1.0f : -1.0f;
            const float sy = (corner & 2) ? 1.0f : -1.0f;
            const Vector3 point =
                Vector3Add(middle, Vector3Add(Vector3Scale(right, halfWidth * sx),
                                              Vector3Scale(up, halfHeight * sy)));
            boxMin = Vector3Min(boxMin, point);
            boxMax = Vector3Max(boxMax, point);
        }
    }

    /* Clip to the world. The frustum reaches into empty sky; the lattice does
     * not, and fitting to the sky would throw the resolution away again. */
    boxMin = Vector3Max(boxMin, worldMin);
    boxMax = Vector3Min(boxMax, worldMax);

    if (boxMin.x > boxMax.x || boxMin.y > boxMax.y || boxMin.z > boxMax.z) {
        /* Camera looking away from the board — fall back to the whole map. */
        centre = Vector3Scale(Vector3Add(worldMin, worldMax), 0.5f);
        radius = Vector3Length(Vector3Subtract(worldMax, worldMin)) * 0.5f;
        return;
    }

    centre = Vector3Scale(Vector3Add(boxMin, boxMax), 0.5f);
    radius = Vector3Length(Vector3Subtract(boxMax, boxMin)) * 0.5f;
}

void FrameRenderer::drawShadowMap()
{
    if (!shadows_.valid() || !layers().features.shadows) return;

    Vector3 centre;
    float   radius = 1.0f;
    shadowFocus(view_.camera->toRaylib(), centre, radius);

    /* Depth reaches the world's diagonal — enough for any caster to throw into
     * the focus sphere, and tight enough that the bias stays meaningful. */
    Vector3 worldMin, worldMax;
    worldBounds(*view_.state, worldMin, worldMax);
    const float depthExtent = Vector3Length(Vector3Subtract(worldMax, worldMin));

    const SunLight::ShadowProjection projection =
        sun_.shadowProjectionForSphere(centre, radius, depthExtent, ShadowMap::kResolution);


    /* THE FULL LATTICE, NOT THE PLAYER'S CUTAWAY — the same rule the probe
     * capture follows, and for the same reason. The iso level is a CAMERA
     * affordance: it hides the storeys between the eye and the room you want
     * to look into. It says nothing about what the world is made of, and the
     * sun does not care what the camera has been asked to skip.
     *
     * Submitting the cutaway here made the lighting a function of the view.
     * Dropping to the ground floor deleted the roof from the sun's depth pass,
     * so the interior it had been shading went abruptly to full sunlight — the
     * room brightened because you looked at it. Worse, it was inconsistent
     * with the two neighbouring systems: the baked sun (B) lightmaps the whole
     * lattice and never moved, and the reflection probes capture at full
     * depth, so a window reflected a shadowed room the floor in front of it no
     * longer agreed with.
     *
     * WHAT THIS COSTS, stated plainly because it was the original argument for
     * cutting: a hidden storey now throws a shadow from geometry that is not
     * drawn. On a lattice that is nearly always the same footprint the walls
     * below already shade, so it reads as the building's own shadow; the
     * visible case is a roof overhang darkening ground beside the building.
     * That is a shadow whose caster is real and merely undrawn, which is a far
     * smaller lie than lighting that changes when the player presses 1.
     *
     * AND NOW THE FACINGS TOO, which is the same rule applied to the newer
     * cut and matters more than the storey one ever did. The dynamic cutaway
     * removes the walls between the camera and a building's interior, and it
     * re-decides that every time the camera turns. Letting it reach this pass
     * would mean every building's near walls stopped and started casting as
     * the player swung the camera round — a shadow that breathes with the
     * rotation key. A default CutawayView cuts nothing, so this pass gets the
     * world as it is without having to ask for it. */
    ShadowMap::Scope scope(shadows_, projection);
    drawGeometry(CutawayView::whole(), depthMaterial_, /*castersOnly=*/true);

    /* Then the glass, into the same target's colour plane. Depth WRITES off so
     * a window never shadows what is behind it, depth TEST on so glass already
     * hidden behind a wall records nothing — light stopped by the wall never
     * reached that window to be tinted. */
    if (shadows_.valid()) {
        rlDisableDepthMask();
        statics_->drawKind(CutawayView::whole(), SurfaceKind::Window,
                           shadows_.transmitterMaterial());
        rlEnableDepthMask();
    }
}

/* The G-buffer pass. Structurally the same submission drawGeometry makes, but
 * pushing each material's roughness so the buffer's alpha carries something —
 * the prepass shader is shared, so this stays ONE pass, it just stops being
 * blind to what it is drawing.
 *
 * Props and units take a single value each rather than one per material. A
 * crate is not going to be a mirror, and threading per-model roughness through
 * PropSet to serve a reflection nobody will see is not worth the plumbing;
 * when a chrome prop exists, this is where it changes. */
void FrameRenderer::drawGeometryPrepass()
{
    const Material& material = prepassMaterial();

    if (layers().drawing(drawLayer::kStatics))
        statics_->drawPrepass(view_.cutaway, material, materials_, prepass_);

    prepass_.setRoughness(0.8f);
    if (layers().drawing(drawLayer::kProps)) props_.draw(material);

    prepass_.setRoughness(materials_.factorsOf(SurfaceKind::Body).x);
    if (layers().drawing(drawLayer::kUnits)) {
        const Unit* animating = (*view_.animator).isRunning() ? &view_.state->selectedUnit() : nullptr;
        units_->drawRoster(view_.state->roster(), view_.cutaway.maxStorey, animating, material);

        if (animating) {
            const PathPoint position = (*view_.animator).positionOn((*view_.preview));
            const float offset = UnitRenderer::centreOffset(*animating);
            units_->drawAt(*animating,
                           position.x + (offset > 0.5f ? 0.5f : 0.0f),
                           position.height,
                           position.y + (offset > 0.5f ? 0.5f : 0.0f),
                           material);
        }
    }
}

void FrameRenderer::rebuildEnvironmentProbes(const GameState& state)
{
    if (!probes_.valid()) return;

    Vector3 minimum, maximum;
    worldBounds(state, minimum, maximum);

    /* Flooded fresh every time rather than incrementally patched. The flood is
     * one pass over 5184 cells with a queue — cheaper than reasoning about
     * which rooms a detonation could have merged, and correct by construction
     * where the incremental version would be correct by argument. */
    const RoomPartition rooms(state.world());
    placeProbes(probes_, rooms, state.world().lattice(), minimum, maximum);
}

void FrameRenderer::captureEnvironmentProbes()
{
    /* A 1x1 white stands in for the occlusion buffer. SSAO is screen space —
     * its texture is addressed by gl_FragCoord against the SCENE's size, and
     * inside a 128-pixel cubemap face those coordinates mean nothing. White is
     * "nothing occluded", which is the right answer to a question that cannot
     * be asked here. */
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1,
                           PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    pbr_.setShadowsEnabled(layers().features.shadows);

    /* NO DECALS IN A CUBEMAP, for exactly the reason the occlusion buffer above
     * gets a white stand-in — and this one is worse, because it is not a subtle
     * error. The DBuffer is addressed by gl_FragCoord against uSceneSize, which
     * is the face size here, so the main camera's decals would be stretched
     * whole across EVERY face of EVERY probe: the reflection in a window would
     * carry a blood splatter from wherever it happened to be on screen, seen
     * from the wrong viewpoint, moving as the camera moved. That is the odd
     * reflection, and it is one uniform.
     *
     * IT IS ALSO WHAT EVERY OTHER ENGINE DOES. Unreal does not apply deferred
     * decals to reflection captures; Source 2's cubemaps are baked long before
     * a runtime decal exists. A decal still changes the reflection you SEE on
     * it, because it changes that surface's normal, roughness and f0 before
     * lighting — which is what makes a wet pool mirror the sky while the dry
     * road beside it does not. What it does not do is appear inside the
     * reflected image. Restored by render before the lit pass. */
    pbr_.setDecalsEnabled(false);

    pbr_.setSceneSize(static_cast<float>(ReflectionProbeSet::kFaceSize),
                      static_cast<float>(ReflectionProbeSet::kFaceSize));
    pbr_.bindFrameTextures(shadows_.depthTexture(), white,
                           lightmapTexture_, lightIndexTexture_,
                           shadows_.transmissionTexture());

    /* Surfaces drawn here sample the array's PREVIOUS contents — one bounce.
     * A second bounce would cost another full sweep to change a reflection
     * seen inside a reflection. */
    pbr_.setEnvironmentProbes(probes_);

    /* No BeginMode3D: capture() installs each face's matrices itself, exactly
     * as ShadowMap::Scope does for the sun.
     *
     * SHADED FROM THE PROBE'S POINT OF VIEW, per face, because with a probe
     * per room there is no longer one point that would do — every
     * view-dependent term has to be evaluated where the reflection is being
     * gathered, and that is a different place for every layer. */
    /* THE FULL LATTICE, NOT THE PLAYER'S CUTAWAY. The iso level is a camera
     * affordance — it hides upper storeys so you can see into the building —
     * and it has no business reaching the probes. Capturing under it strips
     * the ceiling and the upper walls out of every interior cubemap, so an
     * indoor probe records the sky and the street where its own room should
     * be, and every surface in that room then reflects the outdoors. That is
     * indistinguishable from the wall-leak this whole system exists to fix,
     * and it appears only once the player cuts away. */
    const int slices = probes_.stale() ? probeFacesPerFrame_ * 4 : probeFacesPerFrame_;
    probes_.capture([this](Vector3 eye) {
        pbr_.updateEnvironment(sun_, shadows_, eye);
        drawGeometryLit(CutawayView::whole());
    }, slices);
}

void FrameRenderer::render(const FrameView& view)
{
    /* The frame every pass below reads. */
    view_ = view;

    /* NOT THE GAME: no world, no passes, just a screen. Returning early rather
     * than guarding each pass keeps the two paths honestly separate - a menu
     * that accidentally ran the shadow map would still look right and cost a
     * full scene render to do it. */
    if (view_.uiState != UIState::InGame) {
        drawFrontEnd();
        return;
    }

    /* The main frame draws for the player's camera, so its layers are the live
     * ones until a capture swaps its own in — see activeLayers_. An in-game
     * FrameView always carries the camera; buildFrameView sets it. */
    activeLayers_ = &view_.camera->layers();

    RibbonPassSettings settings = view_.ribbon;

    /* THE VIEW CAMERA, NOT THE PAWN'S. The controller filled this with the
     * camera it computed the ribbon numbers from — which is the pawn's, and
     * was the same thing until the view target could point the screen at
     * another camera. The frame draws through FrameView::camera, so the copy
     * every pass below reads is overridden once here rather than at a dozen
     * BeginMode3D sites. */
    settings.camera = view_.camera->toRaylib();

    /* Whether panes replace the fullscreen pass this frame. The WORLD-space
     * work — shadow map, probes, the captures themselves — runs either way,
     * because it serves every camera; what a split skips is everything that
     * only exists to shade the one fullscreen view. */
    const bool split = split_ != SplitLayout::Single;

    /* PROFILING ZONES ARE PAIRED, CPU AND GPU, PER PASS. Both are needed and
     * they answer different questions: the CPU zone is how long submitting the
     * pass took, the GPU zone is how long running it took. A pass that is cheap
     * to submit and expensive to run is a shader problem; the reverse is a
     * draw-call problem. One timeline cannot tell them apart. */
    CW_PROFILE_ZONE_N("render");

    /* 1. THE SUN'S VIEW. Depth only, and first, because the lit pass samples
     *    what it writes. */
    {
        CW_PROFILE_ZONE_N("shadow map");
        CW_GPU_ZONE("shadow map");
        drawShadowMap();
    }

    /* 1b. THE REFLECTION PROBES. After the shadow map, so the world reflected
     *     in a window is a shadowed one.
     *
     *     THE REBUILD IS HERE AND NOT AT THE DETONATION. Destruction runs
     *     mid-frame, and re-placing probes underneath a pass that is already
     *     sampling them would swap the volume array out from under a draw
     *     call. Deferring to the top of the next frame costs one frame of a
     *     stale room partition and removes the race entirely. */
    if (probes_.valid() && layers().features.reflections) {
        if (probesDirty_) {
            rebuildEnvironmentProbes(*view_.state);
            probesDirty_ = false;
        }
        captureEnvironmentProbes();
    }

    /* 2. THE VIEW'S SCREEN-SPACE BUFFERS — prepass, decals, occlusion — run
     *    through THE ONE SCENE FUNCTION, exactly as every capture runs them.
     *    The main view is a camera whose output is the backbuffer, and
     *    drawSceneForView cannot tell it from a minimap. Its old private copy
     *    of these passes is gone, which is the point: a pass added to the
     *    scene function now exists for every camera, or for none. The
     *    G-buffer rules — write don't composite, overlays are not occluders,
     *    decals before occlusion — live there now, once. */
    if (!split)
        drawSceneForView(settings.camera, view_.camera->layers(), &mainBuffers_,
                         Camera::ScenePhase::Offscreen,
                         static_cast<float>(GetScreenWidth()),
                         static_cast<float>(GetScreenHeight()), view_.cutaway);

    /* 2b. THE SILHOUETTE MASK — the units alone, into their own target, each
     *     writing its tint and its depth. Nothing consumes it yet; it exists
     *     so that drawing a soldier through a wall later is one full-screen
     *     shader rather than a pipeline change. Cleared to transparent, so
     *     "nothing here" and "something here" are distinguishable by alpha. */
    if (!split && layers().features.customDepth && customDepth_.valid()) {
        CustomDepthStencil::Scope scope(customDepth_);
        ClearBackground(BLANK);
        BeginMode3D(settings.camera);

        /* PER OBJECT, not per category — which is the whole reason this is an
         * integer rather than a channel. Props share a value because nothing
         * distinguishes them yet; every unit gets its own, so a later pass can
         * outline one soldier without outlining the squad.
         *
         * 0 is reserved for "nothing", so ids start at 1 — the coverage bit in
         * alpha makes that unnecessary, but leaving 0 unused means a consumer
         * that forgets to check coverage fails visibly rather than subtly. */
        customDepth_.setStencil(kPropStencil);
        props_.draw(customDepth_.material());

        int nextStencil = kFirstUnitStencil;
        const Unit* animating = (*view_.animator).isRunning() ? &view_.state->selectedUnit() : nullptr;
        units_->drawRoster(view_.state->roster(), view_.cutaway.maxStorey, animating,
                           customDepth_.material(),
                           [this, &nextStencil](const Unit&) {
                               customDepth_.setStencil(nextStencil++);
                           });
        if (animating) {
            customDepth_.setStencil(nextStencil++);
            const PathPoint position = (*view_.animator).positionOn((*view_.preview));
            const float offset = UnitRenderer::centreOffset(*animating);
            units_->drawAt(*animating,
                           position.x + (offset > 0.5f ? 0.5f : 0.0f),
                           position.height,
                           position.y + (offset > 0.5f ? 0.5f : 0.0f),
                           customDepth_.material());
        }
        EndMode3D();
    }

    /* 3b. THE OVERVIEW CAPTURE, and it has to be HERE — after the shadow map
     *     exists to be sampled, and before the main pass sets its own
     *     environment up.
     *
     *     A scene pass pushes its whole environment into the shared PbrShader
     *     and nothing puts back what was there, so the only safe ordering is
     *     "capture first, main pass overwrites". Everything below re-establishes
     *     the lot, which is exactly what makes that safe. Move this after line
     *     919 and the battlefield gets lit with the minimap's state. Same rule
     *     ModelPreview documents at length, same reason. */
    /* The panes ride the same renderAll as the plan and feed cameras, so they
     * only need to exist and be aimed before it runs. */
    if (split) syncSplitPanes();
    else       releaseSplitPanes();

    captureOverview(GetFrameTime());

    /* 4. THE LIT SCENE, in linear radiance at supersampled resolution — the
     *    ONE SCENE FUNCTION's Main phase, into the frame's HDR target, exactly
     *    the arrangement Camera::captureNow makes for a capture. Skipped when
     *    panes tile the window: each pane already ran this same work through
     *    its own capture, and a fullscreen pass would shade pixels the
     *    composite is about to cover. */
    if (!split) {
        CW_PROFILE_ZONE_N("lit scene");
        CW_GPU_ZONE("lit scene");
        HdrTarget::Scope scope(scene_);
        ClearBackground(BLANK);
        drawSceneForView(settings.camera, view_.camera->layers(), &mainBuffers_,
                         Camera::ScenePhase::Main, static_cast<float>(sceneWidth_),
                         static_cast<float>(sceneHeight_), view_.cutaway);
    }

    /* 5. RESOLVE. Past this line everything is display colour on the
     *    backbuffer, which is why the ribbon and its glow did not have to
     *    change to gain a lit world behind them. Under a split there is no
     *    fullscreen scene to resolve; the panes' already-tonemapped textures
     *    tile the window instead. */
    BeginDrawing();
    ClearBackground(palette::kBackground);
    if (split) {
        drawSplitScreen();
    } else {
        /* The tone map switch travels INTO the resolve rather than branching
         * here — the pass blits raw when the curve is off, so there is no
         * second path for a future feature to forget. See ToneMapPass. */
        tonemap_.draw(scene_, static_cast<float>(GetScreenWidth()),
                      static_cast<float>(GetScreenHeight()), layers().features.toneMap);
    }

    /* The ribbon's live dials, pushed where the pass that reads them runs. */
    ribbonShader_->setPanSpeed(view_.settings->ribbon.panSpeed);
    glow_->setTuning(view_.settings->ribbon);

    /* 6. OVER THE FINISHED PICTURE — the scene function's Display phase, on
     *    the backbuffer: the movement ribbons, fading against the main view's
     *    own prepass, exactly as a capture's Display phase fades against its. */
    if (!split) {
        drawSceneForView(settings.camera, view_.camera->layers(), &mainBuffers_,
                         Camera::ScenePhase::Display,
                         static_cast<float>(GetScreenWidth()),
                         static_cast<float>(GetScreenHeight()), view_.cutaway);

        /* The emissive halo stays a main-view extra: bloom needs blur targets
         * at the output's size, and only the window has them. Must come after
         * the rings and before anything 2D over the top, which should not
         * glow. */
        if (layers().drawing(drawLayer::kMovementRings)
            && layers().drawing(drawLayer::kRingGlow))
            glow_->render(*ribbonRenderer_, settings, mainBuffers_.depth().depthTexture());
    }

    drawMinimap();

    const DevModel model = buildDevModel();

    /* Last, over everything, and inside BeginDrawing — rlImGui submits its
     * vertices through rlgl like any other 2D draw. The UI is not a layer: it
     * is what turns the layers back on.
     *
     * The exposure round-trip is so ToneMapPass keeps its setter rather than
     * handing out a reference to its own field. */
    float exposure = tonemap_.exposure();
    DevTunables tunables{ sun_, view_.settings->ribbon, mainBuffers_.occlusion().tuning(),
                          exposure, view_.settings->effects };

    /* Every intermediate the frame produced, so it can be LOOKED at rather
     * than reasoned about. Rebuilt per frame because several of these change
     * identity — the lightmap texture is recreated whenever destruction moves
     * the atlas, so a cached handle would go stale exactly when it matters. */
    /* The depth ramp spans the board rather than the far plane: raylib's far
     * plane is 1000 units and the whole lattice is about 34 across, so a ramp
     * scaled to the frustum would render every depth buffer as flat black. */
    {
        Vector3 minimum, maximum;
        worldBounds(*view_.state, minimum, maximum);
        previews_.setDepthRange(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR,
                                Vector3Length(Vector3Subtract(maximum, minimum)) * 1.5f);
    }

    using Preview = TexturePreviews::Mode;

    /* WHICH WAY UP EACH SOURCE IS STORED, stated per entry because only this
     * function knows. Almost everything here is a render target and therefore
     * bottom-up; the two lightmap textures are the exceptions, uploaded from
     * CPU arrays by LoadTextureFromImage above and so already top-down. Getting
     * this wrong does not fail, it just shows the buffer inverted. */
    using From = TexturePreviews::Origin;
    constexpr From kTarget = From::Framebuffer;
    constexpr From kUpload = From::Image;

    const Texture2D noTexture{};
    int slot = 0;

    DevTextures textures;
    textures.add("sun depth",
                 previews_.render(slot++, shadows_.depthTexture(), Preview::Raw, kTarget),
                 "the sun's shadow map, in its own orthographic depth");
    textures.add("sun transmission",
                 previews_.render(slot++, shadows_.transmissionTexture(), Preview::Raw, kTarget),
                 "sunlight surviving. white is open air, darker is glass");
    textures.add("ambient occlusion",
                 previews_.render(slot++, mainBuffers_.occlusion().texture(), Preview::Raw, kTarget),
                 "screen space. white is unoccluded; 1x1 white when off");
    textures.add("g-buffer depth",
                 previews_.render(slot++, mainBuffers_.depth().depthTexture(),
                                  Preview::Depth, kTarget),
                 "linearised and banded — each band is an equal slice of distance");
    textures.add("g-buffer normal",
                 previews_.render(slot++, mainBuffers_.depth().colourTexture(),
                                  Preview::Raw, kTarget),
                 "world normal, encoded n * 0.5 + 0.5");
    textures.add("g-buffer roughness",
                 previews_.render(slot++, mainBuffers_.depth().colourTexture(),
                                  Preview::Alpha, kTarget),
                 "the same buffer's alpha. black is a mirror, white is fully diffuse");
    textures.add("lightmap atlas",
                 previews_.render(slot++, lightmapTexture_, Preview::Raw, kUpload),
                 "baked sun visibility, packed per (cell, face)");
    textures.add("lightmap index",
                 previews_.render(slot++, lightIndexTexture_, Preview::Raw, kUpload),
                 "(cell, face) -> atlas slot, 16 bit across R and G");
    textures.add("custom stencil",
                 previews_.render(slot++, customDepth_.stencil(), Preview::Stencil, kTarget),
                 "one hue per object id; dark grey is nothing drawn");
    textures.add("custom depth",
                 previews_.render(slot++, customDepth_.depth(), Preview::Depth, kTarget),
                 "tagged objects only, to compare against the g-buffer's depth");
    /* NOT THE RENDERER'S INTERNAL BUFFERS, unlike everything above — each of
     * these is a whole second render of the world from another camera.
     *
     * ENUMERATED RATHER THAN LISTED, which is the point of the set holding
     * them: a camera added anywhere in the codebase shows up here without this
     * function being told about it. The name comes from the descriptor that
     * made it, so it is the same string the profiler row carries and the two
     * can be read against each other.
     *
     * The names outlive the frame because the set owns them — DevTextures
     * borrows its pointers and copies nothing; see the note below. */
    cameras_.forEach([&](CameraId, const std::string& name, const Camera& camera) {
        textures.add(name.c_str(),
                     previews_.render(slot++, camera.texture(), Preview::Raw, kTarget),
                     camera.hasScreenSpaceEffects()
                         ? "a second camera, with its own prepass: ssao and decals work"
                         : "a second camera. no prepass of its own, so no ssao or decals");
    });
    /* A MEMBER BUFFER, NOT TextFormat. DevTextures borrows its name and note
     * pointers and copies nothing, and TextFormat hands back one of four
     * rotating static buffers — four more formatted strings between here and
     * draw() and this label would be describing somebody else's texture. */
    std::snprintf(probePreviewNote_, sizeof(probePreviewNote_),
                  "room %d of %d, +X -X +Y -Y +Z -Z. magenta is open sky. %d slices stale",
                  probes_.probeCount() > 0 ? probes_.previewProbe() + 1 : 0,
                  probes_.probeCount(), probes_.staleFaceCount());
    textures.add("reflection probe",
                 previews_.render(slot++, probes_.previewTexture(), Preview::Raw, kTarget),
                 probePreviewNote_);

    /* The DBuffer, alongside every other intermediate — and it earns its place
     * more than most. A decal that fails to appear in the lit image has four
     * plausible causes (the projection missed, the angle fade rejected it, the
     * blend is wrong, the lit shader is not reading the planes) and these three
     * pictures separate them at a glance: ink here and nothing on screen is the
     * READ; nothing here is the PASS. */
    DevDecalTool decalTool;
    decalTool.available   = decalRenderer_.valid() && mainBuffers_.decalsAvailable();
    decalTool.placedCount = static_cast<int>(decals_.count());
    decalTool.cursorOnSurface = view_.cursorOnSurface;

    const int materials = static_cast<int>(decals_.materialCount());
    decalTool.materialCount =
        (materials < DevDecalTool::kMaxMaterials) ? materials : DevDecalTool::kMaxMaterials;
    for (int i = 0; i < decalTool.materialCount; i++)
        decalTool.materialNames[i] = decals_.materialName(i);

    textures.add("dbuffer albedo",
                 previews_.render(slot++, mainBuffers_.decals().albedo(), Preview::Raw, kTarget),
                 "decal base colour, premultiplied. black is untouched");
    textures.add("dbuffer normal",
                 previews_.render(slot++, mainBuffers_.decals().normal(), Preview::Raw, kTarget),
                 "decal world normal, encoded and premultiplied");
    textures.add("dbuffer coverage",
                 previews_.render(slot++, mainBuffers_.decals().albedo(), Preview::Alpha, kTarget),
                 "the SAME plane's alpha, which is 1 - coverage: "
                 "white is no decal, dark is fully inked");

    DevSteam steamPanel;
    steamPanel.running     = view_.steam.running;
    steamPanel.reason      = view_.steam.reason;
    steamPanel.persona     = view_.steam.persona;
    steamPanel.steamId     = view_.steam.steamId;
    steamPanel.avatarState = view_.steam.avatarState;
    steamPanel.avatarUrl   = view_.steam.avatarUrl;
    steamPanel.avatar      = steamAvatar_;

    /* Under the dev panel, over everything else — it is a full-screen scrim and
     * the ImGui panel has to stay usable on top of it. Costs nothing when
     * hidden. */
    uiGallery_.draw(gameUi_, settings.camera);

    /* THE PLAYER CAMERA'S OWN LAYERS, not the read-only active slot: the panel
     * edits these in place, and what it should edit is the main view — the same
     * per-camera settings a capture keeps for itself. See FrameView::camera. */
    ViewLayers& playerLayers = view_.camera->layers();
#if XC_HAVE_WEB
    devView_.draw(model, playerLayers, tunables, textures, decalTool,
                  steamPanel, devRequests_, webPanel_.get());
#else
    devView_.draw(model, playerLayers, tunables, textures, decalTool,
                  steamPanel, devRequests_);
#endif
    tonemap_.setExposure(exposure);
    EndDrawing();
}

/* -------------------------------------------------------------- the loop */
void FrameRenderer::rebuildRibbonsIfStale(const GameState& state, const RibbonTuning& tuning)
{
    if (tuning.geometryDiffers(ribbonBuilt_)) rebuildRibbons(state, tuning);
}

void FrameRenderer::rebuildStatics(const GameState& state)
{
    statics_->rebuild(state.world());
}

void FrameRenderer::rebakeAll(const GameState& state)
{
    (void)state;
    if (!lightBake_) return;
    lightBake_->bakeAll(currentSun());
    uploadLightmap();
}

DevRequests FrameRenderer::takeDevRequests()
{
    const DevRequests taken = devRequests_;
    devRequests_ = DevRequests{};
    return taken;
}

void FrameRenderer::setupDevView(int storeys) { devView_.setup(storeys); }
void FrameRenderer::setDevViewVisible(bool visible) { devView_.setVisible(visible); }
void FrameRenderer::toggleDevView() { devView_.toggleVisible(); }
void FrameRenderer::shutdownDevView() { devView_.shutdown(); }
void FrameRenderer::toggleUiGallery() { uiGallery_.toggleVisible(); }
void FrameRenderer::updateEffects(float deltaSeconds) { flashes_.update(deltaSeconds); }

FrameRenderer::UIRequest FrameRenderer::takeUIRequest()
{
    const UIRequest taken = uiRequest_;
    uiRequest_ = UIRequest{};
    return taken;
}

void FrameRenderer::drawFrontEnd()
{
    BeginDrawing();
    ClearBackground(palette::kBackground);

    /* Behind ImGui, and through raylib rather than ImGui::Image, because this
     * is a fullscreen blit under its own shader and going through the UI layer
     * to do it would mean a borderless window sized to the viewport for no
     * gain.
     *
     * ZONED like any other per-frame system, and this one is not free — it is
     * a sixteen-tap radial blur over every pixel of the window. It only runs
     * for two seconds, but an unzoned pass is invisible rather than zero, and
     * an unexplained spike at startup is exactly the hunt the panel exists to
     * prevent. */
    if (view_.uiState == UIState::SplashScreen) {
        CW_PROFILE_ZONE_N("splash");
        CW_GPU_ZONE("splash");
        splash_.draw(view_.splashSeconds);

        /* The loading bar, over the image and under ImGui. Drawn with the
         * engine's own widget kit rather than with ImGui because this is
         * PRODUCT UI — the first thing anyone sees — and the dev panel's
         * toolkit has no business in it. */
        drawSplashOverlay(gameUi_.begin(), view_.splashSeconds, view_.splashProgress);
        gameUi_.end();
    }

    rlImGuiBegin();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{ viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                         viewport->WorkPos.y + viewport->WorkSize.y * 0.5f };
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2{ 0.5f, 0.5f });
    ImGui::SetNextWindowSize(ImVec2{ 420.0f, 0.0f }, ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings;

    switch (view_.uiState) {
        case UIState::SplashScreen:
            /* The image carries the wordmark itself, so with it there is
             * nothing left for ImGui to draw. Without it, the text below IS
             * the splash — see splash_ in the header for why that case is
             * ordinary rather than an error.
             *
             * No titlebar and no input either way: the splash is a thing you
             * watch, and Application decides when it is over. */
            if (!splash_.available()) {
                ImGui::Begin("##splash", nullptr, kFlags | ImGuiWindowFlags_NoTitleBar |
                                                  ImGuiWindowFlags_NoInputs);
                ImGui::Dummy(ImVec2{ 0.0f, 24.0f });
                ImGui::SetWindowFontScale(2.0f);
                ImGui::TextUnformatted("cromwell");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::TextDisabled("an XCOM 2-style tactical prototype");
                ImGui::Dummy(ImVec2{ 0.0f, 24.0f });
                ImGui::End();
            }
            break;

        case UIState::MainMenu:
            ImGui::Begin("Main Menu", nullptr, kFlags);
            if (ImGui::Button("New Game", ImVec2{ -1.0f, 34.0f }))
                uiRequest_.goTo = UIState::InGame;
            if (ImGui::Button("Options",  ImVec2{ -1.0f, 34.0f }))
                uiRequest_.goTo = UIState::Options;
            if (ImGui::Button("Quit",     ImVec2{ -1.0f, 34.0f }))
                uiRequest_.quit = true;
            ImGui::End();
            break;

        case UIState::Options: {
            ImGui::Begin("Options", nullptr, kFlags);

            /* Read from the frame's settings and reported as REQUESTS - the
             * renderer does not own these, Application does. */
            bool ao = mainBuffers_.occlusion().enabled();
            if (ImGui::Checkbox("Ambient occlusion", &ao))
                uiRequest_.setAmbientOcclusion = ao;

            bool baked = view_.settings->useBakedSun;
            if (ImGui::Checkbox("Baked sun (vs shadow map)", &baked))
                uiRequest_.setUseBakedSun = baked;

            bool cutaway = view_.settings->softCutaway;
            if (ImGui::Checkbox("Soft cutaway", &cutaway))
                uiRequest_.setSoftCutaway = cutaway;

            ImGui::Separator();
            if (ImGui::Button("Back", ImVec2{ -1.0f, 30.0f }))
                uiRequest_.goTo = UIState::MainMenu;
            ImGui::End();
            break;
        }

        default:
            break;
    }

    rlImGuiEnd();
    EndDrawing();
}

void FrameRenderer::uploadSteamAvatar(const std::vector<unsigned char>& jpegBytes)
{
    if (jpegBytes.empty()) return;

    /* ".jpg" tells raylib which decoder to use for a buffer that has no
     * filename. raylib only HAS a jpeg decoder because the build defines
     * SUPPORT_FILEFORMAT_JPG - see CMakeLists.txt; without it this returns an
     * empty image and the avatar is silently blank. */
    Image image = LoadImageFromMemory(".jpg", jpegBytes.data(),
                                      static_cast<int>(jpegBytes.size()));
    if (image.data == nullptr) {
        TraceLog(LOG_WARNING, "STEAM: avatar decode failed (%zu bytes)", jpegBytes.size());
        return;
    }

    if (steamAvatar_.id != 0) UnloadTexture(steamAvatar_);
    steamAvatar_ = LoadTextureFromImage(image);
    UnloadImage(image);

    TraceLog(LOG_INFO, "STEAM: avatar uploaded %dx%d", steamAvatar_.width, steamAvatar_.height);
}

}  // namespace game
