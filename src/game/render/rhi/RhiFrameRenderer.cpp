#include "game/render/rhi/RhiFrameRenderer.hpp"

#include "cromwell/assets/IImageDecoder.hpp"
#include "cromwell/debug/DebugDraw.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"
#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/platform/IPlatform.hpp"
#include "cromwell/platform/ISurface.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "cromwell/post/ToneMapPass.hpp"
#include "game/lattice/Constants.hpp"
#include "game/light/RoomPartition.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/render/FrameView.hpp"
#include "game/render/Palette.hpp"
#include "game/render/scene/ProbePlacement.hpp"
#include "game/render/dev/DevModel.hpp"

#include "imgui.h"
#include "game/render/scene/RenderFilter.hpp"
#include "game/render/scene/UnitRenderer.hpp"
#include "game/render/ui/GameUi.hpp"
#include "game/render/ui/SplashOverlay.hpp"
#include "game/state/GameState.hpp"
#include "game/world/World.hpp"

#include <cstdio>

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
                                   cromwell::SunLight& sun,
                                   GameUi& ui, cromwell::DecalSet& decals)
    : platform_(platform), sun_(sun), ui_(ui), decals_(decals),
      assets_(platform.device(), platform.files(), platform.images()),
      scene_(platform.device(), assets_.materials()),
      pipeline_(platform.device(), assets_),
      uiPainter_(platform.device()),
      statics_(platform.device()), bodies_(platform.device()),
      overlays_(platform.device()), imgui_(platform.device()),
      previews_(platform.device())
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

    /* THE ONE DEVICE RESOURCE THIS OBJECT HOLDS DIRECTLY. Everything else here
     * belongs to a member that releases its own; the avatar is a bare handle
     * because it has no object worth inventing for one texture. */
    if (steamAvatar_.valid()) platform_.device().destroy(steamAvatar_);
}

