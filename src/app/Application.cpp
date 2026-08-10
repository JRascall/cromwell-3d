#include "app/Application.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "core/border/Band.hpp"
#include "core/border/BandExtractor.hpp"
#include "core/border/LoopSet.hpp"
#include "core/light/RoomPartition.hpp"
#include "core/movement/PathReconstructor.hpp"
#include "core/units/Vehicle.hpp"
#include "render/decal/DecalDemo.hpp"
#include "render/gpu/ComputeSelfTest.hpp"
#include "render/gpu/ShaderLibrary.hpp"
#include "render/style/Palette.hpp"
#include "render/ribbon/RibbonConstants.hpp"

#include <algorithm>
#include <cstdio>

namespace xcom {
namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 800;

/* Custom depth stencil values. 0 stays reserved for "nothing was drawn", and
 * units are numbered from 1 upward so a consumer can outline one soldier, or
 * test a range for the whole squad. */
constexpr int kPropStencil      = 200;
constexpr int kFirstUnitStencil = 1;

std::string format(const char* text) { return text ? text : ""; }

}  // namespace

Application::Application(CliOptions options) : options_(std::move(options))
{
    state_.setMoveBudget(options_.moveBudget);
    state_.setIsoLevel(options_.isoLevel);
    state_.setLosMode(options_.losMode);
    state_.selectIndex(options_.selectedUnit);
    debugView_ = options_.debugView % kDebugViewCount;
    if (options_.forceBothRings) rings_.forceBothRings();
}

/* ------------------------------------------------------------ lifecycle */
bool Application::resizeSceneTarget(int windowWidth, int windowHeight)
{
    sceneWidth_  = windowWidth * ToneMapPass::kSupersampleFactor;
    sceneHeight_ = windowHeight * ToneMapPass::kSupersampleFactor;
    return scene_.create(sceneWidth_, sceneHeight_, true);
}

bool Application::initialiseRenderer(int width, int height)
{
    statics_  = std::make_unique<StaticsMesh>();
    units_    = std::make_unique<UnitRenderer>(state_.world());
    overlays_ = std::make_unique<OverlayRenderer>(state_.world(), state_.roster());

    ribbonShader_ = std::make_unique<RibbonShader>();
    if (!ribbonShader_->load()) return false;

    ribbonMeshes_   = std::make_unique<RibbonMeshSet>();
    ribbonRenderer_ = std::make_unique<RibbonRenderer>(*ribbonShader_, *ribbonMeshes_);

    glow_ = std::make_unique<GlowPass>();
    glow_->loadShader();
    glow_->resize(width, height);

    sceneDepth_ = std::make_unique<DepthTarget>(width, height);

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

    /* SSAO reads the normals the prepass writes, so without that shader there
     * is nothing for it to sample and it stays unavailable rather than
     * sampling a colour plane full of whatever the last pass left.
     *
     * DECALS HAVE THE SAME DEPENDENCY and for a stronger reason: the projector
     * pass unprojects the prepass depth to find its receiving surface and reads
     * the prepass normal to reject the ones facing the wrong way. With no
     * prepass there is nothing to project onto, so the buffer is never
     * allocated and DecalBuffer hands out its "no decal" stand-in for the rest
     * of the run. */
    if (prepass_.load()) {
        ao_.load();
        ao_.resize(width, height);

        if (decalRenderer_.load()) {
            decalBuffer_.resize(width, height);

            /* Scaffolding, and the only thing that puts a decal on the board
             * today — see DecalDemo.hpp. Inside the decalRenderer_ guard so a
             * run with no decal shader does not build textures nothing will
             * ever sample. */
            /* Materials always, instances only on request — the dev panel's
             * decal tool has to have something to place even when the scatter
             * is off. See DecalDemo.hpp. */
            if (options_.decalDemo) populateDemoDecals(decals_, state_.world());
            else                    registerDemoMaterials(decals_);
        }
    }
    ao_.setEnabled(options_.ambientOcclusion);

    /* After the material library, because every model registers its own
     * materials there as it loads. */
    props_.loadManifest(materials_);

    /* ONE PROBE PER ROOM, not one per board. A single probe in the middle of
     * the lattice parallax-corrects every surface against the world's bounds,
     * which re-aims a wall's reflection ray from a point on the far side of
     * that wall and returns geometry the wall blocks — the wall reads as
     * transparent. See ReflectionProbeSet.hpp. */
    if (probes_.create()) rebuildEnvironmentProbes();
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
    lightBake_ = std::make_unique<SunBaker>(state_.world(), 64, 8);
    const SunBakeStats bake = lightBake_->bakeAll(currentSun());
    TraceLog(LOG_INFO, "LIGHTMAP: baked %d patches (%d texels) in %.0f ms",
             bake.patches, bake.texels, bake.milliseconds);
    uploadLightmap();

    statics_->rebuild(state_.world());
    probesDirty_ = true;  /* the world the probes captured no longer exists */

#if XC_HAVE_WEB
    /* DEGRADES, NEVER FAILS. A missing libcef.dll or a helper that did not get
     * staged costs the browser panel and nothing else, and the renderer this
     * is bolted onto has to keep working on a machine that has never heard of
     * Chromium. Hence a warning rather than a `return false`. */
    web_ = std::make_unique<WebRuntime>();
    if (web_->start()) {
        /* A starting size only. The browser tab resizes this to whatever it is
         * actually given the moment it opens, because anything else would be
         * scaled to fit and text does not survive that. */
        webPanel_ = std::make_unique<WebSurface>(*web_, 1024, 700,
                                                 "https://www.google.com");
    } else {
        TraceLog(LOG_WARNING, "WEB: disabled - %s", web_->reason().c_str());
        web_.reset();
    }
#endif

    return true;
}

/* The renderer's sun, in the form the baker wants. One source of truth for
 * where the sun is; SunLight owns it and this converts. */
SunSample Application::currentSun() const
{
    const Vector3 travel = sun_.travelDirection();

    SunSample sample;
    sample.directionX = travel.x;
    sample.directionY = travel.y;
    sample.directionZ = travel.z;

    sample.angularRadius = sun_.angularRadius();
    return sample;
}

void Application::uploadLightmap()
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
void Application::rebakeAfterChange(const Cell& centre, float radiusTiles)
{
    if (!lightBake_) return;
    lightBake_->refreshGeometry();
    lightBake_->bakeRegion(currentSun(), centre, radiusTiles);
    uploadLightmap();
}

const Material& Application::prepassMaterial() const
{
    return prepass_.valid() ? prepass_.material() : depthMaterial_;
}

std::string Application::cameraArguments() const
{
    const Camera3D& camera = camera_.camera();

    char buffer[160];
    std::snprintf(buffer, sizeof(buffer),
                  "--cam %.3f %.3f %.3f %.3f %.3f %.3f",
                  static_cast<double>(camera.position.x),
                  static_cast<double>(camera.position.y),
                  static_cast<double>(camera.position.z),
                  static_cast<double>(camera.target.x),
                  static_cast<double>(camera.target.y),
                  static_cast<double>(camera.target.z));
    return buffer;
}

