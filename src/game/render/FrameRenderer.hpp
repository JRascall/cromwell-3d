/* FrameRenderer.hpp — every GPU resource, and the order the passes run in.
 *
 * SINGLE RESPONSIBILITY: own the render targets, shaders and lighting, and
 * sequence one frame: shadow map, depth prepass, occlusion, probe capture,
 * scene, ribbon, overlays, tone map, HUD, dev panel.
 *
 * WHAT IT DOES NOT OWN: the camera rig, the selection, the hovered cell, the
 * path preview, the decal tool's armed state. Those are PlayerController's, and
 * they arrive here as a FrameView. Nothing in this class can move a camera or
 * select a unit, which is the whole point of the split — Application was 1615
 * lines because it did all three jobs and nothing stopped a draw method from
 * quietly reaching into interaction state.
 *
 * THE METHODS CALLED OUTSIDE A FRAME take their world explicitly. Rebuilding
 * the static mesh or re-baking the sun happens in response to a grenade, not
 * during a draw, so there is no FrameView in scope to read — passing it in is
 * what keeps `view_` meaning "the frame currently being drawn" rather than
 * "whatever was last drawn".
 */
#pragma once

#include "cromwell/decal/DecalBuffer.hpp"
#include "cromwell/decal/DecalRenderer.hpp"
#include "cromwell/debug/DebugRenderer.hpp"
#include "cromwell/camera/CameraSet.hpp"
#include "cromwell/camera/SplitScreen.hpp"
#include "cromwell/decal/DecalSet.hpp"
#include "cromwell/gpu/target/CustomDepthStencil.hpp"
#include "cromwell/gpu/target/HdrTarget.hpp"
#include "cromwell/gpu/target/DepthTarget.hpp"
#include "cromwell/gpu/target/ScenePassBuffers.hpp"
#include "cromwell/lighting/PbrShader.hpp"
#include "cromwell/lighting/ProbeSpheres.hpp"
#include "cromwell/lighting/ReflectionProbeSet.hpp"
#include "cromwell/lighting/ShadowMap.hpp"
#include "cromwell/lighting/SunLight.hpp"
#include "cromwell/material/MaterialLibrary.hpp"
#include "cromwell/overlay/OverlayShader.hpp"
#include "cromwell/overlay/TexturePreviews.hpp"
#include "cromwell/post/AmbientOcclusion.hpp"
#include "cromwell/post/PrepassShader.hpp"
#include "cromwell/post/SkyPass.hpp"
#include "cromwell/post/ToneMapPass.hpp"
#include "cromwell/render/ISceneSource.hpp"
#include "cromwell/ribbon/RibbonRenderer.hpp"
#include "cromwell/ribbon/RibbonShader.hpp"
#include "game/cli/CliOptions.hpp"
#include "game/light/SunBaker.hpp"
#include "game/render/FrameView.hpp"
#include "game/render/dev/DevView.hpp"
#include "game/render/ui/GameUi.hpp"
#include "game/render/ui/SplashOverlay.hpp"
#include "game/render/ui/WidgetGallery.hpp"
#include "game/render/overlay/BlastFlashes.hpp"
#include "game/render/overlay/OverlayRenderer.hpp"
#include "game/render/ribbon/GlowPass.hpp"
#include "game/render/ribbon/RibbonMeshSet.hpp"
#include "game/render/scene/PropSet.hpp"
#include "game/render/splash/SplashPass.hpp"
#include "game/render/scene/StaticsMesh.hpp"
#include "game/render/scene/UnitRenderer.hpp"
#include "game/state/GameState.hpp"

#if XC_HAVE_WEB
#include "cromwell/web/surface/WebSurface.hpp"
#endif

#include <memory>
#include <vector>
#include <optional>