void RhiFrameRenderer::drawDevPanel(const FrameView& view)
{
    if (devView_ == nullptr || view.state == nullptr) return;

    /* BROUGHT UP ON FIRST USE, not in the constructor: it loads a shader off
     * disk and reads the live ImGui context, and neither exists that early. A
     * renderer that could not start its panel says so once rather than every
     * frame - the same arrangement drawUi has above. */
    if (!imguiReady_ && !imguiFailed_) {
        if (imgui_.initialise()) imguiReady_ = true;
        else {
            imguiFailed_ = true;
            LOGGER.error("rhi: the dev panel backend could not start - press F1 for nothing");
        }
    }
    if (!imguiReady_) return;

    /* THE PANEL RENDERS ITSELF AND STOPS. rlImGuiEnd would draw it through
     * rlgl, which is the half of rlImGui this path exists to replace. */
    devView_->setDeferredPresent(true);

    const GameState& state = *view.state;

    /* ---- WHAT THIS PATH CAN HONESTLY REPORT ----------------------------
     *
     * Filled from the frame about to be drawn, exactly as
     * FrameRenderer::buildDevModel does for the other renderer - one way, so
     * the panel never queries the game itself.
     *
     * WHAT IS DELIBERATELY LEFT AT ITS DEFAULT is everything describing a
     * system this path has not converted: the ribbon loop and edge counts
     * (§4.6), the baked sun, the decal tool and the texture previews. Those
     * panels show zeros, which is the truthful answer for a subsystem that is
     * not running - inventing numbers would make the panel lie about what the
     * device renderer is doing, which is the one thing a diagnostic may never
     * do. */
    DevModel model;
    model.selectedName = state.selectedUnit().displayName();
    model.selectedCell = state.selectedUnit().position();
    model.isoLevel     = state.isoLevel();
    model.losMode      = state.losMode();
    model.grenadeArmed = view.grenadeArmed;
    model.hoverRestOk  = view.hoverRestOk;
    model.probeCount   = scene_.probes().probeCount();

    if (view.hovered)
        model.hoverCell = state.world().lattice().cellAt(*view.hovered);

    if (view.settings != nullptr) {
        model.showCover = view.settings->showCover;
        model.debugView = view.settings->debugView;
    }

    model.sunAzimuth   = sun_.azimuthDegrees();
    model.sunElevation = sun_.elevationDegrees();

    /* THE PANEL EDITS THE CAMERA'S OWN LAYERS, not a copy: the reflections
     * switch and the debug view are read back out of them next frame. */
    static ViewLayers fallbackLayers;
    ViewLayers& layers = view.camera != nullptr ? view.camera->layers() : fallbackLayers;

    /* WHAT THE CHECKBOXES SHOW, READ BACK FROM WHAT THE PIPELINE ACTUALLY
     * READS. Hardcoding these to true made every toggle display as ON however
     * it was left, so the panel disagreed with the picture - which is worse
     * than a control that does nothing, because it asserts something false. */
    model.shadowsActive   = layers.features.shadows;
    model.occlusionActive = layers.features.ambientOcclusion;

    /* DEFAULTS FOR THE SUBSYSTEMS THIS PATH HAS NOT GOT. Constructed here
     * rather than held as members because nothing reads them back - see the
     * note on devRequests_.
     *
     * THE DECAL TOOL IS STILL ONE OF THEM and stays at its defaults on purpose:
     * decals are §4.6 and this renderer has no DBuffer, so `available` is false
     * and the panel says so rather than offering a button that cannot work.
     * Filling it in would be the panel lying about what the renderer can do. */
    const DevTunables tunables{ sun_, ribbonTuning_, occlusionTuning_, bloomTuning_,
                                exposure_, effects_ };

    /* ---- THE DECAL TOOL, WHICH NOW HAS SOMETHING TO REPORT ---------------
     *
     * It was left at its defaults while there was no DBuffer on this path, and
     * `available = false` was the truthful answer. There is one now, so the
     * panel offers the brush and names the materials the game's set knows.
     *
     * `placedCount` IS THE DEVICE'S NUMBER, NOT THE GAME'S, and the difference
     * is the point: a decal whose material has no albedo map is dropped by the
     * sync, so a tool reporting the game's count would say "12 placed" over a
     * board showing none — the one reading that makes a missing texture look
     * like a broken pass. */
    DevDecalTool decalTool;
    decalTool.available       = pipeline_.decalAlbedo().valid();
    decalTool.placedCount     = rhiDecals_.placedCount();
    decalTool.cursorOnSurface = view.cursorOnSurface;

    const int materials = static_cast<int>(decals_.materialCount());
    decalTool.materialCount =
        (materials < DevDecalTool::kMaxMaterials) ? materials : DevDecalTool::kMaxMaterials;
    for (int i = 0; i < decalTool.materialCount; i++)
        decalTool.materialNames[i] = decals_.materialName(i);

    const DevTextures textures = buildDevTextures();

    /* ---- STEAM, FROM THE FRAME RATHER THAN LEFT EMPTY -------------------
     *
     * The same six fields FrameRenderer copies out of FrameView. It was a
     * default-constructed DevSteam, so the panel reported "not running" on a
     * machine where Steam was running - a diagnostic asserting something false.
     *
     * AND THE PICTURE, WHICH IS NOW OURS RATHER THAN BORROWED. It used to be
     * missing, because `DevSteam::avatar` was a raylib Texture2D and this
     * backend's ImTextureID is an RHI handle — different id spaces, so handing
     * the raylib texture across would have sampled whatever RHI resource
     * happened to share that number and drawn a WRONG PICTURE rather than
     * failing. The panel's ids are backend-neutral now and this renderer
     * decodes and uploads its own copy; see uploadSteamAvatar. */
    DevSteam steamPanel;
    steamPanel.running     = view.steam.running;
    steamPanel.reason      = view.steam.reason;
    steamPanel.persona     = view.steam.persona;
    steamPanel.steamId     = view.steam.steamId;
    steamPanel.avatarState = view.steam.avatarState;
    steamPanel.avatarUrl   = view.steam.avatarUrl;

    steamPanel.avatar       = static_cast<std::uint64_t>(steamAvatar_.id);
    steamPanel.avatarWidth  = steamAvatarWidth_;
    steamPanel.avatarHeight = steamAvatarHeight_;

    devView_->draw(model, layers, tunables, textures, decalTool, steamPanel,
                   devRequests_);

    /* AND THE HALF rlImGui WOULD HAVE DONE. */
    imgui_.render(ImGui::GetDrawData());

    /* ---- AND ACT ON WHAT THE PANEL ASKED FOR ---------------------------
     *
     * The requests a BUTTON raises, as opposed to the switches a checkbox
     * writes directly. Application drains these through takeDevRequests and
     * turns them into input; nothing on this path took them, so every button in
     * the panel was inert while the checkboxes beside them worked - which is
     * exactly the split that looks like a broken panel and is not one.
     *
     * WHAT IS ANSWERED HERE IS EXACTLY WHAT THIS OBJECT OWNS THE STATE FOR, and
     * the line is worth stating because it is the one it would be easy to cross.
     * The occlusion switch is a layer flag on the camera this renderer holds;
     * the preview probe is a slot in the panel this renderer hosts. Sending
     * either up to Application would have it come back down to the same field
     * through code whose other branch drives the raylib renderer's own objects.
     *
     * EVERYTHING THAT IS THE GAME'S STATE IS LEFT ALONE - reset the world,
     * cycle the ring, toggle LOS or cover or the grenade or the cutaway, the
     * bake, the camera requests, the decal tool. Those all have working handlers
     * in Application already; they needed the requests to ARRIVE, not a second
     * implementation here. A renderer that reached into the game to answer them
     * would be two answers to "what does toggle cover mean", discovered later as
     * the panel and the keyboard disagreeing. */
    if (devRequests_.toggleOcclusion) {
        layers.features.ambientOcclusion = !layers.features.ambientOcclusion;
        devRequests_.toggleOcclusion = false;
    }

    /* WRAPPED ON THE PROBE COUNT, so the button is a no-op on a board with no
     * probes rather than selecting a layer that does not exist - which would
     * preview an uninitialised slice of the array and look like a probe that
     * failed to capture. */
    if (devRequests_.cyclePreviewProbe) {
        const int count = scene_.probes().probeCount();
        previewProbe_ = count > 0 ? (previewProbe_ + 1) % count : 0;
        devRequests_.cyclePreviewProbe = false;
    }
}

