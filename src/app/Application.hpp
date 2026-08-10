/* Application.hpp — the frame loop and the wiring.
 *
 * SINGLE RESPONSIBILITY: own the pieces and sequence them. Every decision it
 * makes is about ORDER — sample input, step the sim, draw the depth prepass,
 * draw the scene, draw the HUD. Nothing about how any of those work lives
 * here, which is what the old 590-line main.c could not say.
 */
#pragma once

#include "app/cli/CliOptions.hpp"
#include "app/rules/DestructionSystem.hpp"
#include "app/state/GameState.hpp"
#include "app/rules/HullCrusher.hpp"
#include "app/input/InputHandler.hpp"
#include "app/path/MoveAnimator.hpp"
#include "app/camera/OrbitCamera.hpp"
#include "app/path/PathPreviewBuilder.hpp"
#include "app/rules/RestPlacement.hpp"
#include "app/state/RingSelector.hpp"
#include "app/picking/SurfacePicker.hpp"
#include "app/picking/TilePicker.hpp"
#include "app/picking/UnitPicker.hpp"
#include "render/overlay/BlastFlashes.hpp"
#include "render/overlay/DevView.hpp"
#include "render/overlay/RenderEffects.hpp"
#include "render/overlay/ViewLayers.hpp"
#include "render/decal/DecalBuffer.hpp"
#include "render/decal/DecalRenderer.hpp"
#include "render/decal/DecalSet.hpp"
#include "render/gpu/DepthTarget.hpp"
#include "render/gpu/HdrTarget.hpp"
#include "render/overlay/Hud.hpp"
#include "render/overlay/OverlayRenderer.hpp"
#include "render/overlay/OverlayShader.hpp"
#include "render/gpu/CustomDepthStencil.hpp"
#include "render/overlay/TexturePreviews.hpp"
#include "render/lighting/ProbeSpheres.hpp"
#include "render/lighting/ReflectionProbeSet.hpp"
#include "render/lighting/PbrShader.hpp"
#include "render/lighting/ShadowMap.hpp"
#include "render/lighting/SunLight.hpp"
#include "core/light/SunBaker.hpp"
#include "render/material/MaterialLibrary.hpp"
#include "render/model/PropSet.hpp"
#include "render/post/AmbientOcclusion.hpp"
#include "render/post/PrepassShader.hpp"
#include "render/post/SkyPass.hpp"
#include "render/post/ToneMapPass.hpp"
#include "render/world/StaticsMesh.hpp"
#include "render/units/UnitRenderer.hpp"
#include "render/ribbon/GlowPass.hpp"
#include "render/ribbon/RibbonMeshSet.hpp"
#include "render/ribbon/RibbonRenderer.hpp"
#include "render/ribbon/RibbonShader.hpp"
#include "render/ribbon/RibbonTuning.hpp"
#if XC_HAVE_WEB
#include "render/web/WebRuntime.hpp"
#include "render/web/WebSelfTest.hpp"
#include "render/web/WebSurface.hpp"
#endif

#include <memory>
#include <optional>

namespace xcom {

class Application {
public:
    explicit Application(CliOptions options);

    /* Opens the window, runs the loop, returns the process exit code. */
    int run();

private:
    /* ---- lifecycle ---------------------------------------------------- */
    bool initialiseRenderer(int width, int height);
    void rebuildDerivedState();      /* reach + ribbons, after any data edit */
    void rebuildRibbons();

    /* ---- per frame ---------------------------------------------------- */
    void applyInput(const FrameInput& input);

    /* Folds the debug panel's clicks into the same FrameInput the keyboard
     * produced, and blanks whatever the panel is currently swallowing — a drag
     * on a slider must not also orbit the camera. */
    FrameInput arbitrate(FrameInput input);

    void updateCamera(const FrameInput& input);
    void updatePointer(const FrameInput& input);
    void handleClick();
    void stepAnimation(float deltaSeconds);

    /* The world and the bodies in it, submitted through whichever shader the
     * running pass wants. Called three times a frame — shadow, depth prepass,
     * lit — which is exactly why the material is an argument. */
    void drawGeometry(const Material& material, bool castersOnly = false);

    /* The same geometry, but each sub-mesh through its own material. Only the
     * lit pass wants this; the shadow and prepass passes want one shader over
     * everything, which is what drawGeometry is for. */
    /* `maxStorey` is the cutaway. It is the player's isoLevel for the scene
     * and the FULL lattice for a probe capture — a reflection has to reflect
     * the building that exists, not the one the camera is currently allowed to
     * see through. Capturing under the cutaway removes the ceiling and the
     * upper walls from every interior probe, which puts the outdoors into an
     * indoor cubemap and looks exactly like the leak this system fixes. */
    void drawGeometryLit(int maxStorey);

    /* The G-buffer pass: the same geometry through the prepass shader, with
     * each material's roughness pushed so the buffer's alpha is meaningful. */
    void drawGeometryPrepass();

    /* The gameplay markers: LOS tint, cover shields, hover plate, path line,
     * blast flashes. Unlit by intent, and drawn through OverlayShader so their
     * authored sRGB lands correctly in the linear target. */
    void drawOverlays();