namespace game {

using namespace cromwell;

/* DISAMBIGUATING `Camera`. raylib declares a global `Camera` as an alias for
 * Camera3D, and the using-directive above brings cromwell's names in at that
 * same scope — so unqualified `Camera` would be ambiguous everywhere in this
 * namespace.
 *
 * A using-DECLARATION resolves it in favour of the engine's type, which is the
 * one this code means every time: the thing with a position, a lens, layers and
 * optionally a target. raylib's POD is still reachable, spelled Camera3D, which
 * is what the render passes take. See cromwell/camera/Camera.hpp. */
using cromwell::Camera;

/* IMPLEMENTS ISceneSource, which is what lets the engine's passes draw this
 * game's world without naming any of it. The three submit* overrides are
 * private and at the bottom of the class: nothing here calls them directly, the
 * passes reach them through the interface, and keeping them unreachable by name
 * is how they stay honest about using only what arrives on the PassContext.
 *
 * This class is still the whole renderer — the sequence, the targets and the
 * shaders are all below. That is the next step and not this one; see
 * cromwell/render/ISceneSource.hpp for where the line is being drawn. */
class FrameRenderer : public ISceneSource {
public:
    /* How many loops and edges each ring's border came out to. Reported rather
     * than kept private because the dev HUD shows them, and a ribbon that
     * silently built zero loops looks identical to one that was switched off. */
    struct RibbonStats {
        int moveLoops = 0, moveEdges = 0;
        int sprintLoops = 0, sprintEdges = 0;
    };

    bool initialise(int width, int height, const CliOptions& options,
                    const GameState& state);

    /* The render targets that track the window. */
    void resizeForWindow();

    /* ---- invalidation, from outside a frame ---------------------------- */
    void rebuildStatics(const GameState& state);
    void rebuildRibbons(const GameState& state, const RibbonTuning& tuning);

    /* Width, lift and colour live in the VERTICES, so a panel slider moving
     * one is a rebuild rather than a uniform push. Only geometry-affecting
     * dials count: changing emissive every frame must not rebuild a mesh. */
    void rebuildRibbonsIfStale(const GameState& state, const RibbonTuning& tuning);
    void rebakeAfterChange(const GameState& state, const Cell& centre, float radiusTiles);
    void rebakeAll(const GameState& state);
    void markProbesDirty() { probesDirty_ = true; }
    void addBlastFlash(float x, float y, float z) { flashes_.add(x, y, z); }
    void clearFlashes() { flashes_.clear(); }

    /* Decodes fetched avatar bytes and uploads them. Called by Application on
     * the frame the fetch completes - the bytes arrive on a worker, and a GPU
     * upload has to happen here. Replaces any previous avatar. */
    void uploadSteamAvatar(const std::vector<unsigned char>& jpegBytes);

    /* Time-based effects that are the renderer's, stepped once per frame. */
    void updateEffects(float deltaSeconds);

    /* F6 (F5 until the view-target toggle took it). Only the splash
     * participates so far — it is the pass whose whole look lives in its
     * shader, so it is the one where reloading buys the most. Anything else
     * that wants in adds itself here. */
    void reloadShaders() { splash_.reload(); }

    /* ---- second cameras, for any system ---------------------------------
     *
     * THE DOOR INTO THE CameraSet, so a system that wants a camera feed — an
     * ability's overhead snapshot, a UI panel's unit close-up, another
     * player's view in a corner — describes one and holds the handle, without
     * editing this class. Everything added here is drawn by the same scene
     * pass as the built-in plan and feed cameras, on its own schedule, under
     * its own profiler zone, staggered against its neighbours.
     *
     *     CameraDesc desc;
     *     desc.name = "overwatch";  desc.width = desc.height = 256;
     *     desc.schedule = CaptureSchedule::interval(0.25f);
     *     desc.camera = Camera::perspective(70.0f);
     *     desc.camera.at(post).lookingAt(target).withLayers(worldOnly());
     *     CameraId id = renderer.addCamera(std::move(desc));
     *
     * Then, off the handle: `camera(id)->texture()` binds to any shader,
     * `drawTo(rect)` puts it on the HUD, `viewportAt(rect)` makes it
     * clickable, `schedule().request()` forces a redraw the moment something
     * it shows has changed, and CameraDirector::cutTo puts it on the screen
     * itself.
     *
     * addCamera NEEDS A GL CONTEXT — call it after initialise(), any time
     * except from inside the frame's own draw. Returns 0 if the target could
     * not be made; a 0 handle resolves to null everywhere, so the failure is
     * a missing feed rather than a crash.
     *
     * removeCamera AND THE DIRECTOR: a camera being removed must not be the
     * one the screen is showing — cutBack() first. The set cannot know who is
     * looking through its cameras. */
    CameraId      addCamera(CameraDesc&& desc) { return cameras_.add(std::move(desc)); }
    Camera*       camera(CameraId id)          { return cameras_.find(id); }
    const Camera* camera(CameraId id) const    { return cameras_.find(id); }
    void          removeCamera(CameraId id)    { cameras_.remove(id); }