void Application::worldBounds(Vector3& minimum, Vector3& maximum) const
{
    const Lattice& lattice = state_.world().lattice();

    /* A tile of margin on every side: geometry sits inside the grid, but a low
     * sun throws its shadow past the edge, and clipping the projection there
     * would cut those shadows off in mid air. */
    constexpr float kMargin = 1.0f;

    minimum = Vector3{ -kMargin, -kMargin, -kMargin };
    maximum = Vector3{ static_cast<float>(lattice.width()) + kMargin,
                       static_cast<float>(lattice.storeys()) * kStoreyHeight + kMargin,
                       static_cast<float>(lattice.height()) + kMargin };
}

void Application::rebuildRibbons()
{
    ribbonMeshes_->clear();

    BandExtractor extractor(state_.world());
    Band    band;
    LoopSet loops;

    struct RingSpec { float cap; Color colour; Ring ring; int* loopCount; int* edgeCount; };
    const RingSpec specs[2] = {
        { state_.moveBudget(),   ribbon_.moveColour,   Ring::Move,   &moveLoops_,   &moveEdges_   },
        { state_.sprintBudget(), ribbon_.sprintColour, Ring::Sprint, &sprintLoops_, &sprintEdges_ },
    };

    /* BOTH rings are built here; which one displays is a hover decision, not
     * a rebuild — see RingSelector. */
    for (const RingSpec& spec : specs) {
        state_.buildBand(spec.cap, band);
        extractor.extract(band, loops);
        *spec.loopCount = loops.loopCount();
        *spec.edgeCount = loops.edgeCount();
        ribbonMeshes_->append(state_.world(), loops, spec.colour, spec.ring,
                              ribbon_.width, ribbon_.lift);
    }
    ribbonBuilt_ = ribbon_;
}

void Application::rebuildDerivedState()
{
    state_.recompute();
    rebuildRibbons();
    preview_.clear();
    route_.clear();
}

/* ---------------------------------------------------------------- input */
void Application::applyInput(const FrameInput& input)
{
    if (input.setStoreyGround) state_.setIsoLevel(0);
    if (input.setStoreyMiddle) state_.setIsoLevel(1);
    if (input.setStoreyTop)    state_.setIsoLevel(state_.world().lattice().storeys() - 1);

    if (input.cycleRing)     rings_.cycleOverride();
    if (input.toggleCutaway) softCutaway_ = !softCutaway_;
    if (input.toggleCover)   showCover_ = !showCover_;
    if (input.toggleGrenade) grenadeArmed_ = !grenadeArmed_;
    if (input.toggleOcclusion) ao_.setEnabled(!ao_.enabled());
    if (input.toggleBake) useBakedSun_ = !useBakedSun_;
    if (input.toggleFlatView) debugView_ = (debugView_ + 1) % kDebugViewCount;

    if (input.copyCamera) {
        /* The DEBUG VIEW rides along, because reproducing a view means both:
         * an artefact seen in the occlusion pass is not visible in the lit
         * frame, and a camera without the view that showed it is half the
         * information. Paste the whole line after the executable. */
        const std::string arguments =
            cameraArguments() +
            (debugView_ != 0 ? " --debug-view " + std::to_string(debugView_) : "");

        SetClipboardText(arguments.c_str());
        status_ = "camera copied: " + arguments;

        /* In the log as well as on the clipboard: a clipboard survives exactly
         * until the next copy, and a viewpoint worth reporting is usually worth
         * still having after the session that found it. */
        TraceLog(LOG_INFO, "CAMERA: %s", arguments.c_str());
    }

    if (input.toggleLos) {
        state_.setLosMode(!state_.losMode());
        rebuildDerivedState();
    }

    /* 45 degrees a second: fast enough to sweep the whole arc while watching,
     * slow enough to stop on a look. */
    constexpr float kSunDegreesPerSecond = 45.0f;
    if (input.sunAzimuthRate != 0.0f)
        sun_.nudgeAzimuth(input.sunAzimuthRate * kSunDegreesPerSecond * input.deltaSeconds);
    if (input.sunElevationRate != 0.0f)
        sun_.nudgeElevation(input.sunElevationRate * kSunDegreesPerSecond * input.deltaSeconds);

    if (input.resetWorld) {
        state_.reset();
        state_.setMoveBudget(options_.moveBudget);
        flashes_.clear();
        status_.clear();
        statics_->rebuild(state_.world());
    probesDirty_ = true;  /* the world the probes captured no longer exists */
        rebuildDerivedState();
    }
}

/* THE PANEL AND THE KEYBOARD ARE THE SAME INPUT DEVICE.
 *
 * Every debug control produces the request a key would have produced and is
 * applied by applyInput, so there is exactly one implementation of what
 * "toggle cover" means. The two absolute settings — a storey slider, a sun
 * slider — have no keyboard equivalent to borrow, so they land here.
 *
 * The clearing is the other half: while ImGui owns the cursor or the keyboard,
 * the game must not also see those events, or dragging the azimuth slider
 * orbits the camera underneath it. */
