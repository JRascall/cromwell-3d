#include "game/render/FrameRenderer.hpp"

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
            if (options.decalDemo) populateDemoDecals(decals_, state.world());
            else                    registerDemoMaterials(decals_);
        }
    }
    ao_.setEnabled(options.ambientOcclusion);

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

void FrameRenderer::rebuildRibbons(const GameState& state, const RibbonTuning& tuning)
{
    ribbonMeshes_->clear();

    BandExtractor extractor(state.world());
    Band    band;
    LoopSet loops;

    struct RingSpec { float cap; Color colour; Ring ring; int* loopCount; int* edgeCount; };
    const RingSpec specs[2] = {
        { state.moveBudget(),   tuning.moveColour,   Ring::Move,   &ribbonStats_.moveLoops,   &ribbonStats_.moveEdges   },
        { state.sprintBudget(), tuning.sprintColour, Ring::Sprint, &ribbonStats_.sprintLoops, &ribbonStats_.sprintEdges },
    };

    /* BOTH rings are built here; which one displays is a hover decision, not
     * a rebuild — see RingSelector. */
    for (const RingSpec& spec : specs) {
        state.buildBand(spec.cap, band);
        extractor.extract(band, loops);
        *spec.loopCount = loops.loopCount();
        *spec.edgeCount = loops.edgeCount();
        ribbonMeshes_->append(state.world(), loops, spec.colour, spec.ring,
                              tuning.width, tuning.lift);
    }
    ribbonBuilt_ = tuning;
}

/* The render targets that track the window. */
void FrameRenderer::resizeForWindow()
{
    sceneDepth_->resize(GetScreenWidth(), GetScreenHeight());
    glow_->resize(GetScreenWidth(), GetScreenHeight());
    resizeSceneTarget(GetScreenWidth(), GetScreenHeight());
    /* must track the prepass exactly: SSAO samples it by pixel */
    ao_.resize(GetScreenWidth(), GetScreenHeight());
    /* and the DBuffer likewise - it unprojects that depth, so a DBuffer at any
     * other size would be inventing precision the depth does not have */
    if (decalRenderer_.valid())
        decalBuffer_.resize(GetScreenWidth(), GetScreenHeight());
}

