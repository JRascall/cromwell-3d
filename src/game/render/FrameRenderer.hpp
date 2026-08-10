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
#include "cromwell/decal/DecalSet.hpp"
#include "cromwell/gpu/target/CustomDepthStencil.hpp"
#include "cromwell/gpu/target/HdrTarget.hpp"
#include "cromwell/gpu/target/DepthTarget.hpp"
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
#include "cromwell/ribbon/RibbonRenderer.hpp"
#include "cromwell/ribbon/RibbonShader.hpp"
#include "game/cli/CliOptions.hpp"
#include "game/light/SunBaker.hpp"
#include "game/render/FrameView.hpp"
#include "game/render/dev/DevView.hpp"
#include "game/render/overlay/BlastFlashes.hpp"
#include "game/render/overlay/Hud.hpp"
#include "game/render/overlay/OverlayRenderer.hpp"
#include "game/render/ribbon/GlowPass.hpp"
#include "game/render/ribbon/RibbonMeshSet.hpp"
#include "game/render/scene/PropSet.hpp"
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

class FrameRenderer {
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

    /* The dev panel is drawn by this class, so its lifetime is too. */
    void setupDevView(int storeys);
    void setDevViewVisible(bool visible);
    void toggleDevView();
    void shutdownDevView();

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
    const RibbonStats& ribbonStats() const { return ribbonStats_; }

    SunLight&           sun() { return sun_; }
    AmbientOcclusion&   ao()  { return ao_; }
    ReflectionProbeSet& probes() { return probes_; }
    DecalSet&           decals() { return decals_; }

#if XC_HAVE_WEB
    WebSurface* webPanel() { return webPanel_.get(); }
    void setWebPanel(std::unique_ptr<WebSurface> panel) { webPanel_ = std::move(panel); }
#endif

private:
    /* ---- passes, all reading view_ -------------------------------------- */
    /* Both take the storey depth EXPLICITLY rather than reading the iso level
     * themselves. The cutaway belongs to the camera, and a pass that reaches
     * for it silently — as the shadow map used to — makes the lighting change
     * when the player changes floor. The lit pass gets the cutaway; the sun
     * and the probes get the whole lattice. */
    void drawGeometry(int maxStorey, const Material& material, bool castersOnly = false);
    void drawGeometryLit(int maxStorey);
    void drawGeometryPrepass();
    void drawOverlays();
    void drawShadowMap();
    void captureEnvironmentProbes();
    void rebuildEnvironmentProbes(const GameState& state);
    HudModel buildHudModel() const;

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

    std::unique_ptr<StaticsMesh>     statics_;
    std::unique_ptr<UnitRenderer>    units_;
    std::unique_ptr<OverlayRenderer> overlays_;
    std::unique_ptr<RibbonShader>    ribbonShader_;
    std::unique_ptr<RibbonMeshSet>   ribbonMeshes_;
    std::unique_ptr<RibbonRenderer>  ribbonRenderer_;
    std::unique_ptr<GlowPass>        glow_;
    std::unique_ptr<DepthTarget>     sceneDepth_;

    DecalSet       decals_;
    DecalBuffer    decalBuffer_;
    DecalRenderer  decalRenderer_;

    BlastFlashes   flashes_;
    Hud            hud_;
    DevView        devView_;
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
    AmbientOcclusion ao_;

    HdrTarget scene_;
    int       sceneWidth_ = 0;
    int       sceneHeight_ = 0;

    /* Position-only material for the shadow map. ALIASES ShadowMap's material
     * — Material::maps is a heap pointer, so this copy shares that array.
     * Read-only here, and it must stay that way. */
    Material depthMaterial_ = { 0 };

    /* The signed-in player's avatar, once fetched and decoded. */
    Texture2D steamAvatar_{};

    RibbonTuning ribbonBuilt_;
    RibbonStats  ribbonStats_;
};

}  // namespace game