FrameInput Application::arbitrate(FrameInput input)
{
    const DevRequests requests = devRequests_;
    devRequests_ = DevRequests{};

    if (devView_.wantsKeyboard()) {
        input.setStoreyGround = input.setStoreyMiddle = input.setStoreyTop = false;
        input.cycleRing = input.toggleCutaway = input.toggleLos = false;
        input.toggleCover = input.toggleGrenade = input.toggleOcclusion = false;
        input.toggleBake = input.toggleFlatView = input.resetWorld = false;
        input.panForward = input.panRight = 0.0f;
        input.sunAzimuthRate = input.sunElevationRate = 0.0f;
    }

    if (devView_.wantsMouse()) {
        input.orbiting     = false;
        input.wheel        = 0.0f;
        input.leftPressed  = false;
        input.leftReleased = false;
    }

#if XC_HAVE_WEB
    /* THE POINTER NEEDS NOTHING HERE. The page lives inside an ImGui window,
     * so any click that reaches it has already made WantCaptureMouse true and
     * the block above has already taken the mouse away from the world.
     *
     * The keyboard does need it, because ImGui cannot see a caret that is
     * inside the page rather than in one of its own fields. Asked of the
     * surface directly rather than cached from the last frame, so it is this
     * frame's answer — and it is false the moment focus leaves the field,
     * which is what keeps an open browser from swallowing the game's hotkeys
     * for as long as it is on screen. */
    if (webPanel_ && webPanel_->wantsKeyboard()) {
        input.setStoreyGround = input.setStoreyMiddle = input.setStoreyTop = false;
        input.cycleRing = input.toggleCutaway = input.toggleLos = false;
        input.toggleCover = input.toggleGrenade = input.toggleOcclusion = false;
        input.toggleBake = input.toggleFlatView = input.resetWorld = false;
        input.panForward = input.panRight = 0.0f;
        input.sunAzimuthRate = input.sunElevationRate = 0.0f;
    }
#endif

    input.toggleCutaway   |= requests.toggleCutaway;
    input.toggleCover     |= requests.toggleCover;
    input.toggleLos       |= requests.toggleLos;
    input.toggleGrenade   |= requests.toggleGrenade;
    input.toggleOcclusion |= requests.toggleOcclusion;
    input.toggleBake      |= requests.toggleBake;
    input.toggleFlatView  |= requests.toggleFlatView;
    input.cycleRing       |= requests.cycleRing;
    input.resetWorld      |= requests.resetWorld;

    if (requests.cyclePreviewProbe && probes_.probeCount() > 0)
        probes_.setPreviewProbe((probes_.previewProbe() + 1) % probes_.probeCount());

    if (requests.isoLevel)     state_.setIsoLevel(*requests.isoLevel);
    if (requests.sunAzimuth)   sun_.setAzimuth(*requests.sunAzimuth);
    if (requests.sunElevation) sun_.setElevation(*requests.sunElevation);

    if (requests.rebakeSun && lightBake_) {
        lightBake_->bakeAll(currentSun());
        uploadLightmap();
    }

    if (requests.clearDecals) decals_.clear();

    /* PRESENT MEANS ARMED, and its absence means disarmed — which is what makes
     * closing the panel cancel the tool rather than leaving a ghost stuck to the
     * cursor with no way to reach the button that turns it off. */
    decalArmed_ = requests.decalBrush.has_value();
    if (decalArmed_) decalBrush_ = *requests.decalBrush;

    /* Width, lift and colour live in the vertices, so the panel moving one is
     * a rebuild rather than a uniform. Here rather than mid-draw, where the
     * slider was actually dragged. */
    if (ribbon_.geometryDiffers(ribbonBuilt_)) rebuildRibbons();

    return input;
}

void Application::updateCamera(const FrameInput& input)
{
    if (input.orbiting) camera_.orbit(input.mouseDelta);

    camera_.pan(input.panForward, input.panRight, input.deltaSeconds, input.panFast,
                state_.world().lattice().width(), state_.world().lattice().height());
    camera_.zoom(input.wheel);

    if (input.windowResized) {
        sceneDepth_->resize(GetScreenWidth(), GetScreenHeight());
        glow_->resize(GetScreenWidth(), GetScreenHeight());
        resizeSceneTarget(GetScreenWidth(), GetScreenHeight());
        /* must track the prepass exactly: SSAO samples it by pixel */
        ao_.resize(GetScreenWidth(), GetScreenHeight());
        /* and the DBuffer likewise — it unprojects that depth, so a DBuffer at
         * any other size would be inventing precision the depth does not have */
        if (decalRenderer_.valid())
            decalBuffer_.resize(GetScreenWidth(), GetScreenHeight());
    }
}

bool Application::canRestAt(int cellIndex) const
{
    const RestPlacement placement(state_.world(), state_.roster());
    return placement.canRest(state_.selectedUnit(), state_.moveGraph(),
                             state_.blockedMask(),
                             state_.world().lattice().cellAt(cellIndex));
}

void Application::buildPreviewFor(std::optional<int> destination)
{
    preview_.clear();
    route_.clear();
    if (!destination) return;

    const Unit& unit = state_.selectedUnit();
    const int start = state_.world().lattice().index(unit.position());
    if (*destination == start) return;
    if (state_.reach().cost(*destination) > state_.sprintBudget()) return;
    if (!canRestAt(*destination)) return;

    route_ = PathReconstructor::reconstruct(state_.reach(), *destination, start);
    if (route_.size() < 2) return;

    PathPreviewBuilder builder(state_.world());
    builder.build(unit, state_.reach(), route_, preview_);
}

void Application::updatePointer(const FrameInput& input)
{
    const Ray ray = GetScreenToWorldRay(input.mousePosition, camera_.camera());
    const TilePicker picker(state_.world());
    const std::optional<int> hit = picker.pick(ray, state_.isoLevel());

    /* THE SURFACE, as opposed to the tile — kept alongside rather than derived
     * from `hovered_`, because they are genuinely different answers. TilePicker
     * reports the standable tile a soldier could walk to and ignores walls
     * entirely; this reports the geometry under the cursor, wall faces
     * included, as a point and a normal. Only the decal tool reads it, and only
     * while the dev panel is up, but it is refreshed every frame regardless:
     * a picker that runs only when a button is pressed cannot grey that button
     * out beforehand, which is the whole difference between "pointing at
     * nothing" and "the tool is broken". */
    cursorSurface_ = SurfacePicker(state_.world()).pick(ray, state_.isoLevel());

    /* After the pick, so the ghost is on the surface the cursor is over THIS
     * frame — a preview one frame behind the mouse reads as lag on the tool. */
    updateDecalPreview();

    if (hit != hovered_) {
        hovered_ = hit;
        if (grenadeArmed_) { preview_.clear(); route_.clear(); }
        else buildPreviewFor(hovered_);
    }

    if (input.leftPressed) pressedAt_ = input.mousePosition;
    if (input.leftReleased && Vector2Distance(pressedAt_, input.mousePosition) < 6.0f)
        handleClick();
}

/* ---- the dev decal tool --------------------------------------------------
 * ARM, PREVIEW, COMMIT. The panel supplies the brush and owns the armed flag;
 * this supplies the world. Nothing in the panel knows what a camera or a mouse
 * ray is, which is the same split every other DevRequest uses.
 *
 * THE PREVIEW IS A REAL DECAL, drawn by the real pass with the real settings —
 * not an outline, not a wireframe box, not a flat quad. That is the entire
 * point: what is hard to predict about a projected decal is exactly the part a
 * cheaper preview would not show you. Whether it wraps cleanly over the kerb
 * it is straddling, whether the angle fade has eaten the half of it that fell
 * on the wall behind, whether it is stretching where the surface turns away —
 * a box outline answers none of those, and they are the only questions worth
 * asking before committing. So the ghost goes through the same projector, the
 * same DBuffer and the same blend as the thing it is predicting, and the only
 * difference between it and a placement is which list it lives in. */