/* ---- what the texture panel gets, and only while it is being looked at -----
 *
 * EVERY ENTRY IS THIS RENDERER'S OWN INTERMEDIATE. The obvious shortcut - copy
 * the raylib renderer's DevTextures across, since it is still constructed on
 * this path - would put a picture of a buffer THE OTHER RENDERER produced under
 * a label describing this one, on a screen whose entire job is deciding whether
 * this one's buffers are right. It would also not even draw: the ids belong to
 * different backends. See DevTextureView.
 *
 * WHAT IS ABSENT AND WHY. No lightmap atlas or index (the sun bake is not
 * converted), no custom depth or stencil, no DBuffer planes (§4.6), and no
 * second-camera entries (there is one view on this path). Those rows simply do
 * not appear, rather than appearing empty - a row saying "not allocated" is a
 * claim that the buffer exists and is unallocated, which is a different fact. */
DevTextures RhiFrameRenderer::buildDevTextures()
{
    DevTextures textures;

    /* NOTHING IS BLITTED FOR A PANEL NOBODY HAS OPENED, and the check is here
     * rather than inside the previews because this is the only object that
     * knows. It is also why the shaders are not even loaded until the first
     * time F1 is pressed: a --shot run, a self-test and every shipped frame
     * pay nothing at all for a diagnostic they never show. */
    if (devView_ == nullptr || !devView_->visible()) return textures;

    if (!previewsReady_ && !previewsFailed_) {
        if (previews_.initialise()) previewsReady_ = true;
        else                        previewsFailed_ = true;
    }
    if (!previewsReady_) return textures;

    CW_PROFILE_ZONE_N("dev previews");
    CW_GPU_ZONE("dev previews");

    using Mode = cromwell::DeviceTexturePreviews::Mode;

    /* ONE STABLE SLOT PER ENTRY. A slot owns its target across frames, so a
     * number that shifted as rows came and went would recreate render targets
     * every time a probe appeared. Numbered here, once. */
    int slot = 0;

    const auto add = [&](const char* name, cromwell::rhi::TextureHandle source,
                         Mode mode, uint32_t size, const char* note) {
        const cromwell::rhi::TextureHandle preview =
            previews_.render(slot++, source, mode, size, size);

        /* AN INVALID PREVIEW STILL GETS A ROW, with a zero id - the panel says
         * "not allocated", which is the truthful answer for a pass whose target
         * has not been created. Skipping the row would make a missing buffer
         * indistinguishable from a buffer nobody thought to list. */
        textures.add(name, static_cast<std::uint64_t>(preview.id),
                     static_cast<int>(size), static_cast<int>(size), note);
    };

    /* 256 SQUARE FOR EVERY 2D ENTRY. The panel scales what it is given, so this
     * is the preview's own resolution rather than its display size: big enough
     * that a shadow map's silhouettes are readable, small enough that a dozen
     * of them cost nothing. A per-entry size would tie a render target's
     * lifetime to a slider. */
    constexpr uint32_t kPreviewSize = 256;

    /* ---- WHAT `Mode::Depth` HAS TO BE TOLD, and it is the CAMERA's --------
     *
     * The same near and far the view is built with above, and a span sized to
     * the world rather than to the far plane - a thousand units out, where
     * nothing on a tactical board is, would put the whole scene in the first
     * two percent of the ramp. The raylib previews are given the world diagonal
     * times 1.5 and this matches, so the two panels can be read against each
     * other.
     *
     * IT APPLIES TO THE PREPASS DEPTH AND NOT TO THE SHADOW MAP, which is why
     * the shadow map below asks for `Red` instead: the sun's projection is
     * ORTHOGRAPHIC, so its buffer is already a linear ramp across the fitted
     * box and inverting a perspective divide it never had would bend a correct
     * picture. Two buffers, two conventions, and the mode is where that is
     * said. */
    const float diagonal = (boundsMaximum_ - boundsMinimum_).length();
    previews_.withDepthRange(0.1f, 1000.0f, diagonal * 1.5f);

    add("sun depth", pipeline_.shadowMap(), Mode::Red, kPreviewSize,
        "the sun's shadow map, as stored - orthographic, so it is already a "
        "linear ramp. focused on the camera frustum, so it moves as you fly");
    add("sun transmission", pipeline_.shadowTransmission(), Mode::Colour, kPreviewSize,
        "sunlight surviving what it crossed. white is open air, tinted is glass");
    add("ambient occlusion", pipeline_.occlusion(), Mode::Red, kPreviewSize,
        "screen space, after the bilateral blur. white is unoccluded");
    add("g-buffer depth", pipeline_.sceneDepth(), Mode::Depth, kPreviewSize,
        "the prepass, linearised and banded - each band is an equal slice of "
        "distance, near is bright");
    add("g-buffer normal", pipeline_.sceneNormals(), Mode::Colour, kPreviewSize,
        "world normal, encoded n * 0.5 + 0.5");
    add("g-buffer roughness", pipeline_.sceneNormals(), Mode::Alpha, kPreviewSize,
        "the SAME buffer's alpha, which is roughness rather than coverage. "
        "black is a mirror, white is fully diffuse");
    /* ---- THE DBUFFER, AND IT EARNS ITS PLACE MORE THAN MOST -------------
     *
     * A decal that fails to appear in the lit image has four plausible causes —
     * the projection missed, the angle fade rejected it, the blend is wrong, or
     * the lit shader is not reading the planes — and these three pictures
     * separate them at a glance: ink here and nothing on screen is the READ;
     * nothing here is the PASS. */
    add("dbuffer albedo", pipeline_.decalAlbedo(), Mode::Hdr, kPreviewSize,
        "decal base colour, premultiplied and LINEAR on this path. black is "
        "untouched");
    add("dbuffer normal", pipeline_.decalNormal(), Mode::Colour, kPreviewSize,
        "decal world normal, encoded and premultiplied");
    add("dbuffer coverage", pipeline_.decalNormal(), Mode::Alpha, kPreviewSize,
        "the SAME plane's alpha, which is 1 - coverage: white is no decal, "
        "dark is fully inked");

    /* ---- the custom depth buffer, both halves ---------------------------
     *
     * The stencil plane is an ID field rather than a picture — one hue per
     * value, dark where nothing was drawn. Reading it as ordinary colour is
     * what makes "the ids are all the same" and "nothing was tagged" tell
     * themselves apart, which is the whole reason to look. */
    add("custom stencil", pipeline_.customStencil(), Mode::Colour, kPreviewSize,
        "one value per tagged object in R, coverage in A. black is nothing "
        "drawn - unit ids count from 1, so 0 really means untagged");
    add("custom depth", pipeline_.customDepth(), Mode::Depth, kPreviewSize,
        "the tagged objects only, to compare against the g-buffer's depth - "
        "which is what tells a visible outline from an occluded one");

    add("scene colour", pipeline_.sceneColour(), Mode::Hdr, kPreviewSize,
        "linear HDR radiance BEFORE the resolve, range-compressed. not the "
        "frame's tone curve and not its exposure - see the preview shader");

    /* ---- AND THE PROBE STRIP, WHICH IS §4.3'S LEFTOVER ------------------
     *
     * That entry has been waiting on this panel rather than on anything hard:
     * "a cubemap array you cannot look at is worse to debug than a single
     * cubemap, because now there is also the question of WHICH layer is wrong."
     * Six faces and a probe number answer both. */
    cromwell::DeviceProbeSet& probes = scene_.probes();
    const int probeCount = probes.probeCount();

    if (previewProbe_ >= probeCount) previewProbe_ = 0;

    std::snprintf(probePreviewNote_, sizeof(probePreviewNote_),
                  "room %d of %d, +X -X +Y -Y +Z -Z. magenta is open sky. "
                  "%d slices stale",
                  probeCount > 0 ? previewProbe_ + 1 : 0, probeCount,
                  probes.staleFaceCount());

    constexpr uint32_t kProbeFaceSize = 96;
    const cromwell::rhi::TextureHandle strip =
        probeCount > 0 ? previews_.renderCube(slot++, probes.texture(), previewProbe_,
                                              kProbeFaceSize)
                       : cromwell::rhi::TextureHandle{};

    textures.add("reflection probe", static_cast<std::uint64_t>(strip.id),
                 static_cast<int>(kProbeFaceSize) * 6, static_cast<int>(kProbeFaceSize),
                 probePreviewNote_);

    return textures;
}