    void drawShadowMap();

    /* Renders the world into one (probe, face) slice of the reflection probe
     * array per frame, round-robin. One extra scene render a frame regardless
     * of how many rooms the map has — see ReflectionProbeSet::capture. */
    void captureEnvironmentProbes();

    /* Re-floods the world into rooms and re-places a probe in each, then marks
     * every layer stale. Called wherever the static mesh is rebuilt: blowing a
     * wall open genuinely merges two rooms, and a probe list from before that
     * describes a building which no longer exists. */
    void rebuildEnvironmentProbes();
    void drawFrame();
    HudModel buildHudModel() const;

    /* The current camera as a ready-to-paste `--cam px py pz tx ty tz`.
     *
     * Three decimals, which is finer than anybody can position by hand and is
     * the point: this exists so a view can be handed to somebody else exactly,
     * rather than described. Reproducing an angle-dependent artefact from a
     * prose description is most of the cost of investigating one. */
    std::string cameraArguments() const;

    /* The lattice's world-space extent, which is what the sun's orthographic
     * shadow box is fitted to. */
    void worldBounds(Vector3& minimum, Vector3& maximum) const;

    /* The sphere the sun's shadow map is fitted to: the camera's frustum
     * clipped to the lattice, so texels are spent where the player is
     * looking. */
    void shadowFocus(const Camera3D& camera, Vector3& centre, float& radius) const;

    /* Allocates the scene target at the supersampled size for this window. */
    bool resizeSceneTarget(int windowWidth, int windowHeight);

    /* What the scene prepass is drawn with — depth AND world normals when the
     * prepass shader loaded, plain depth otherwise. The ribbon only needs the
     * depth either way; losing the normals just costs SSAO. */
    const Material& prepassMaterial() const;

    void buildPreviewFor(std::optional<int> destination);
    void detonateAt(const Cell& cell);

    /* The dev panel's decal tool: arm, preview, commit. The panel supplies the
     * brush and owns the armed flag; these supply the world. */
    void updateDecalPreview();
    void commitDecalPreview();
    bool canRestAt(int cellIndex) const;

    RibbonPassSettings ribbonSettings() const;

    /* ---- state -------------------------------------------------------- */
    CliOptions options_;
    GameState  state_;

    OrbitCamera  camera_;
    InputHandler input_;
    RingSelector rings_;
    MoveAnimator animator_;

    /* renderers are constructed after the window exists */
    std::unique_ptr<StaticsMesh>     statics_;
    std::unique_ptr<UnitRenderer>    units_;
    std::unique_ptr<OverlayRenderer> overlays_;
    std::unique_ptr<RibbonShader>    ribbonShader_;
    std::unique_ptr<RibbonMeshSet>   ribbonMeshes_;
    std::unique_ptr<RibbonRenderer>  ribbonRenderer_;
    std::unique_ptr<GlowPass>        glow_;
    std::unique_ptr<DepthTarget>     sceneDepth_;

    /* Decals. The set is the board's, the buffer and the renderer are the
     * frame's — and the buffer tracks sceneDepth_'s size exactly, because the
     * pass unprojects that depth to find the surfaces it inks. */
    DecalSet                         decals_;
    DecalBuffer                      decalBuffer_;
    DecalRenderer                    decalRenderer_;

    /* What the cursor is over, as geometry rather than as a tile — see
     * SurfacePicker for why that is a separate question from hovered_. */
    std::optional<SurfaceHit>        cursorSurface_;

    /* The decal tool's armed state and its live brush, mirrored from the panel
     * every frame (present-means-armed; see DevRequests::decalBrush). */
    bool                             decalArmed_ = false;
    DevRequests::DecalPlacement      decalBrush_;

    /* The ghost under the cursor. A REAL Decal, submitted by the real pass after
     * the committed ones so it reads on top — the only thing separating it from
     * a placement is that it lives here instead of in decals_, and is rebuilt
     * from scratch every frame. */
    std::optional<Decal>             decalPreview_;

    BlastFlashes                     flashes_;
    Hud                              hud_;
    DevView                          devView_;

#if XC_HAVE_WEB
    /* CEF's lifetime is the process's, so the runtime outlives the surface and
     * is declared before it — members are destroyed in reverse, and a browser
     * torn down after CefShutdown is an access violation.
     *
     * The surface is drawn and driven entirely by DevView's browser tab; the
     * only things Application does with it are create it, give Chromium a
     * slice of each frame, upload the paint, and ask whether the page is
     * currently holding the keyboard. */
    std::unique_ptr<WebRuntime>  web_;
    std::unique_ptr<WebSurface>  webPanel_;
#endif

    /* What the dev panel asked for last frame, applied at the top of this one
     * alongside the keyboard's requests. */
    DevRequests                      devRequests_;

    /* Which passes are submitted. Edited by the panel, read by drawFrame. */
    ViewLayers                       layers_;

    /* Which lighting TERMS contribute, as opposed to which passes run. Edited
     * by the rendering panel, pushed to the surface shader as one mask. */
    RenderEffects                    effects_;