void Application::updateDecalPreview()
{
    decalPreview_.reset();

    if (!decalArmed_ || !cursorSurface_ || !decalRenderer_.valid()) return;
    if (decalBrush_.material < 0 ||
        decalBrush_.material >= static_cast<int>(decals_.materialCount()))
        return;

    /* HOW DEEP THE BOX GOES, and it means two different things depending on the
     * wrap setting — which is why it is chosen here rather than fixed.
     *
     * WRAPPING: depth IS the wrap budget. The unwrap carries the texture by the
     * distance travelled from the placement plane, and the box caps that
     * distance, so a mark meant to climb the wall it is thrown against needs a
     * box tall enough to reach up it. Scaled with the decal's own size, because
     * a bigger mark should run further before it runs out of paper.
     *
     * NOT WRAPPING: depth is only reach, and the two surfaces want different
     * amounts. On the ground a decal should still reach over the kerb or crate
     * standing on it. On a wall there is nothing to reach: the wall is 0.09
     * thick and the only thing a deeper box could find is the room behind or the
     * street in front, neither of which it has any business inking. */
    const float depth = decalBrush_.wrap
                      ? std::max(1.0f, decalBrush_.size)
                      : (cursorSurface_->vertical
                             ? 0.6f
                             : std::max(1.0f, decalBrush_.size * 0.75f));

    Decal decal = Decal::onSurface(cursorSurface_->point, cursorSurface_->normal,
                                   decalBrush_.rotation * DEG2RAD,
                                   Vector2{ decalBrush_.size, decalBrush_.size }, depth);

    decal.material  = static_cast<DecalMaterialId>(decalBrush_.material);
    decal.opacity   = decalBrush_.opacity;
    decal.roughness = decalBrush_.roughness;
    decal.emissive  = decalBrush_.emissive;
    decal.wrap      = decalBrush_.wrap;

    decalPreview_ = decal;
}

void Application::commitDecalPreview()
{
    if (!decalPreview_) return;

    Decal decal = *decalPreview_;

    /* Newest on top, which is what placing another one is expected to mean.
     * Taken from the count rather than a counter of its own so it survives a
     * "clear all" without the order restarting halfway up the stack. */
    decal.sortOrder = static_cast<int>(decals_.count());
    decals_.add(decal);

    /* STAYS ARMED. Placing a second mark next to the first is the common case —
     * comparing two sizes, or laying a row along a wall — and disarming after
     * every commit would mean a trip back to the panel between each one. The
     * button is the way out. */
}

void Application::handleClick()
{
    /* BEFORE THE GRENADE AND BEFORE SELECTION. While a placement tool is armed
     * the click belongs to it and to nothing else — a click that both stuck a
     * decal to a wall and ordered the squad across the map would be the worst of
     * both. Same reason the grenade check sits ahead of selection. */
    if (decalArmed_) {
        commitDecalPreview();
        return;
    }

    if (grenadeArmed_) {
        if (hovered_) detonateAt(state_.world().lattice().cellAt(*hovered_));
        return;
    }

    const Ray ray = GetScreenToWorldRay(GetMousePosition(), camera_.camera());
    UnitPicker unitPicker(state_.world());
    Unit* picked = unitPicker.pick(state_.roster(), ray, state_.isoLevel());

    if (picked && picked->team() == Team::Player && !picked->isDead()) {
        state_.selectUnit(picked);
        rebuildDerivedState();
        status_ = state_.selectedUnit().selectionDescription();
        return;
    }

    if (hovered_ && preview_.size() >= 2) animator_.start(*hovered_);
}

/* ------------------------------------------------------------ animation */
void Application::stepAnimation(float deltaSeconds)
{
    animator_.advance(deltaSeconds, preview_);
    if (!animator_.isFinished(preview_)) return;

    Unit& unit = state_.selectedUnit();

    int crushed = 0;
    if (unit.crushesHalfCover())
        crushed = HullCrusher(state_.world()).crushAlong(route_);

    unit.setPosition(state_.world().lattice().cellAt(animator_.destinationCell()));
    animator_.stop();
    if (crushed) statics_->rebuild(state_.world());

    char buffer[192];
    if (crushed) {
        std::snprintf(buffer, sizeof(buffer),
                      "%s moved to (%d,%d) storey %d  - crushed %d half-cover edge(s)",
                      unit.displayName().c_str(), unit.position().x, unit.position().y,
                      Lattice::storeyOfZ(unit.position().z) + 1, crushed);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s moved to (%d,%d) storey %d",
                      unit.displayName().c_str(), unit.position().x, unit.position().y,
                      Lattice::storeyOfZ(unit.position().z) + 1);
    }
    status_ = buffer;

    rebuildDerivedState();
}

/* ---- grenade: edit the DATA first, then everything re-derives ---------- */
void Application::detonateAt(const Cell& cell)
{
    DestructionSystem destruction(state_.world(), state_.roster());
    const BlastReport report = destruction.detonate(cell);

    /* selection falls back to the soldier if the tank just died */
    if (state_.selectedUnit().isDead()) state_.selectIndex(0);

    flashes_.add(static_cast<float>(cell.x) + 0.5f,
                 Lattice::cellBaseHeight(cell.z) + 0.6f,
                 static_cast<float>(cell.y) + 0.5f);

    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "grenade (%d,%d) storey %d - %d data edits, %d unit(s) killed%s",
                  cell.x, cell.y, Lattice::storeyOfZ(cell.z) + 1,
                  report.dataEdits, report.unitsKilled,
                  report.dataEdits ? "" : "  [nothing destructible in radius]");
    status_ = buffer;

    grenadeArmed_ = false;
    statics_->rebuild(state_.world());   /* the data changed: rebake the world */
    probesDirty_ = true;  /* the world the probes captured no longer exists */
    rebakeAfterChange(cell, DestructionSystem::kBlastRadius);
    rebuildDerivedState();
}

/* --------------------------------------------------------------- drawing */
RibbonPassSettings Application::ribbonSettings() const
{
    RibbonPassSettings settings;
    settings.camera       = camera_.camera();
    settings.visibleRings = rings_.visibleRings(state_.reach(), hovered_, state_.moveBudget());
    settings.solidRing    = rings_.solidRing(state_.reach(), hovered_, state_.moveBudget());
    settings.hideHeight   = softCutaway_
        ? Lattice::storeyBaseHeight(state_.isoLevel()) + kStoreyHeight
        : kHideHeightOff;
    settings.maxStorey    = state_.isoLevel();
    return settings;
}

void Application::drawGeometry(const Material& material, bool castersOnly)
{
    if (layers_.statics) statics_->draw(state_.isoLevel(), material, castersOnly);
    if (layers_.props)   props_.draw(material);
    if (!layers_.units)  return;

    const Unit* animating = animator_.isRunning() ? &state_.selectedUnit() : nullptr;
    units_->drawRoster(state_.roster(), state_.isoLevel(), animating, material);

    if (animating) {
        const PathPoint position = animator_.positionOn(preview_);
        const float offset = UnitRenderer::centreOffset(*animating);
        units_->drawAt(*animating,
                       position.x + (offset > 0.5f ? 0.5f : 0.0f),
                       position.height,
                       position.y + (offset > 0.5f ? 0.5f : 0.0f),
                       material);
    }
}