DevRequests RhiFrameRenderer::takeDevRequests()
{
    const DevRequests taken = devRequests_;
    devRequests_ = DevRequests{};
    return taken;
}

void RhiFrameRenderer::worldChanged()
{
    /* ONE FLAG, AND THE NEXT FRAME DOES THE WORK. Rebuilding here would mean
     * this object holding a world it was not handed - render() is where the
     * world arrives, and it already knows how to build everything from one. */
    staticsBuilt_ = false;
}

void RhiFrameRenderer::uploadSteamAvatar(const std::vector<unsigned char>& jpegBytes)
{
    if (jpegBytes.empty()) return;

    /* THROUGH THE PLATFORM'S DECODER, not raylib's LoadImageFromMemory.
     *
     * Same bytes and, today, the same stb underneath - but IImageDecoder
     * promises RGBA8 tightly packed whatever the file contained, and it checks
     * the HEADER's dimensions before allocating. That matters here more than
     * anywhere else in the process: these bytes came off the network from a URL
     * this process did not choose. See RaylibImageDecoder.hpp, which spells out
     * both halves. */
    cromwell::DecodedImage image;
    const cromwell::ImageDecodeResult result =
        platform_.images().decode(jpegBytes.data(), jpegBytes.size(), image);

    if (result != cromwell::ImageDecodeResult::Ok || image.pixels.empty()) {
        LOGGER.warn("rhi: steam avatar decode failed ({} bytes)", jpegBytes.size());
        return;
    }

    if (steamAvatar_.valid()) platform_.device().destroy(steamAvatar_);
    steamAvatar_ = {};
    steamAvatarWidth_ = 0;
    steamAvatarHeight_ = 0;

    cromwell::rhi::TextureDesc desc;
    desc.name   = "steam avatar";
    desc.width  = static_cast<uint32_t>(image.width);
    desc.height = static_cast<uint32_t>(image.height);
    desc.format = cromwell::rhi::TextureFormat::RGBA8;
    desc.usage  = cromwell::rhi::TextureUsageSampled;

    steamAvatar_ = platform_.device().createTexture(desc);
    if (!steamAvatar_.valid()) {
        LOGGER.error("rhi: could not create a {}x{} avatar texture",
                     image.width, image.height);
        return;
    }

    /* LAYER AND MIP, NOT WIDTH AND HEIGHT. The third and fourth arguments are a
     * SLICE and a LEVEL and all four are uint32_t, so passing the dimensions
     * compiles and uploads a 1x1 sub-image into a level that does not exist:
     * nothing is written, nothing errors, and the texture samples black. It is
     * the trap that cost an hour on this very backend - rhi/MIGRATION.md §5,
     * top entry. The defaults are the right values and are left alone. */
    platform_.device().updateTexture(steamAvatar_, image.pixels.data());

    steamAvatarWidth_  = image.width;
    steamAvatarHeight_ = image.height;

    LOGGER.info("rhi: steam avatar uploaded {}x{}", image.width, image.height);
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
void RhiFrameRenderer::render(const FrameView& view)
{
    CW_PROFILE_ZONE_N("rhi render");

    if (view.state == nullptr) return;

    if (!ready_ && !failed_) {
        /* THE ASSETS FIRST: the pipeline borrows them and the scene borrows
         * their material table, so a pipeline brought up over an uninitialised
         * table would bind buffers that do not exist yet. */
        if (!assets_.initialise())
            LOGGER.error("rhi: the material table could not be built");

        /* AND THE WORLD'S OWN RESOURCES. Not fatal - a device with no cubemap
         * arrays draws every surface against the analytic sky, which is a
         * flatter frame rather than a broken one. */
        if (!scene_.initialise())
            LOGGER.warn("rhi: no reflection probes - surfaces keep the analytic sky");

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
        statics_.rebuild(world, scene_);
        staticsBuilt_ = true;

        /* THE CUBE, ONCE — AND ONCE MEANS ONCE PER PROCESS, NOT PER WORLD. It
         * never changes, so it is built beside the world rather than in
         * initialise(); this is simply the first frame that knows there is
         * anything to draw. A failure here is logged and not fatal: a lit board
         * with no bodies on it is still worth looking at, and it says exactly
         * what went wrong.
         *
         * ITS OWN FLAG, because worldChanged() clears the one above. A reset
         * generates a new building and the same cube draws the same bodies;
         * rebuilding it would destroy a mesh the scene's body renderables still
         * name, which is the mesh lifetime rule RenderScene.hpp states — and
         * on this backend it is a silently ignored draw rather than an error. */
        if (!bodiesBuilt_) {
            if (bodies_.build()) bodiesBuilt_ = true;
            else LOGGER.error("rhi: the unit cube could not be built - "
                              "no bodies will draw");
        }

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
        placeProbes(scene_.probes(), rooms, lattice, boundsMinimum_, boundsMaximum_);

        /* RENDERABLES, NOT DRAWS. What actually gets drawn is whatever each
         * view's frustum keeps, which is a per-pass number only the engine
         * has - and the difference between the two is the point of the
         * change. */
        LOGGER.info("rhi: static world built - {} triangles in {} renderables "
                    "({}-tile chunks)",
                    statics_.triangleCount(), statics_.renderableCount(),
                    RhiStatics::kChunkTiles);
    }

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

    /* ---- THE BODIES, SYNCHRONISED ONCE PER FRAME -----------------------
     *
     * ONCE, not once per pass. Submitting meant walking the roster for the
     * shadow map, the prepass, the lit pass and every probe face, building the
     * same matrices four or five times and throwing them away. The engine culls
     * and draws what this leaves behind. */
    if (roster_ != nullptr && world_ != nullptr)
        bodies_.sync(scene_, *roster_, *world_,
                     animating_, animatedX_, animatedHeight_, animatedY_);

    /* ---- AND THE OVERLAYS, WHICH ARE ONE PLAYER'S AND NOT THE WORLD'S ----
     *
     * kAllViewers because there is one camera. The moment there are two, this
     * is the line that becomes `cromwell::viewerBit(pane)` and the view below
     * gains a matching `withViewer(pane)` - and nothing else in the renderer,
     * the scene or the engine changes. That is what the viewer field is for;
     * see RhiOverlays.hpp. */
    overlays_.sync(scene_, view, cromwell::kAllViewers);

    /* ---- AND THE DECALS, MIRRORED FROM THE GAME'S OWN SET ----------------
     *
     * The ghost goes last so it composites over every committed mark — a
     * preview that did not would be showing you what you already have rather
     * than what you are about to get. See RhiDecals on why this is a converter
     * and not a second authoritative list. */
    rhiDecals_.sync(scene_, decals_, assets_,
                    view.decalGhost ? &*view.decalGhost : nullptr);

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
    /* THE PANEL'S EXPOSURE AND SSAO DIALS, live. Both were constants here and
     * in ScenePipeline, which is 4.10's first debt item and the reason the two
     * renderers' occlusion looked different - see MIGRATION.md 5 on tuning
     * invented rather than borrowed. */
    frame.exposure = exposure_;

    frame.occlusionRadius   = occlusionTuning_.radius;
    frame.occlusionBias     = occlusionTuning_.bias;
    frame.occlusionStrength = occlusionTuning_.strength;

    /* ---- THE RENDERING PANEL'S PER-TERM SWITCHES ------------------------
     *
     * One bool per CONTRIBUTION to a pixel, as opposed to ViewLayers' one per
     * PASS - see RenderEffects.hpp on why that distinction earns its own
     * struct. "Turn the sun off" is this and not a layer: the sun's pass still
     * runs and still fills the shadow map, and what stops is the direct term in
     * the surface shader.
     *
     * A SUPPRESSION MASK, not an enable mask, and the polarity is load-bearing:
     * a uniform that never arrives reads as zero in GLSL, and zero has to mean
     * the ordinary image. With enable bits, any failure to push this would
     * black out the whole scene - a debug switch breaking the render it exists
     * to explain, which RenderEffects.hpp records as having actually happened. */
    frame.effectSuppress = effects_.suppressMask();

    /* ---- AND WHAT THE BLOOM IS AUTHORED WITH ----------------------------
     *
     * Plain values, like the sun's, because SceneFrame crosses into
     * cromwell_base and carries data rather than objects — see its header. The
     * live BloomTuning is this renderer's so the panel has something to point
     * at; these four lines are where a slider reaches the pass.
     *
     * The on/off switch is NOT here: it is a ViewLayers feature, read below
     * with the rest of them, because whether the pass runs at all is a property
     * of the view and its four numbers are authoring. */
    frame.bloomThreshold = bloomTuning_.threshold;
    frame.bloomKnee      = bloomTuning_.knee;
    frame.bloomIntensity = bloomTuning_.intensity;
    frame.bloomRadius    = bloomTuning_.radius;

    /* THE DIAGNOSTIC VIEW THE PLAYER CYCLED WITH F, so one key drives both
     * renderers. `settings` is borrowed and live — see FrameView. */
    if (view.settings != nullptr) frame.debugView = view.settings->debugView;

    /* ---- THE EYE, WHICH IS NOW A VIEW RATHER THAN THREE FRAME FIELDS ----
     *
     * IT NAMES THE SCENE, THE EYE AND WHERE THE PICTURE LANDS. No target and no
     * viewport, so it draws to the backbuffer - a split-screen pane would
     * differ from this in exactly those two fields and nothing else.
     *
     * NO HIDDEN FLAGS AND NO VIEWER YET. Both land as producers convert: the
     * cutaway becomes hidden flags when the statics register themselves, and
     * the viewer bit matters only when there are two players. Until then the
     * defaults are the safe ones - hide nothing, belong to everybody - which is
     * exactly the property View.hpp argues for. */
    cromwell::View sceneView;
    sceneView.withScene(scene_)
             .withKind(cromwell::ViewKind::Camera)

             /* THE CUTAWAY, AS BITS, AND ONLY ON THIS VIEW. The sun's view and
              * every probe face's are derived from this one by the engine and
              * carry no hidden flags at all - so the bug where the camera's
              * storey cut reached the shadow pass and the lighting changed
              * when the player pressed 1 is now unexpressible rather than
              * merely fixed. See RenderFilter.hpp and CutawayView.hpp. */
             .withHiddenFlags(hiddenBy(view.cutaway));

    if (view.camera != nullptr) {
        /* THE DEV PANEL'S REFLECTIONS SWITCH, which lives on the camera's own
         * layers because it is a property of what THIS view draws. It turns off
         * both halves — the captures and the sampling — because a switch that
         * left the captures running would cost the same and answer nothing. See
         * ViewLayers on why a feature toggle that cannot answer "is this the
         * reflections?" is worse than no toggle at all. */
        /* ---- THE DEV PANEL'S LAYER SWITCHES, ALL OF THE ONES THIS PATH HAS
         *
         * It used to be this line alone, so five of the eight checkboxes moved
         * and changed nothing. A switch that cannot answer "is this the
         * shadows?" is worse than no switch - ViewLayers makes that argument
         * about every toggle and the device path was not honouring it. */
        const cromwell::ViewLayers&     cameraLayers = view.camera->layers();
        const cromwell::RenderFeatures& features = cameraLayers.features;

        frame.reflections      = features.reflections;
        frame.shadows          = features.shadows;
        frame.ambientOcclusion = features.ambientOcclusion;
        frame.sky              = features.sky;

        /* THE FILMIC CURVE, OR THE RAW RADIANCE. Off means the resolve blits
         * the linear scene target with the exposure applied and no curve, which
         * is what ViewLayers promises and what a capture feeding a shader wants.
         * It was the last of the eight feature switches this path ignored;
         * `decals` and `customDepth` remain, and honestly so - neither pass
         * exists here, so there is nothing for them to turn off. */
        frame.toneMap = features.toneMap;
        frame.bloom   = features.bloom;

        /* THE DECALS, BOTH HALVES. Off still runs the pass — it clears the
         * planes — because the lit pipeline binds them every frame whatever
         * happens, so skipping would freeze the last frame's ink rather than
         * remove it. See SceneFrame::decals. */
        frame.decals  = features.decals;

        /* ---- THE CUSTOM DEPTH BUFFER AND THE SELECTION OUTLINE -----------
         *
         * The last of the eight feature switches this path did not read. It
         * gates the pass and its one consumer together, because a buffer
         * nobody reads is work nobody can see.
         *
         * WHICH id IS OUTLINED IS THE GAME'S ANSWER, and this is the only line
         * that knows what a stencil value means: RhiBodies numbers each unit
         * from its roster index plus one, so the selected unit's id is its
         * index plus one. The engine is told a number and never learns it is a
         * soldier — the same arrangement as the filter bits. */
        frame.customDepth = features.customDepth;

        frame.outlineStencil = features.customDepth
                             ? view.state->selectedIndex() + 1 : 0;

        /* ---- AND THE GAME'S OWN CATEGORIES ------------------------------
         *
         * ALWAYS-HIDDEN RATHER THAN HIDDEN, which is the whole reason that
         * second word exists on View. A category switched off has to be off in
         * the shadow map too: units hidden from the camera that still lay
         * unit-shaped shadows across the floor answer "not the units" while the
         * units are demonstrably still in the picture, which is worse than no
         * switch at all. ViewLayers.hpp states that requirement and this is
         * where it is met.
         *
         * The cutaway stays on the other word, unchanged, because it is the
         * opposite case - a fact about where the player is standing, which must
         * never reach the sun. See CutawayView.hpp. */
        sceneView.withAlwaysHiddenFlags(hiddenByLayers(cameraLayers.draw));

        int width = 0;
        int height = 0;
        platform_.surface().size(width, height);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                                        : 1.0f;

        sceneView.withEye(view.camera->viewMatrix(),
                          view.camera->projectionMatrix(aspect, 0.1f, 1000.0f),
                          view.camera->position());


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

    pipeline_.render(frame, sceneView);

    /* AFTER THE RESOLVE, and that is the whole reason it is a separate call
     * rather than a pass in the pipeline: everything before the tone map is
     * linear radiance, and the UI is display colour a designer picked. */
    drawUi(view);

    /* AND THE TOOL OVER THE TOP OF ALL OF IT. */
    drawDevPanel(view);

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
        LOGGER.info("rhi: bodies - {} renderables registered", bodies_.renderableCount());
    }

    /* SUBMIT AND SWAP. Only this renderer calls it — the raylib path's
     * EndDrawing already swaps, and doing both would present twice. */
    platform_.endFrame();
}

}  // namespace game