    /* The built-in plan-view capture's camera — the F5 view-target switch
     * looks through it. Null until the first in-game frame builds the
     * captures; callers fall back to the pawn's camera. */
    Camera* planCamera() { return camera(planView_); }

    /* ---- splitscreen -----------------------------------------------------
     *
     * PANES ARE CAMERAS, tiling the window through the same capture path a
     * minimap takes — see cromwell/camera/SplitScreen.hpp for the layouts and
     * the argument. Pane 0 follows the view camera (so the director's cuts
     * still work inside a split); the rest are their own viewpoints, standing
     * in for the other players until there are any. Setting Single releases
     * the pane cameras and the main pass resumes.
     *
     * WHAT A PANE CANNOT SHOW YET: movement ribbons and their glow — both are
     * post-tonemap passes and a capture has no post-resolve phase. Their
     * layer switches are hidden on pane cameras so the panel stays honest. */
    void        setSplitLayout(SplitLayout layout) { split_ = layout; }
    SplitLayout splitLayout() const { return split_; }

    /* GIVES A PANE A REAL CAMERA TO MIRROR — a second player's rig, a replay
     * camera — instead of its corner stand-in. The pane copies the source's
     * pose, lens, projection and layers every frame, exactly as pane 0 does
     * the view camera, so the source stays a plain Camera whoever owns it.
     * BORROWED: the source must outlive the split, or be cleared with null
     * first. Pane 0 cannot be re-sourced — it is the view camera by
     * definition; that is what makes F5's cuts show up in it. */
    void setPaneSource(int pane, Camera* source)
    {
        if (pane >= 1 && pane < 4) paneSources_[pane] = source;
    }

    /* The dev panel is drawn by this class, so its lifetime is too. */
    /* THE ONE DEV PANEL IN THE PROCESS, borrowed by whichever renderer is
     * drawing. There is a single ImGui context and it owns the input state,
     * the open/closed tabs and every slider position, so a second DevView
     * would be a second answer to "is the layers tab open". Same reasoning as
     * GameUi's one painter; see DevView::setDeferredPresent. */
    DevView& devView() { return devView_; }

    void setupDevView(int storeys);
    void setDevViewVisible(bool visible);
    void toggleDevView();
    void shutdownDevView();

    /* The engine's widget kit, on one screen, for looking at (F2). Drawn just
     * under the dev panel: it is a full-screen scrim, and the ImGui panel has
     * to stay reachable over the top of it. */
    void toggleUiGallery();

    /* What a front-end screen asked for this frame. Drained by Application,
     * which owns the state machine and the settings — the renderer draws the
     * buttons but decides nothing about what pressing one means. */
    struct UIRequest {
        std::optional<UIState> goTo;
        bool                   quit = false;
        std::optional<bool>    setAmbientOcclusion;
        std::optional<bool>    setUseBakedSun;
        std::optional<bool>    setSoftCutaway;
    };

    /* ---- the frame ------------------------------------------------------ */
    void render(const FrameView& view);

    UIRequest takeUIRequest();

    /* ---- what Application needs to ask ---------------------------------- */
    DevRequests takeDevRequests();
    bool  decalPassAvailable() const { return decalRenderer_.valid(); }
    bool  devWantsKeyboard() const { return devView_.wantsKeyboard(); }
    bool  devWantsMouse()    const { return devView_.wantsMouse(); }

    /* The widget kit's own answer, which is a DIFFERENT surface from the dev
     * panel: ImGui knows nothing about a HUD button drawn by cromwell's kit, so
     * without this a click on one would be swallowed by the button AND ordered
     * as a move on the world beneath it. See GameUi::wantsMouse. */
    bool  uiWantsMouse()     const { return gameUi_.wantsMouse(); }