void Application::drawGeometryLit(int maxStorey)
{
    /* The lattice is baked; everything else is not. Props carry no lightmap
     * UVs and units move, so both stay on the shadow map — which is exactly
     * Source 2's split between lightmapped world and mesh entities. */
    pbr_.setDebugView(debugView_);

    /* HERE RATHER THAN ONCE PER FRAME, so the probe capture shades with the
     * same terms the scene does — this function is what draws both. Switching
     * the sun off and seeing it survive in the reflections would be a switch
     * that half works, which is worse than one that does not. */
    pbr_.setLightingSuppress(effects_.suppressMask());
    pbr_.setLightmapEnabled(useBakedSun_);
    if (layers_.statics)
        statics_->drawLit(maxStorey, materials_, pbr_,
                          /*includeTransparent=*/flatShading());

    pbr_.setLightmapEnabled(false);
    if (layers_.props) props_.drawLit(materials_, pbr_);

    /* Every body is one material, so its factors go up once rather than per
     * unit; only the albedo tint changes between them, and that travels in the
     * material's diffuse colour. */
    pbr_.setMaterialFactors(materials_.factorsOf(SurfaceKind::Body));
    pbr_.setMaterialOptions(materials_.optionsOf(SurfaceKind::Body));
    pbr_.setMaterialTransmission(
        materials_.transmissionOf(materials_.handleOf(SurfaceKind::Body)));
    const Material& bodyMaterial = materials_.material(SurfaceKind::Body);

    if (layers_.units) {
        const Unit* animating = animator_.isRunning() ? &state_.selectedUnit() : nullptr;
        units_->drawRoster(state_.roster(), maxStorey, animating, bodyMaterial);

        if (animating) {
            const PathPoint position = animator_.positionOn(preview_);
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
    if (flatShading() || !layers_.statics) return;

    pbr_.setLightmapEnabled(useBakedSun_);

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
    statics_->drawTransparentLit(maxStorey, materials_, pbr_);
    EndBlendMode();
}

void Application::drawOverlays()
{
    if (state_.losMode()) overlays_->drawVisibility(state_.visibility(), state_.isoLevel());

    if (showCover_) {
        const Unit& selected = state_.selectedUnit();
        if (selected.showsCoverShields()) overlays_->drawCoverShields(selected.position());

        if (hovered_) {
            const Cell hoverCell = state_.world().lattice().cellAt(*hovered_);
            if (hoverCell != selected.position()) overlays_->drawCoverShields(hoverCell);
        }
    }

    if (!animator_.isRunning() && hovered_) {
        const Cell hoverCell = state_.world().lattice().cellAt(*hovered_);
        const bool ok = grenadeArmed_ ||
                        (canRestAt(*hovered_) &&
                         state_.reach().cost(*hovered_) <= state_.sprintBudget());

        const bool wideHull = state_.selectedUnit().footprint().isMultiTile() && !grenadeArmed_;
        const Color colour = grenadeArmed_ ? palette::kHoverGrenade
                           : ok ? palette::kHoverValid : palette::kHoverInvalid;

        overlays_->drawHoverPlate(hoverCell, state_.hoverPlateHeight(hoverCell) + 0.03f,
                                  wideHull ? 1.96f : 0.96f, colour);
    }

    overlays_->drawPathPreview(preview_);
    flashes_.draw();
}

HudModel Application::buildHudModel() const
{
    HudModel model;
    const Unit& selected = state_.selectedUnit();

    model.selectedName = selected.hudLabel();
    model.selectedCell = selected.position();
    model.isoLevel     = state_.isoLevel();
    model.ringOverrideName = rings_.overrideName();
    model.softCutaway  = softCutaway_;
    model.losMode      = state_.losMode();
    model.showCover    = showCover_;
    model.grenadeArmed = grenadeArmed_;

    model.moveLoops   = moveLoops_;
    model.moveEdges   = moveEdges_;
    model.sprintLoops = sprintLoops_;
    model.sprintEdges = sprintEdges_;
    model.visibleRings = rings_.visibleRings(state_.reach(), hovered_, state_.moveBudget());
    model.solidRing    = rings_.solidRing(state_.reach(), hovered_, state_.moveBudget());

    if (hovered_ && !animator_.isRunning()) {
        model.hoverCell = state_.world().lattice().cellAt(*hovered_);
        if (state_.reach().isReachable(*hovered_)) model.hoverCost = state_.reach().cost(*hovered_);
        model.hoverRestOk = canRestAt(*hovered_);
    }

    model.sunAzimuth      = sun_.azimuthDegrees();
    model.sunElevation    = sun_.elevationDegrees();
    model.shadowsActive   = shadows_.valid();
    model.occlusionActive = ao_.active();
    model.bakedSun        = useBakedSun_;
    model.debugView       = debugView_;
    model.probeCount      = probes_.probeCount();
    model.cameraArgs      = cameraArguments();

    model.status = status_;

    /* Out from under the dev toolbar, which is pinned to the top of the
     * viewport and cannot move itself. */
    model.topOffset = static_cast<int>(devView_.toolbarHeight());
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
void Application::shadowFocus(const Camera3D& camera, Vector3& centre, float& radius) const
{
    Vector3 worldMin, worldMax;
    worldBounds(worldMin, worldMax);

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

void Application::drawShadowMap()
{
    if (!shadows_.valid() || !layers_.shadows) return;

    Vector3 centre;
    float   radius = 1.0f;
    shadowFocus(camera_.camera(), centre, radius);

    /* Depth reaches the world's diagonal — enough for any caster to throw into
     * the focus sphere, and tight enough that the bias stays meaningful. */
    Vector3 worldMin, worldMax;
    worldBounds(worldMin, worldMax);
    const float depthExtent = Vector3Length(Vector3Subtract(worldMax, worldMin));

    const SunLight::ShadowProjection projection =
        sun_.shadowProjectionForSphere(centre, radius, depthExtent, ShadowMap::kResolution);


    /* Cut away at the same storey the camera does. A hidden upper floor that
     * still cast would drop a shadow out of an empty sky. */
    ShadowMap::Scope scope(shadows_, projection);
    drawGeometry(depthMaterial_, /*castersOnly=*/true);

    /* Then the glass, into the same target's colour plane. Depth WRITES off so
     * a window never shadows what is behind it, depth TEST on so glass already
     * hidden behind a wall records nothing — light stopped by the wall never
     * reached that window to be tinted. */
    if (shadows_.valid()) {
        rlDisableDepthMask();
        statics_->drawKind(state_.isoLevel(), SurfaceKind::Window,
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
void Application::drawGeometryPrepass()
{
    const Material& material = prepassMaterial();

    if (layers_.statics)
        statics_->drawPrepass(state_.isoLevel(), material, materials_, prepass_);

    prepass_.setRoughness(0.8f);
    if (layers_.props) props_.draw(material);

    prepass_.setRoughness(materials_.factorsOf(SurfaceKind::Body).x);
    if (layers_.units) {
        const Unit* animating = animator_.isRunning() ? &state_.selectedUnit() : nullptr;
        units_->drawRoster(state_.roster(), state_.isoLevel(), animating, material);

        if (animating) {
            const PathPoint position = animator_.positionOn(preview_);
            const float offset = UnitRenderer::centreOffset(*animating);
            units_->drawAt(*animating,
                           position.x + (offset > 0.5f ? 0.5f : 0.0f),
                           position.height,
                           position.y + (offset > 0.5f ? 0.5f : 0.0f),
                           material);
        }
    }
}

void Application::rebuildEnvironmentProbes()
{
    if (!probes_.valid()) return;

    Vector3 minimum, maximum;
    worldBounds(minimum, maximum);

    /* Flooded fresh every time rather than incrementally patched. The flood is
     * one pass over 5184 cells with a queue — cheaper than reasoning about
     * which rooms a detonation could have merged, and correct by construction
     * where the incremental version would be correct by argument. */
    const RoomPartition rooms(state_.world());
    probes_.build(rooms, state_.world().lattice(), minimum, maximum);
}

void Application::captureEnvironmentProbes()
{
    /* A 1x1 white stands in for the occlusion buffer. SSAO is screen space —
     * its texture is addressed by gl_FragCoord against the SCENE's size, and
     * inside a 128-pixel cubemap face those coordinates mean nothing. White is
     * "nothing occluded", which is the right answer to a question that cannot
     * be asked here. */
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1,
                           PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    pbr_.setShadowsEnabled(layers_.shadows);

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
     * reflected image. Restored by drawFrame before the lit pass. */
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
    const int fullDepth = state_.world().lattice().storeys() - 1;

    const int slices = probes_.stale() ? probeFacesPerFrame_ * 4 : probeFacesPerFrame_;
    probes_.capture([this, fullDepth](Vector3 eye) {
        pbr_.updateEnvironment(sun_, shadows_, eye);
        drawGeometryLit(fullDepth);
    }, slices);
}

void Application::drawFrame()
{
    const RibbonPassSettings settings = ribbonSettings();

    /* 1. THE SUN'S VIEW. Depth only, and first, because the lit pass samples
     *    what it writes. */
    drawShadowMap();

    /* 1b. THE REFLECTION PROBES. After the shadow map, so the world reflected
     *     in a window is a shadowed one.
     *
     *     THE REBUILD IS HERE AND NOT AT THE DETONATION. Destruction runs
     *     mid-frame, and re-placing probes underneath a pass that is already
     *     sampling them would swap the volume array out from under a draw
     *     call. Deferring to the top of the next frame costs one frame of a
     *     stale room partition and removes the race entirely. */
    if (probes_.valid() && layers_.reflections) {
        if (probesDirty_) {
            rebuildEnvironmentProbes();
            probesDirty_ = false;
        }
        captureEnvironmentProbes();
    }

    /* 2. THE G-BUFFER — depth, world normals, and roughness in the alpha the
     *    normal write was throwing away. Three customers now: the ribbon
     *    compares its own depth against this to fade where it is buried, SSAO
     *    needs depth and normals, and screen-space reflections need all three.
     *
     *    Overlays are deliberately absent. They are not occluders, and the
     *    hover plate lying a centimetre above the floor must not count as
     *    something for the ribbon to hide behind. */
    {
        DepthTarget::Scope scope(*sceneDepth_);
        ClearBackground(BLANK);
        BeginMode3D(settings.camera);

        /* A G-BUFFER IS WRITTEN, NOT COMPOSITED. Blending has to be OFF here,
         * and it was on, because raylib's default state is alpha blending and
         * nothing in this pass had ever said otherwise.
         *
         * That is ruinous for a buffer whose ALPHA CHANNEL IS ROUGHNESS rather
         * than coverage. Every surface was blended into the target using its
         * own roughness as an opacity: a wall at 0.75 landed as three quarters
         * wall and one quarter whatever had been drawn there before. Statics go
         * down kind by kind with floors and roofs ahead of walls, so what was
         * behind was usually still in the colour buffer — and the G-buffer came
         * out holding a MIXTURE of two surfaces per pixel, showing the room
         * behind a wall straight through it in both the normal and the
         * roughness channel.
         *
         * Everything downstream then inherits it. SSAO orients its sampling
         * hemisphere from that blended normal, so on a wall with geometry
         * behind it the hemisphere tilts toward the wrong surface and the taps
         * report occlusion that belongs to the room beyond — which is why the
         * occlusion buffer prints the shapes of rooms onto flat walls, and why
         * boxes with nothing behind them were unaffected.
         *
         * The depth buffer was never wrong; only the colour attachment was. */
        rlDisableColorBlend();

        drawGeometryPrepass();

        rlEnableColorBlend();
        EndMode3D();
    }

    /* 2a. THE DECALS, into the DBuffer. Here and nowhere else: it needs the
     *     prepass finished, because it unprojects that depth buffer to find the
     *     real surface under each pixel of a projector box, and it must be done
     *     before the lit pass, because what it writes is a MATERIAL and the lit
     *     pass is what turns materials into light.
     *
     *     THAT ORDERING IS THE WHOLE FEATURE. A decal blended into the surface
     *     before shading takes that surface's shadow, its probe, its lightmap
     *     texel and its occlusion for free — so a scorch mark in a doorway is a
     *     shadowed scorch mark, with nothing here knowing what a shadow is. A
     *     decal drawn as its own lit quad afterwards would have to recover all
     *     four, and would still z-fight with the surface it sits on. */
    if (layers_.decals)
        decalRenderer_.render(decals_, settings.camera, decalBuffer_,
                              sceneDepth_->depthTexture(), sceneDepth_->colourTexture(),
                              decalPreview_ ? &*decalPreview_ : nullptr);

    /* 2b. THE SILHOUETTE MASK — the units alone, into their own target, each
     *     writing its tint and its depth. Nothing consumes it yet; it exists
     *     so that drawing a soldier through a wall later is one full-screen
     *     shader rather than a pipeline change. Cleared to transparent, so
     *     "nothing here" and "something here" are distinguishable by alpha. */
    if (layers_.customDepth && customDepth_.valid()) {
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
        const Unit* animating = animator_.isRunning() ? &state_.selectedUnit() : nullptr;
        units_->drawRoster(state_.roster(), state_.isoLevel(), animating,
                           customDepth_.material(),
                           [this, &nextStencil](const Unit&) {
                               customDepth_.setStencil(nextStencil++);
                           });
        if (animating) {
            customDepth_.setStencil(nextStencil++);
            const PathPoint position = animator_.positionOn(preview_);
            const float offset = UnitRenderer::centreOffset(*animating);
            units_->drawAt(*animating,
                           position.x + (offset > 0.5f ? 0.5f : 0.0f),
                           position.height,
                           position.y + (offset > 0.5f ? 0.5f : 0.0f),
                           customDepth_.material());
        }
        EndMode3D();
    }

    /* 3. AMBIENT OCCLUSION, from that prepass. Two fullscreen passes, no
     *    geometry resubmitted. */
    ao_.render(settings.camera, sceneDepth_->depthTexture(), sceneDepth_->colourTexture());

    /* 4. THE LIT SCENE, in linear radiance at supersampled resolution. */
    pbr_.updateEnvironment(sun_, shadows_, settings.camera.position);
    pbr_.setSceneSize(static_cast<float>(sceneWidth_), static_cast<float>(sceneHeight_));

    /* After updateEnvironment, which sets the same uniform from whether the
     * map loaded. Skipping the pass is not enough on its own — the last
     * frame's depth would still be bound and still be sampled. */
    pbr_.setShadowsEnabled(layers_.shadows);

    /* The shadow map and the occlusion buffer belong to the frame, not to any
     * material, so they are bound straight to their own texture units rather
     * than copied into every material's map array. Here rather than earlier
     * because both were only just rendered. */
    pbr_.bindFrameTextures(shadows_.depthTexture(), ao_.texture(),
                           lightmapTexture_, lightIndexTexture_,
                           shadows_.transmissionTexture());

    /* The DBuffer, to its own three units — separately from the frame textures
     * above because it was written later than any of them, and binding a target
     * that a pass is still filling is the kind of thing that works right up
     * until a driver reorders it.
     *
     * OFF MEANS OFF here as well as at the pass. Skipping only the render would
     * leave the last frame's planes bound and still sampled, which freezes the
     * decals rather than removing them — the exact failure the reflection-probe
     * switch had, and the reason that one now clears as well as skips. */
    pbr_.setDecalsEnabled(layers_.decals);
    pbr_.bindDecalBuffer(decalBuffer_);
    /* OFF MEANS OFF, not "stop refreshing". Gating only the capture left the
     * cubemaps bound and still sampled, so the layer switch froze the
     * reflections instead of removing them — and a switch that cannot take the
     * probes out of the picture cannot be used to find out whether the probes
     * are responsible for something. */
    if (layers_.reflections) pbr_.setEnvironmentProbes(probes_);
    else                     pbr_.clearEnvironmentProbes();
    {
        HdrTarget::Scope scope(scene_);
        ClearBackground(BLANK);

        /* Before BeginMode3D, and so with depth testing — and therefore depth
         * writing — off: the sky lands under the whole frame without needing a
         * far plane or a cube. */
        if (layers_.sky) sky_.draw(sun_, settings.camera, sceneWidth_, sceneHeight_);

        BeginMode3D(settings.camera);
        drawGeometryLit(state_.isoLevel());

        /* THE PROBE BALLS, and deliberately NOT inside drawGeometryLit: that
         * function is also what the probe capture draws with, so a ball added
         * there would be captured into every cubemap — each probe would see
         * the others as chrome spheres hanging in the room, and then see those
         * reflections reflected. A debug overlay has no business inside the
         * data it is there to inspect. */
        if (debugView_ == 2)
            probeSpheres_.draw(probes_, sun_, settings.camera.position,
                               sun_.ambientIntensity());

        if (layers_.overlays) {
            OverlayShader::Scope unlit(overlayShader_);
            drawOverlays();
        }
        EndMode3D();
    }

    /* 5. RESOLVE. Past this line everything is display colour on the
     *    backbuffer, which is why the ribbon and its glow did not have to
     *    change to gain a lit world behind them. */
    BeginDrawing();
    ClearBackground(palette::kBackground);
    tonemap_.draw(scene_,
                  static_cast<float>(GetScreenWidth()),
                  static_cast<float>(GetScreenHeight()));

    /* The ribbon's live dials, pushed where the pass that reads them runs. */
    ribbonShader_->setPanSpeed(ribbon_.panSpeed);
    glow_->setTuning(ribbon_);

    if (layers_.ribbons) {
        BeginMode3D(settings.camera);
        ribbonRenderer_->submit(settings, 1.0f,
                                static_cast<float>(sceneDepth_->width()),
                                static_cast<float>(sceneDepth_->height()),
                                sceneDepth_->depthTexture());
        EndMode3D();

        /* the emissive halo: unlit emissive is only half the material, the
         * other half is the bloom that would pick it up. Must come after
         * EndMode3D and before the HUD, which should not glow. */
        if (layers_.glow)
            glow_->render(*ribbonRenderer_, settings, sceneDepth_->depthTexture());
    }

    const HudModel model = buildHudModel();
    if (layers_.hudText) hud_.draw(model);

    /* Last, over everything, and inside BeginDrawing — rlImGui submits its
     * vertices through rlgl like any other 2D draw. The UI is not a layer: it
     * is what turns the layers back on.
     *
     * The exposure round-trip is so ToneMapPass keeps its setter rather than
     * handing out a reference to its own field. */
    float exposure = tonemap_.exposure();
    DevTunables tunables{ sun_, ribbon_, ao_.tuning(), exposure, effects_ };

    /* Every intermediate the frame produced, so it can be LOOKED at rather
     * than reasoned about. Rebuilt per frame because several of these change
     * identity — the lightmap texture is recreated whenever destruction moves
     * the atlas, so a cached handle would go stale exactly when it matters. */
    /* The depth ramp spans the board rather than the far plane: raylib's far
     * plane is 1000 units and the whole lattice is about 34 across, so a ramp
     * scaled to the frustum would render every depth buffer as flat black. */
    {
        Vector3 minimum, maximum;
        worldBounds(minimum, maximum);
        previews_.setDepthRange(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR,
                                Vector3Length(Vector3Subtract(maximum, minimum)) * 1.5f);
    }

    using Preview = TexturePreviews::Mode;
    const Texture2D noTexture{};
    int slot = 0;

    DevTextures textures;
    textures.add("sun depth",
                 previews_.render(slot++, shadows_.depthTexture(), Preview::Raw),
                 "the sun's shadow map, in its own orthographic depth");
    textures.add("sun transmission",
                 previews_.render(slot++, shadows_.transmissionTexture(), Preview::Raw),
                 "sunlight surviving. white is open air, darker is glass");
    textures.add("ambient occlusion",
                 previews_.render(slot++, ao_.texture(), Preview::Raw),
                 "screen space. white is unoccluded; 1x1 white when off");
    textures.add("g-buffer depth",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->depthTexture() : noTexture,
                                  Preview::Depth),
                 "linearised and banded — each band is an equal slice of distance");
    textures.add("g-buffer normal",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->colourTexture() : noTexture,
                                  Preview::Raw),
                 "world normal, encoded n * 0.5 + 0.5");
    textures.add("g-buffer roughness",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->colourTexture() : noTexture,
                                  Preview::Alpha),
                 "the same buffer's alpha. black is a mirror, white is fully diffuse");
    textures.add("lightmap atlas",
                 previews_.render(slot++, lightmapTexture_, Preview::Raw),
                 "baked sun visibility, packed per (cell, face)");
    textures.add("lightmap index",
                 previews_.render(slot++, lightIndexTexture_, Preview::Raw),
                 "(cell, face) -> atlas slot, 16 bit across R and G");
    textures.add("custom stencil",
                 previews_.render(slot++, customDepth_.stencil(), Preview::Stencil),
                 "one hue per object id; dark grey is nothing drawn");
    textures.add("custom depth",
                 previews_.render(slot++, customDepth_.depth(), Preview::Depth),
                 "tagged objects only, to compare against the g-buffer's depth");
    /* A MEMBER BUFFER, NOT TextFormat. DevTextures borrows its name and note
     * pointers and copies nothing, and TextFormat hands back one of four
     * rotating static buffers — four more formatted strings between here and
     * draw() and this label would be describing somebody else's texture. */
    std::snprintf(probePreviewNote_, sizeof(probePreviewNote_),
                  "room %d of %d, +X -X +Y -Y +Z -Z. magenta is open sky. %d slices stale",
                  probes_.probeCount() > 0 ? probes_.previewProbe() + 1 : 0,
                  probes_.probeCount(), probes_.staleFaceCount());
    textures.add("reflection probe",
                 previews_.render(slot++, probes_.previewTexture(), Preview::Raw),
                 probePreviewNote_);

    /* The DBuffer, alongside every other intermediate — and it earns its place
     * more than most. A decal that fails to appear in the lit image has four
     * plausible causes (the projection missed, the angle fade rejected it, the
     * blend is wrong, the lit shader is not reading the planes) and these three
     * pictures separate them at a glance: ink here and nothing on screen is the
     * READ; nothing here is the PASS. */
    DevDecalTool decalTool;
    decalTool.available   = decalRenderer_.valid() && decalBuffer_.valid();
    decalTool.placedCount = static_cast<int>(decals_.count());
    decalTool.cursorOnSurface = cursorSurface_.has_value();

    const int materials = static_cast<int>(decals_.materialCount());
    decalTool.materialCount =
        (materials < DevDecalTool::kMaxMaterials) ? materials : DevDecalTool::kMaxMaterials;
    for (int i = 0; i < decalTool.materialCount; i++)
        decalTool.materialNames[i] = decals_.materialName(i);

    textures.add("dbuffer albedo",
                 previews_.render(slot++, decalBuffer_.albedo(), Preview::Raw),
                 "decal base colour, premultiplied. black is untouched");
    textures.add("dbuffer normal",
                 previews_.render(slot++, decalBuffer_.normal(), Preview::Raw),
                 "decal world normal, encoded and premultiplied");
    textures.add("dbuffer coverage",
                 previews_.render(slot++, decalBuffer_.albedo(), Preview::Alpha),
                 "the SAME plane's alpha, which is 1 - coverage: "
                 "white is no decal, dark is fully inked");

#if XC_HAVE_WEB
    devView_.draw(model, layers_, tunables, textures, decalTool, devRequests_,
                  webPanel_.get());
#else
    devView_.draw(model, layers_, tunables, textures, decalTool, devRequests_);
#endif
    tonemap_.setExposure(exposure);
    EndDrawing();
}

/* -------------------------------------------------------------- the loop */
int Application::run()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | (options_.screenshotPath ? 0 : FLAG_WINDOW_RESIZABLE));
    InitWindow(kWindowWidth, kWindowHeight, "xcom-c - XCOM 2 tile lattice");
    SetTargetFPS(60);

    if (!initialiseRenderer(kWindowWidth, kWindowHeight)) {
        std::fprintf(stderr, "FATAL: could not load assets/shaders - "
                             "run from the project root\n");
        CloseWindow();
        return 1;
    }

    /* The panel stays closed until F1, or until --dev-view asks for it — which
     * is the only way a screenshot run can have it, having no F1 to press. */
    devView_.setup(state_.world().lattice().storeys());
    if (options_.forceDevView) devView_.setVisible(true);

    /* Diagnostic mode: prove the compute path and leave. Before the web test
     * and before the camera, because it depends on nothing but a live GL
     * context and its answer is a precondition for anything built on compute.
     * Exit code follows the result, so a build script can gate on it. */
    if (options_.computeSelfTestPath) {
        const std::string report = runComputeSelfTest(*options_.computeSelfTestPath);
        std::fputs(report.c_str(), stderr);

        const bool passed = computeSelfTestPassed();

#if XC_HAVE_WEB
        webPanel_.reset();
        if (web_) web_->stop();
        web_.reset();
#endif
        devView_.shutdown();
        CloseWindow();
        return passed ? 0 : 1;
    }

#if XC_HAVE_WEB
    /* Diagnostic mode: script the browser, write down what happened, leave.
     * Before the camera and the derived state because none of that matters
     * here — the only thing this run exercises is the web surface. */
    if (options_.webSelfTestPath) {
        if (web_) {
            const std::string report =
                runWebSelfTest(*web_, *options_.webSelfTestPath,
                               options_.webSelfTestUrl ? *options_.webSelfTestUrl
                                                       : std::string(),
                               options_.webSelfTestType ? *options_.webSelfTestType
                                                        : std::string());
            std::fputs(report.c_str(), stderr);
        } else {
            std::fputs("web self test: CEF is not running\n", stderr);
        }
        webPanel_.reset();
        if (web_) web_->stop();
        web_.reset();
        devView_.shutdown();
        CloseWindow();
        return 0;
    }
#endif

    camera_.applyPreset(options_.cameraPreset, options_.freeCamera);
    rebuildDerivedState();
    if (options_.detonateAt) detonateAt(*options_.detonateAt);

    int frames = 0;
    while (!WindowShouldClose()) {
#if XC_HAVE_WEB
        /* Chromium's slice of the frame, first: nothing in the browser
         * advances without it, and everything below wants this frame's page
         * rather than the last one's. OnPaint lands inside this call, on this
         * thread — see WebSurface.hpp on why that matters. */
        if (web_) web_->tick();
#endif

        FrameInput input = input_.sample(options_.mouseX, options_.mouseY);
        if (input.toggleDevView) devView_.toggleVisible();

#if XC_HAVE_WEB
        /* Before the frame that will draw it. The pointer and keyboard are
         * routed later, from inside DevView's browser tab, because that is the
         * only place that knows where the page ended up on screen. */
        if (webPanel_) webPanel_->upload();
#endif

        input = arbitrate(input);

        applyInput(input);
        updateCamera(input);
        flashes_.update(input.deltaSeconds);

        if (animator_.isRunning())          stepAnimation(input.deltaSeconds);
        else if (!input.orbiting && !devView_.wantsMouse()) updatePointer(input);

        drawFrame();

        if (options_.screenshotPath && ++frames >= options_.screenshotFrame) {
            TakeScreenshot(options_.screenshotPath->c_str());
            break;
        }
    }

#if XC_HAVE_WEB
    /* BEFORE CloseWindow, in this order. The surface owns a GL texture, so it
     * has to go while the context is still alive; CefShutdown has to come
     * after the browser it would otherwise be waiting on, and WebRuntime::stop
     * pumps the loop enough times to turn "asked to close" into "closed". */
    webPanel_.reset();
    if (web_) web_->stop();
    web_.reset();
#endif

    devView_.shutdown();
    CloseWindow();
    return 0;
}

}  // namespace xcom