    /* The ribbon's live numbers, and the copy the strips were last built from
     * — width, lift and colour are baked into vertices, so a change there has
     * to be noticed rather than merely pushed. */
    RibbonTuning                     ribbon_;
    RibbonTuning                     ribbonBuilt_;

    /* ---- lighting ----------------------------------------------------- */
    SunLight           sun_;
    ShadowMap          shadows_;
    ReflectionProbeSet probes_;

    /* Debug view 2 only: a chrome ball at each probe's capture point. */
    ProbeSpheres       probeSpheres_;

    /* Unreal's custom depth/stencil, ported: tagged geometry rasterised apart
     * from the scene, each object carrying an arbitrary 0-255 value and its own
     * depth. See CustomDepthStencil.hpp — the pipeline half only; nothing reads
     * it yet, and outlines are one full-screen shader away from it. */
    CustomDepthStencil customDepth_;

    /* Remaps the renderer's internal buffers into something the inspector can
     * actually show — an object id of 1 is 1/255 and reads as black. */
    TexturePreviews    previews_;

    /* Set whenever the geometry the probes captured stops being true. The
     * rooms themselves are re-derived at the top of the next frame, because
     * destruction runs mid-frame and re-placing probes underneath a pass that
     * is already sampling them is a race the shader cannot see.
     *
     * The round-robin cursor and the stale count now live on the probe set —
     * with a probe per room they are its business, not a pair of loose ints
     * here. */
    bool             probesDirty_ = true;

    /* How many (probe, face) slices to refresh per frame. One is the steady
     * state and what the amortisation argument assumes; the dev panel can
     * raise it to make a rebuild settle faster while somebody is looking at
     * it. */
    int              probeFacesPerFrame_ = 1;

    /* The inspector's label for the probe strip. A member rather than a local
     * because DevTextures borrows the pointer and copies nothing — see where
     * it is filled in. */
    char             probePreviewNote_[128] = "";
    PbrShader        pbr_;
    MaterialLibrary  materials_;
    PropSet          props_;

    /* The baked static sun. Constructed once the world exists; re-baked on the
     * same event that rebuilds the static mesh, so the lighting can never
     * describe geometry that is no longer there. */
    std::unique_ptr<SunBaker> lightBake_;
    Texture2D                 lightmapTexture_{};
    Texture2D                 lightIndexTexture_{};

    /* B toggles the baked sun against the shadow map, live, so the two can be
     * compared in one session rather than across two builds. Defaults OFF: the
     * bake is the newer, less proven path, and a renderer should not ship its
     * experiment as the thing you get by default. */
    bool useBakedSun_ = false;

    /* Re-bakes only what a change at `centre` could have altered, and pushes
     * the result to the GPU. */
    void rebakeAfterChange(const Cell& centre, float radiusTiles);
    void uploadLightmap();
    SunSample currentSun() const;
    SkyPass          sky_;
    ToneMapPass      tonemap_;
    OverlayShader    overlayShader_;
    PrepassShader    prepass_;
    AmbientOcclusion ao_;

    /* The linear HDR buffer the whole scene is shaded into, at
     * ToneMapPass::kSupersampleFactor times the window on each axis. */
    HdrTarget scene_;
    int       sceneWidth_ = 0;
    int       sceneHeight_ = 0;

    /* Position-only material for the shadow map. Falls back to raylib's
     * default if the depth shader is missing, so a shader failure costs
     * shadows rather than the whole frame.
     *
     * ALIASES ShadowMap's material — Material::maps is a heap pointer, so this
     * copy shares that array. Read-only here, and it must stay that way. */
    Material depthMaterial_ = { 0 };

    /* interaction state */
    std::optional<int>     hovered_;
    std::vector<PathPoint> preview_;
    std::vector<int>       route_;
    Vector2                pressedAt_{};

    bool softCutaway_  = true;
    bool showCover_    = true;
    bool grenadeArmed_ = false;

    /* F cycles the diagnostic views: 0 off, 1 geometry only, 2 the reflection
     * probe as a mirror. Both exist because questions like "is that a hole in
     * the mesh or a hole in the lighting" and "is the cubemap even populated"
     * cannot be answered from the lit image, and guessing at them has cost
     * real time. See PbrShader::setDebugView. */
    int debugView_ = 0;
    /* off, geometry, probes, rooms, roughness, occlusion.
     * See PbrShader::setDebugView. */
    static constexpr int kDebugViewCount = 6;

    /* Views that REPLACE surface shading, as opposed to adding something to an
     * ordinary frame. Geometry and rooms both paint every fragment a flat
     * colour, so glass is drawn solid in the opaque pass and the blended pass
     * is skipped. The probe view is not one of these — its chrome balls need a
     * real scene to sit in — and writing `debugView_ != 0` instead is how it
     * would silently become one. */
    bool flatShading() const
    {
        return debugView_ == 1 || debugView_ == 3 ||
               debugView_ == 4 || debugView_ == 5;
    }

    int moveLoops_ = 0, moveEdges_ = 0;
    int sprintLoops_ = 0, sprintEdges_ = 0;

    std::string status_;
};

}  // namespace xcom