    /* THE ONE UI SURFACE, for the renderer being built to paint through. See
     * RhiFrameRenderer's constructor on why there is exactly one. */
    GameUi& ui() { return gameUi_; }
    const RibbonStats& ribbonStats() const { return ribbonStats_; }

    SunLight&           sun() { return sun_; }
    AmbientOcclusion&   ao()  { return mainBuffers_.occlusion(); }
    ReflectionProbeSet& probes() { return probes_; }
    DecalSet&           decals() { return decals_; }

#if XC_HAVE_WEB
    WebSurface* webPanel() { return webPanel_.get(); }
    void setWebPanel(std::unique_ptr<WebSurface> panel) { webPanel_ = std::move(panel); }
#endif

private:
    /* ---- ISceneSource: this game's geometry, for any of the engine's passes -
     *
     * WRITTEN TO USE `pass` AND GAME STATE ONLY. Every one of these could reach
     * for `layers()`, `materials_` or `prepass_` as members — they are members,
     * this class still owns them — and every one of them takes the same things
     * off the context instead. That restraint is the entire test: these three
     * functions are what a second project writes, and if they compile only
     * because they are sitting inside the renderer then the interface is a
     * decoration and the next project finds out the hard way.
     *
     * The one thing they legitimately read from `this` is the game's own
     * per-frame state — view_, statics_, props_, units_ — which is exactly what
     * a project's own implementation would hold. */
    void submitDepth(const PassContext& pass) override;
    void submitLit(const PassContext& pass) override;
    void submitTransparent(const PassContext& pass) override;

    /* HOW MUCH OF THE WORLD THIS PASS MAY DRAW. The one place the engine's
     * "is this a question about the world or about a camera" is turned into
     * this game's answer — see PassContext::worldSpace and CutawayView.hpp.
     * Every submission goes through it, so the sun and the probes cannot
     * acquire the player's cutaway by a call site forgetting. */
    CutawayView cutawayFor(const PassContext& pass) const;

    /* A context with the shared shading state and the pass's identity filled
     * in. The per-pass fields — material, camera, target size — are set by the
     * call site, which is the only thing that knows them. */
    PassContext passFor(PassKind kind, const ViewLayers& layers);

    /* THE ROSTER, PLUS THE ONE BODY THAT IS NOT IN IT.
     *
     * A unit walking a path is drawn at an interpolated position rather than at
     * its logical cell, so the sweep skips it and the caller places it
     * afterwards. Every pass that draws units needs BOTH halves, and four of
     * them used to carry their own copy of the pairing — a pass that remembered
     * the sweep and forgot the placement would drop the moving soldier from
     * that pass alone, which reads as a unit that stops casting a shadow, or
     * stops occluding, for exactly as long as it is walking.
     *
     * `tag` runs immediately before each body, including the placed one, for a
     * pass that needs to say something per object. */
    void submitBodies(const Material& material, int maxStorey,
                      const UnitRenderer::UnitTag& tag = {});

    /* THE WHOLE BOARD FROM ABOVE, into its own texture — the minimap, and the
     * standing proof that rendering from a second camera works.
     *
     * ORTHOGRAPHIC AND TOP-DOWN, framed to the lattice. On an interval rather
     * than every frame, because a second camera is a second scene pass and a
     * map of a board nobody is moving does not need sixty of them a second. See
     * cromwell/camera/Camera.hpp, and CaptureSchedule.hpp for why the
     * schedule is stated rather than defaulted.
     *
     * Must be called BEFORE the main pass sets up its environment. The comment
     * at the call site says why. */
    void captureOverview(float deltaSeconds);

    /* One camera's scene pass. Shared by every camera that renders to a texture
     * — what differs between them travels ON the camera, so a new one is data
     * rather than another branch in here.
     *
     * `buffers` null means no depth prepass of its own, and therefore no
     * occlusion and no decals from this camera — see the note at the top of the
     * function, and Camera::hasScreenSpaceEffects for what allocates them. */
    void drawCameraScene(Camera& camera, Camera::ScenePhase phase, float width, float height,
                         const CutawayView& cutaway);