void FrameRenderer::drawGeometry(const CutawayView& cutaway, const Material& material,
                                 bool castersOnly)
{
    if (view_.settings.layers.statics) statics_->draw(cutaway, material, castersOnly);
    if (view_.settings.layers.props)   props_.draw(material);
    if (!view_.settings.layers.units)  return;

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
    pbr_.setDebugView(view_.settings.debugView);

    /* HERE RATHER THAN ONCE PER FRAME, so the probe capture shades with the
     * same terms the scene does — this function is what draws both. Switching
     * the sun off and seeing it survive in the reflections would be a switch
     * that half works, which is worse than one that does not. */
    pbr_.setLightingSuppress(view_.settings.effects.suppressMask());
    pbr_.setLightmapEnabled(view_.settings.useBakedSun);
    if (view_.settings.layers.statics)
        statics_->drawLit(cutaway, materials_, pbr_,
                          /*includeTransparent=*/view_.settings.flatShading());

    pbr_.setLightmapEnabled(false);
    if (view_.settings.layers.props) props_.drawLit(materials_, pbr_);

    /* Every body is one material, so its factors go up once rather than per
     * unit; only the albedo tint changes between them, and that travels in the
     * material's diffuse colour. */
    pbr_.setMaterialFactors(materials_.factorsOf(SurfaceKind::Body));
    pbr_.setMaterialOptions(materials_.optionsOf(SurfaceKind::Body));
    pbr_.setMaterialTransmission(
        materials_.transmissionOf(materials_.handleOf(SurfaceKind::Body)));
    const Material& bodyMaterial = materials_.material(SurfaceKind::Body);

    if (view_.settings.layers.units) {
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
    if (view_.settings.flatShading() || !view_.settings.layers.statics) return;

    pbr_.setLightmapEnabled(view_.settings.useBakedSun);

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

    if (view_.settings.showCover) {
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
    model.softCutaway  = view_.settings.softCutaway;
    model.losMode      = view_.state->losMode();
    model.showCover    = view_.settings.showCover;
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
    model.occlusionActive = ao_.active();
    model.bakedSun        = view_.settings.useBakedSun;
    model.debugView       = view_.settings.debugView;
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
    if (!shadows_.valid() || !view_.settings.layers.shadows) return;

    Vector3 centre;
    float   radius = 1.0f;
    shadowFocus(view_.camera, centre, radius);

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

    if (view_.settings.layers.statics)
        statics_->drawPrepass(view_.cutaway, material, materials_, prepass_);

    prepass_.setRoughness(0.8f);
    if (view_.settings.layers.props) props_.draw(material);

    prepass_.setRoughness(materials_.factorsOf(SurfaceKind::Body).x);
    if (view_.settings.layers.units) {
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

    pbr_.setShadowsEnabled(view_.settings.layers.shadows);

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

    const RibbonPassSettings settings = view_.ribbon;

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
    if (probes_.valid() && view_.settings.layers.reflections) {
        if (probesDirty_) {
            rebuildEnvironmentProbes(*view_.state);
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
    if (view_.settings.layers.decals)
        decalRenderer_.render(decals_, settings.camera, decalBuffer_,
                              sceneDepth_->depthTexture(), sceneDepth_->colourTexture(),
                              view_.decalGhost ? &*view_.decalGhost : nullptr);

    /* 2b. THE SILHOUETTE MASK — the units alone, into their own target, each
     *     writing its tint and its depth. Nothing consumes it yet; it exists
     *     so that drawing a soldier through a wall later is one full-screen
     *     shader rather than a pipeline change. Cleared to transparent, so
     *     "nothing here" and "something here" are distinguishable by alpha. */
    if (view_.settings.layers.customDepth && customDepth_.valid()) {
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

    /* 3. AMBIENT OCCLUSION, from that prepass. Two fullscreen passes, no
     *    geometry resubmitted. */
    {
        CW_PROFILE_ZONE_N("ssao");
        CW_GPU_ZONE("ssao");
        ao_.render(settings.camera, sceneDepth_->depthTexture(), sceneDepth_->colourTexture());
    }

    /* 4. THE LIT SCENE, in linear radiance at supersampled resolution. */
    pbr_.updateEnvironment(sun_, shadows_, settings.camera.position);
    pbr_.setSceneSize(static_cast<float>(sceneWidth_), static_cast<float>(sceneHeight_));

    /* After updateEnvironment, which sets the same uniform from whether the
     * map loaded. Skipping the pass is not enough on its own — the last
     * frame's depth would still be bound and still be sampled. */
    pbr_.setShadowsEnabled(view_.settings.layers.shadows);

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
    pbr_.setDecalsEnabled(view_.settings.layers.decals);
    pbr_.bindDecalBuffer(decalBuffer_);
    /* OFF MEANS OFF, not "stop refreshing". Gating only the capture left the
     * cubemaps bound and still sampled, so the layer switch froze the
     * reflections instead of removing them — and a switch that cannot take the
     * probes out of the picture cannot be used to find out whether the probes
     * are responsible for something. */
    if (view_.settings.layers.reflections) pbr_.setEnvironmentProbes(probes_);
    else                     pbr_.clearEnvironmentProbes();
    {
        CW_PROFILE_ZONE_N("lit scene");
        CW_GPU_ZONE("lit scene");
        HdrTarget::Scope scope(scene_);
        ClearBackground(BLANK);

        /* Before BeginMode3D, and so with depth testing — and therefore depth
         * writing — off: the sky lands under the whole frame without needing a
         * far plane or a cube. */
        if (view_.settings.layers.sky) sky_.draw(sun_, settings.camera, sceneWidth_, sceneHeight_);

        BeginMode3D(settings.camera);
        drawGeometryLit(view_.cutaway);

        /* THE PROBE BALLS, and deliberately NOT inside drawGeometryLit: that
         * function is also what the probe capture draws with, so a ball added
         * there would be captured into every cubemap — each probe would see
         * the others as chrome spheres hanging in the room, and then see those
         * reflections reflected. A debug overlay has no business inside the
         * data it is there to inspect. */
        if (view_.settings.debugView == 2)
            probeSpheres_.draw(probes_, sun_, settings.camera.position,
                               sun_.ambientIntensity());

        if (view_.settings.layers.overlays) {
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
    ribbonShader_->setPanSpeed(view_.settings.ribbon.panSpeed);
    glow_->setTuning(view_.settings.ribbon);

    if (view_.settings.layers.ribbons) {
        BeginMode3D(settings.camera);
        ribbonRenderer_->submit(settings, 1.0f,
                                static_cast<float>(sceneDepth_->width()),
                                static_cast<float>(sceneDepth_->height()),
                                sceneDepth_->depthTexture());
        EndMode3D();

        /* the emissive halo: unlit emissive is only half the material, the
         * other half is the bloom that would pick it up. Must come after
         * EndMode3D and before anything 2D over the top, which should not
         * glow. */
        if (view_.settings.layers.glow)
            glow_->render(*ribbonRenderer_, settings, sceneDepth_->depthTexture());
    }

    const DevModel model = buildDevModel();

    /* Last, over everything, and inside BeginDrawing — rlImGui submits its
     * vertices through rlgl like any other 2D draw. The UI is not a layer: it
     * is what turns the layers back on.
     *
     * The exposure round-trip is so ToneMapPass keeps its setter rather than
     * handing out a reference to its own field. */
    float exposure = tonemap_.exposure();
    DevTunables tunables{ sun_, view_.settings.ribbon, ao_.tuning(), exposure, view_.settings.effects };

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
                 previews_.render(slot++, ao_.texture(), Preview::Raw, kTarget),
                 "screen space. white is unoccluded; 1x1 white when off");
    textures.add("g-buffer depth",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->depthTexture() : noTexture,
                                  Preview::Depth, kTarget),
                 "linearised and banded — each band is an equal slice of distance");
    textures.add("g-buffer normal",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->colourTexture() : noTexture,
                                  Preview::Raw, kTarget),
                 "world normal, encoded n * 0.5 + 0.5");
    textures.add("g-buffer roughness",
                 previews_.render(slot++, sceneDepth_ ? sceneDepth_->colourTexture() : noTexture,
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
    decalTool.available   = decalRenderer_.valid() && decalBuffer_.valid();
    decalTool.placedCount = static_cast<int>(decals_.count());
    decalTool.cursorOnSurface = view_.cursorOnSurface;

    const int materials = static_cast<int>(decals_.materialCount());
    decalTool.materialCount =
        (materials < DevDecalTool::kMaxMaterials) ? materials : DevDecalTool::kMaxMaterials;
    for (int i = 0; i < decalTool.materialCount; i++)
        decalTool.materialNames[i] = decals_.materialName(i);

    textures.add("dbuffer albedo",
                 previews_.render(slot++, decalBuffer_.albedo(), Preview::Raw, kTarget),
                 "decal base colour, premultiplied. black is untouched");
    textures.add("dbuffer normal",
                 previews_.render(slot++, decalBuffer_.normal(), Preview::Raw, kTarget),
                 "decal world normal, encoded and premultiplied");
    textures.add("dbuffer coverage",
                 previews_.render(slot++, decalBuffer_.albedo(), Preview::Alpha, kTarget),
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

#if XC_HAVE_WEB
    devView_.draw(model, view_.settings.layers, tunables, textures, decalTool,
                  steamPanel, devRequests_, webPanel_.get());
#else
    devView_.draw(model, view_.settings.layers, tunables, textures, decalTool,
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
            /* No titlebar and no input: the splash is a thing you watch, and
             * Application decides when it is over. */
            ImGui::Begin("##splash", nullptr, kFlags | ImGuiWindowFlags_NoTitleBar |
                                              ImGuiWindowFlags_NoInputs);
            ImGui::Dummy(ImVec2{ 0.0f, 24.0f });
            ImGui::SetWindowFontScale(2.0f);
            ImGui::TextUnformatted("cromwell");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("an XCOM 2-style tactical prototype");
            ImGui::Dummy(ImVec2{ 0.0f, 24.0f });
            ImGui::End();
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
            bool ao = ao_.enabled();
            if (ImGui::Checkbox("Ambient occlusion", &ao))
                uiRequest_.setAmbientOcclusion = ao;

            bool baked = view_.settings.useBakedSun;
            if (ImGui::Checkbox("Baked sun (vs shadow map)", &baked))
                uiRequest_.setUseBakedSun = baked;

            bool cutaway = view_.settings.softCutaway;
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