    /* Puts the orthographic capture on screen. Separate from making it, because
     * where a picture goes is a HUD decision and the camera that took it should
     * have no opinion about screen corners. */
    void drawMinimap();

    /* THE ONE SCENE FUNCTION — every camera's picture, the main view
     * included, comes out of this. `rig` is the viewpoint, `requested` the
     * layers it asked for, `buffers` its screen-space set (gated to what the
     * buffers can actually serve, the way captures always were). The main
     * view calls it with mainBuffers_ and the backbuffer as its output; a
     * capture calls it through drawCameraScene with its own. Adding a pass
     * here adds it to EVERY camera, gated by that camera's switches — which
     * is the whole point. */
    void drawSceneForView(const Camera3D& rig, const ViewLayers& requested,
                          ScenePassBuffers* buffers, Camera::ScenePhase phase, float width,
                          float height, const CutawayView& cutaway);

    /* ---- splitscreen internals -----------------------------------------
     * Creates, sizes and aims the pane cameras for the current layout; called
     * once per split frame, before the captures render. Idempotent — pane
     * targets resize themselves only when the layout or the window changed. */
    void syncSplitPanes();

    /* Frees the pane cameras. Called when the layout returns to Single, and
     * cheap to call when there is nothing to free. */
    void releaseSplitPanes();

    /* Composites the pane textures onto the backbuffer, in the resolve slot
     * the fullscreen tonemap normally occupies. */
    void drawSplitScreen();
    void drawOverlays();
    void drawShadowMap();
    void captureEnvironmentProbes();
    void rebuildEnvironmentProbes(const GameState& state);
    DevModel buildDevModel() const;

    /* The screens that are NOT the game. Drawn instead of the world, not over
     * it: outside InGame there is no camera worth pointing anywhere and every
     * pass below the UI would be shading an empty board. */
    void drawFrontEnd();

    UIRequest uiRequest_;

    bool resizeSceneTarget(int windowWidth, int windowHeight);
    const Material& prepassMaterial() const;
    void worldBounds(const GameState& state, Vector3& minimum, Vector3& maximum) const;
    void shadowFocus(const Camera3D& camera, Vector3& centre, float& radius) const;
    void uploadLightmap();
    SunSample currentSun() const;

    /* The frame currently being drawn. Set at the top of render() and read by
     * every pass below it; meaningless outside one. */
    FrameView view_;

    /* THE LAYERS OF WHICHEVER CAMERA IS BEING DRAWN FOR — the player's for the
     * main frame, a capture's for the duration of its drawCameraScene. Every
     * layer test in the passes reads layers(); pointing this somewhere else is
     * how a capture's switches are honoured by passes that were never told
     * captures exist.
     *
     * A POINTER SWAPPED, NOT A VALUE COPIED OVER SOMEONE ELSE'S SETTINGS. The
     * previous arrangement wrote a capture's layers into the game's live
     * ViewSettings and restored them afterwards — correct, but it meant the
     * engine's per-camera settings only worked by temporarily impersonating
     * the player's. Now every camera's layers stay its own and this slot says
     * whose turn it is. */
    const ViewLayers* activeLayers_ = nullptr;
    const ViewLayers& layers() const { return *activeLayers_; }

    std::unique_ptr<StaticsMesh>     statics_;
    std::unique_ptr<UnitRenderer>    units_;
    std::unique_ptr<OverlayRenderer> overlays_;
    std::unique_ptr<RibbonShader>    ribbonShader_;
    std::unique_ptr<RibbonMeshSet>   ribbonMeshes_;
    std::unique_ptr<RibbonRenderer>  ribbonRenderer_;
    std::unique_ptr<GlowPass>        glow_;

    /* THE MAIN VIEW'S SCREEN-SPACE BUFFERS — prepass, occlusion, DBuffer — as
     * ONE ScenePassBuffers, the same type every capture owns. This used to be
     * three loose members (sceneDepth_, ao_, decalBuffer_) that only the main
     * path knew how to hold together, which is exactly why the main path had
     * its own copy of the scene code. One type, one resize, one function:
     * the main view is a camera whose output is the backbuffer, and
     * drawSceneForView cannot tell it apart from a capture. Window-sized,
     * unlike a capture's (which match its supersampled HDR target) — the
     * main view's established resolution policy, kept. */
    ScenePassBuffers mainBuffers_;

    DecalSet       decals_;
    DecalRenderer  decalRenderer_;

    BlastFlashes   flashes_;
    DevView        devView_;

    /* ONE UI surface for the whole game — the splash's loading bar and the
     * widget gallery both draw into it. Two would mean two font atlas caches
     * and, worse, two halves of the per-widget hover state. See GameUi.hpp. */
    GameUi         gameUi_;

    /* Draws whatever any system queued into the process-wide DebugDraw. Owned
     * here because it is a pass among the scene's others; the QUEUE it reads is
     * global and headless, so a pathfinder can fill it without knowing this
     * class exists. See cromwell/debug/DebugDraw.hpp. */
    DebugRenderer  debugRenderer_;

    /* EVERY SECOND CAMERA IN THE FRAME, in one collection. Adding another is a
     * descriptor and a member to hold its handle — not another target, another
     * lambda, another profiler name and another phase to get right. See
     * cromwell/camera/CameraSet.hpp.
     *
     * Populated on first use, because the captures need a GL context the
     * constructor does not have. */
    CameraSet      cameras_;

    /* Handles into it. The set owns the captures; these are how anything
     * addresses one afterwards — to move it, re-layer it, or read its texture. */
    CameraId       planView_ = 0;
    CameraId       cctvView_ = 0;

    /* The splitscreen panes, when a layout is active — handles into the same
     * set, so they appear in the dev panel's texture previews and the
     * profiler like any other camera. Zero means "no pane". */
    SplitLayout    split_ = SplitLayout::Single;
    CameraId       panes_[4] = { 0, 0, 0, 0 };

    /* Cameras the panes mirror, when the game has given them one — see
     * setPaneSource. Borrowed, never owned; null means the corner stand-in. */
    Camera*        paneSources_[4] = { nullptr, nullptr, nullptr, nullptr };

    /* --minimap-realtime: the plan view captures every frame rather than on
     * its interval. Stored at initialise because the cameras are created
     * lazily, on the first frame that has a GL context. */
    bool           minimapRealtime_ = false;
    WidgetGallery  uiGallery_;
    DevRequests    devRequests_;

#if XC_HAVE_WEB
    std::unique_ptr<WebSurface> webPanel_;
#endif

    SunLight           sun_;
    ShadowMap          shadows_;
    ReflectionProbeSet probes_;
    ProbeSpheres       probeSpheres_;
    CustomDepthStencil customDepth_;
    TexturePreviews    previews_;

    bool probesDirty_ = true;
    int  probeFacesPerFrame_ = 1;
    char probePreviewNote_[128] = "";

    PbrShader       pbr_;
    MaterialLibrary materials_;
    PropSet         props_;

    std::unique_ptr<SunBaker> lightBake_;
    Texture2D                 lightmapTexture_{};
    Texture2D                 lightIndexTexture_{};

    SkyPass          sky_;
    ToneMapPass      tonemap_;
    OverlayShader    overlayShader_;
    PrepassShader    prepass_;

    HdrTarget scene_;
    int       sceneWidth_ = 0;
    int       sceneHeight_ = 0;

    /* Position-only material for the shadow map. ALIASES ShadowMap's material
     * — Material::maps is a heap pointer, so this copy shares that array.
     * Read-only here, and it must stay that way. */
    Material depthMaterial_ = { 0 };

    /* The signed-in player's avatar, once fetched and decoded. */
    Texture2D steamAvatar_{};

    /* The animated splash backdrop. Loads itself on first draw and reports
     * whether there is an image at all — see SplashPass.hpp for why its
     * absence is an ordinary outcome rather than a failure. */
    SplashPass splash_;

    RibbonTuning ribbonBuilt_;
    RibbonStats  ribbonStats_;
};

}  // namespace game
