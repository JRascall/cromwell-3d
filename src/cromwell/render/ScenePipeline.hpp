/* ScenePipeline.hpp — the frame's passes, their targets, and the order they run in.
 *
 * SINGLE RESPONSIBILITY: own every render target, shader and pipeline the
 * sequence needs, and run one frame through rhi::IRenderDevice.
 *
 * ==================== WHY THIS IS ENGINE AND NOT GAME =====================
 *
 * Because none of it is about this game.
 *
 * A shadow map is a depth-only pass over the world from the sun's point of
 * view, framed to the world's bounds, sampled by the lit pass. That sentence is
 * true of an RTS, an FPS and a third-person game, and the code implementing it
 * mentions nothing that is not true of all three. The same goes for the depth
 * prepass, the occlusion pass, the resolve, and the order they run in — the
 * ordering constraints are properties of rendering, not of soldiers.
 *
 * WHAT IS THE GAME'S: the meshes, and where the world is. Both arrive as
 * RENDERABLES in a RenderScene, which the engine culls, sorts and draws. The
 * game never sees an encoder, a pass or a pipeline.
 *
 * This split is the whole point of the port. An engine whose frame sequence
 * lives in the game is an engine that gets copy-pasted into the next project
 * and then diverges; one where the game supplies geometry to a sequence it does
 * not own is one that can be lifted. See the note at the top of CMakeLists.txt
 * on the arrows never reversing.
 *
 * ============================ CONVERSION STATE ============================
 *
 *   shadow map    done (4096, focused on the camera's frustum and texel
 *                 snapped, sampled with PCSS - no cascade, no transmission
 *                 plane for glass, no baked lightmap)
 *   depth prepass done
 *   occlusion     done (with its bilateral blur - the two are one pass in
 *                 two halves and neither works alone)
 *   sky           done (analytic, same two lobes the ambient term integrates)
 *   custom depth  done (tagged objects into an id + depth buffer, with a
 *                 selection outline reading it after the tone map)
 *   decals        done (DBuffer: three planes at half the scene, one box per
 *                 decal, blended into the surface's material before it lights)
 *   probes        done (one cubemap per room in a cube array, captured round
 *                 robin a face at a time, parallax-corrected and selected per
 *                 pixel - no prefiltered mip chain, so the term slides back to
 *                 the analytic sky as roughness rises)
 *   lit scene     done (untextured - real sun and sky, shadow, occlusion,
 *                 vertex colour, emissive materials and the DBuffer's
 *                 override; no albedo/normal/roughness maps on SURFACES yet)
 *   bloom         done (threshold, mip chain, tent upsample, composited into
 *                 the HDR target before the resolve - not after it)
 *   tone map      done (and it resolves the 2x supersample - see kSupersample)
 *
 * Passes are added here as they are converted. The game-side renderer that
 * drives this stays the same size as each one lands, which is the property
 * worth watching.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/render/IScenePass.hpp"
#include "cromwell/render/SceneDrawList.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

/* Forward-declared rather than included: SceneFrame only carries a pointer to
 * one, and DebugDraw drags in the whole segment queue for callers that never
 * ask for a debug line. */
class DebugDraw;

class DeviceDecalSet;
class DeviceProbeSet;
class RenderAssets;
class RenderScene;
namespace rhi { class ICommandEncoder; class IRenderDevice; }

/* WHAT THE GAME DECIDES ABOUT THIS FRAME. Everything else — targets, formats,
 * pass order — is the pipeline's.
 *
 * An aggregate, filled by one caller immediately before the call and read by
 * one callee: the same one-shot carrier FrameView and PassContext are, and for
 * the same reason. */
struct SceneFrame {
    /* Where the sun is, as a direction light TRAVELS. Normalised by the caller;
     * a zero vector skips the shadow pass rather than producing a degenerate
     * matrix. */
    Vec3 sunDirection{ -0.45f, -0.8f, -0.4f };

    /* ---- the environment, in LINEAR radiance ----------------------------
     *
     * NOT sRGB, and not a swatch anybody picked off a colour wheel. These are
     * the numbers cromwell::SunLight derives from a time of day, and the sun's
     * is genuinely several times brighter than one — the whole reason the scene
     * target is RGBA16F and the resolve exists.
     *
     * PLAIN VALUES RATHER THAN A SunLight REFERENCE, deliberately. SunLight
     * still names raylib types, and SceneFrame crosses into cromwell_base where
     * raylib does not exist; a frame description carrying six numbers is also
     * the thing a replay, a thumbnail bake or a test can fill without owning a
     * light. The caller converts at the boundary — see RhiFrameRenderer. */
    Vec3  sunRadiance{ 1.0f, 1.0f, 1.0f };
    Vec3  skyZenith{ 0.16f, 0.29f, 0.52f };
    Vec3  skyHorizon{ 0.52f, 0.62f, 0.75f };

    /* THE BOUNCE OFF THE GROUND, and it is not black. A face pointing straight
     * down still sees light, and zeroing this is the single most common reason
     * untextured geometry reads as plastic — every underside becomes a hole. */
    Vec3  skyGround{ 0.13f, 0.12f, 0.10f };

    /* How much of the sky actually reaches a surface. Well under one on
     * purpose: the sky colours above are authored to look right as a BACKDROP,
     * and a backdrop's radiance is nothing like the irradiance it delivers. See
     * SunLight::ambientIntensity for the full argument. */
    float ambientIntensity = 0.42f;

    /* HOW BIG THE SUN LOOKS, in radians — and therefore how soft every shadow
     * is, because a penumbra is 2 * distance * tan(this). Nearly the real sun's
     * 0.0047; see SunLight::kAngularRadius, which is where this comes from and
     * which explains at length why an oversized value turns contact hardening
     * into uniform fog. */
    float sunAngularRadius = 0.0055f;

    /* The resolve's exposure. Here rather than a constant in the pipeline
     * because it is the one number a player-facing brightness slider moves. */
    float exposure = 4.5f;

    /* ---- WHERE THE EYE IS USED TO LIVE HERE, AND NOW LIVES ON THE VIEW ----
     *
     * `view`, `projection` and `cameraPosition` moved to cromwell::View when
     * the render scene landed, and the move is the point rather than a tidy-up.
     *
     * A FRAME DESCRIBES A WORLD FOR AN INSTANT; A VIEW DESCRIBES AN EYE. The
     * sun, the sky, the exposure and the debug queue are the same for every
     * player looking at one world — the matrices are not. With both on one
     * struct, four split-screen panes would be four SceneFrames differing in
     * three fields, and every field that ought to be shared would be copied
     * four times and could drift. That is the same mistake as two SunLights,
     * which RhiFrameRenderer's header describes: a second answer to a question
     * with one answer, discovered later as two renderers disagreeing. */

    /* ---- WHICH FEATURES THIS FRAME WANTS -------------------------------
     *
     * The dev panel's layer switches, and until now the device path read only
     * two of the eight - so `shadows`, `sky` and `ambient occlusion` were
     * checkboxes that moved and changed nothing. A switch that cannot answer
     * "is this the shadows?" is worse than no switch, which is the argument
     * ViewLayers already makes about every other toggle.
     *
     * OFF DOES NOT MEAN "SKIP THE PASS" for two of these, and that is the part
     * worth stating. The lit pass samples the shadow map and the occlusion
     * plane unconditionally - a pipeline's bindings are the same every frame -
     * so a skipped pass leaves LAST frame's texture bound and the feature does
     * not turn off, it FREEZES. Each one therefore has a defined "off" value
     * the pass still writes: depth 1.0 is nothing in shadow, white is nothing
     * occluded. */
    bool shadows = true;
    bool ambientOcclusion = true;

    /* The sky is different: nothing samples it, it is simply the first colour
     * into the scene target. Off means the lit pass CLEARS rather than loads,
     * because otherwise the target still holds the previous frame. */
    bool sky = true;

    /* ---- BLOOM: light spreading off whatever is brighter than the scene ---
     *
     * Composited INTO the HDR scene target before the resolve, which is the
     * decision `study/plans/bloom_emissive.md` argues for at length and the one
     * the raylib path's GlowPass got wrong: that one composites AFTER the tone
     * map, in display colour, which ties it to the main view twice over and
     * makes the no-tonemap debug view stop meaning anything.
     *
     * The four numbers are BloomTuning's; they arrive here as plain values for
     * the same reason the sun's do — a frame description crosses into
     * cromwell_base and carries data rather than objects. */
    /* WHETHER THE DECALS ARE DRAWN AND SAMPLED. Both halves, like the probes
     * and for the same reason: gating only the PASS would leave the last
     * frame's planes bound and still read, so the switch would freeze the
     * decals rather than remove them. The pass still runs and still CLEARS when
     * this is off, which is what makes "off" mean off. */
    bool  decals = true;

    /* ---- THE CUSTOM DEPTH BUFFER AND THE OUTLINE THAT READS IT ----------
     *
     * `customDepth` is ViewLayers' switch: it gates the pass AND the outline,
     * because the outline is the only consumer and a buffer nobody reads is
     * work nobody can see.
     *
     * `outlineStencil` is WHICH tagged object gets a silhouette — 0 for none,
     * which is the default and the ordinary state. The engine never learns what
     * the value MEANS; the game assigns ids to its renderables and names one
     * here, exactly as it spends the filter bits.
     *
     * THE COLOURS ARE DISPLAY COLOUR, NOT RADIANCE, because this composites
     * after the tone map — see the shader on why an outline is interface rather
     * than light. */
    bool  customDepth = true;
    int   outlineStencil = 0;
    float outlineThickness = 2.0f;

    float outlineVisible[4]  = { 1.00f, 0.85f, 0.30f, 1.0f };

    /* Dimmer and cooler where the object is behind something. Not transparent:
     * knowing the selected soldier is behind that wall is most of what the
     * effect is for. Alpha zero here turns the x-ray half off. */
    float outlineOccluded[4] = { 0.35f, 0.55f, 0.90f, 0.75f };

    /* WHAT A FULL-STRENGTH DECAL EMISSIVE MASK IS WORTH, in linear radiance.
     * The mask is one 8-bit channel of an RGBA8 plane and the surface shaders
     * output HDR, so the scale cannot live in the buffer — the same reason the
     * sun is brighter than a wall. Tuned against SunLight's radiance so a fully
     * self-lit decal reads as glowing rather than merely pale after the tone
     * map. */
    float decalEmissiveScale = 12.0f;

    bool  bloom = true;
    float bloomThreshold = 1.1f;
    float bloomKnee = 0.55f;
    float bloomIntensity = 0.06f;
    float bloomRadius = 1.0f;

    /* THE FILMIC CURVE IN THE RESOLVE. Off means RAW — the exposure is still
     * applied and the supersample still collapses, but the linear radiance goes
     * to the target unbent, clamped by its format. For a capture feeding a
     * shader that wants radiance rather than display colour, and for looking at
     * what the lit pass actually wrote. */
    bool toneMap = true;

    /* ---- WHICH LIGHTING TERMS ARE SWITCHED OFF, as bits ------------------
     *
     * `RenderEffects::suppressMask()` — one bit per CONTRIBUTION to a pixel,
     * where ViewLayers' switches are one per PASS. The two answer different
     * questions and the second cannot be built out of the first: turning the
     * shadow pass off says nothing about whether the artefact on that wall is
     * the ambient specular.
     *
     * A SUPPRESSION MASK RATHER THAN AN ENABLE ONE, and the polarity is the
     * point. A uniform that fails to arrive reads as zero in GLSL, and zero has
     * to mean "the ordinary image" — with enable bits, any path that forgot to
     * push this would black the scene out entirely, which is a debug switch
     * breaking the render it exists to explain. RenderEffects.hpp records that
     * happening once; hence the direction and hence this note.
     *
     * ZERO IS THE DEFAULT AND MEANS NOTHING IS SUPPRESSED, so a probe capture,
     * a thumbnail bake and a test all get the real image without saying
     * anything. */
    int effectSuppress = 0;

    /* ---- and what the occlusion pass is tuned to -----------------------
     *
     * BORROWED FROM THE LIVE AmbientOcclusion::Tuning, not retyped. These were
     * copied constants in drawOcclusion and had drifted to 0.9 / 0.025 against
     * the raylib path's 0.45 / 0.008 - which is the mistake MIGRATION.md 5
     * records under "tuning invented rather than borrowed", and the reason the
     * two renderers' SSAO looked different. Carrying them here is 4.10's first
     * debt item: the dev panel's sliders now reach this renderer. */
    float occlusionRadius = 0.45f;
    float occlusionBias = 0.008f;
    float occlusionStrength = 1.0f;

    /* WHETHER THE REFLECTION PROBES RUN AT ALL. Off is the dev panel's
     * "reflection probes" switch, and it turns off both halves — the capture
     * and the sampling — because a switch that left the captures running would
     * cost the same and answer nothing. See ViewLayers, which makes the same
     * argument for every other feature toggle: a switch that cannot answer "is
     * this the reflections?" is worse than no switch.
     *
     * A frame with this off falls back to the analytic sky, which is exactly
     * what a board with no probes placed on it already does. */
    bool reflections = true;

    /* WHICH DIAGNOSTIC VIEW IS UP, if any. 0 is the ordinary frame; the rest
     * match ViewSettings::debugView so one key cycles the same views on both
     * renderers. 5 is the occlusion plane, which is the only way to actually
     * SEE what SSAO is doing rather than infer it from a corner. */
    int debugView = 0;

    /* The backbuffer's clear colour, in linear terms. */
    float clearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    /* THE DEBUG LINES TO DRAW OVER THE SCENE, or null for none.
     *
     * BORROWED, AND NOT CONSUMED. The queue is aged once a frame by whoever
     * owns it — see DebugDraw::advance — precisely so that a caller drawing the
     * scene twice does not silently eat everything on the first pass. This
     * pipeline reads it and leaves it alone.
     *
     * A POINTER RATHER THAN A REFERENCE because a frame legitimately has none:
     * a probe capture, a thumbnail bake and a test all draw a scene with no
     * diagnostic geometry in it, and requiring an empty queue to be constructed
     * to say so is ceremony. */
    const DebugDraw* debug = nullptr;
};

class ScenePipeline {
public:
    /* THE ASSETS ARE BORROWED AND SHARED. Materials used to be a member here,
     * and that was the wrong lifetime — see RenderAssets.hpp, which names the
     * three (device, world, view) and explains why a pipeline is the third. */
    ScenePipeline(rhi::IRenderDevice& device, RenderAssets& assets);
    ~ScenePipeline();

    ScenePipeline(const ScenePipeline&) = delete;
    ScenePipeline& operator=(const ScenePipeline&) = delete;

    /* Loads shaders and builds targets. Call once, after the device exists.
     * False means the frame cannot be drawn — it has already logged which stage
     * failed, and the caller should fall back rather than retry. */
    bool initialise();

    /* ONE FRAME, OF ONE VIEW OF ONE SCENE.
     *
     * `view` names the scene, the eye, what that eye may see and where the
     * picture lands; the engine derives the sun's view and every probe face's
     * from it. `frame` is what is true of the WORLD this instant — the sun, the
     * sky, the exposure — and is shared by every view of it.
     *
     * THERE IS NO THIRD ARGUMENT ANY MORE. `IGeometrySource& geometry` used to
     * be one, and its removal is rhi/MIGRATION.md §4.12 step 5 — the game does
     * not submit geometry, does not implement a pass callback, and never learns
     * what a shadow pass wants. */
    void render(const SceneFrame& frame, const View& view);

    /* ---- THE HATCH: a game's own pass, at a named point ------------------
     *
     * Registered once at startup and run for every view. See IScenePass.hpp,
     * which is the whole argument for what this may and may not do — and which
     * says at length that this is the RARE case and materials are the ordinary
     * one.
     *
     * BORROWED, NOT OWNED: the pass must outlive this pipeline. Passing the
     * same pass twice registers it twice, which is a caller error rather than
     * something to silently deduplicate — running twice at two points is a
     * legitimate thing to want. */
    void addPass(ScenePassPoint point, IScenePass& pass);

    /* THE FRAME'S INTERMEDIATES, for a later pass to sample and for a
     * diagnostic to look at. Invalid until initialise() has succeeded.
     *
     * The G-buffer's ALPHA IS ROUGHNESS, not coverage — see the converted
     * prepass fragment shader. Anything sampling it should know that before it
     * reads the fourth channel. */
    rhi::TextureHandle shadowMap() const { return shadowDepth_; }

    /* WHAT THE SUN BECOMES CROSSING GLASS, at half the shadow map's size. A
     * COLOUR rather than a fraction, so a stained pane tints the light it
     * passes; the lit passes multiply their sunlight by it.
     *
     * WORTH EXPOSING ALONGSIDE THE DEPTH because the pair is how a shadow bug
     * is separated: geometry missing from the depth map and geometry missing
     * from the transmission plane are different failures with the same symptom
     * on the lit image, and looking at one without the other cannot tell them
     * apart. */
    rhi::TextureHandle shadowTransmission() const { return shadowTransmission_; }

    rhi::TextureHandle sceneDepth() const { return sceneDepth_; }
    rhi::TextureHandle sceneNormals() const { return sceneNormals_; }
    /* THE BLURRED ONE, which is what the lit pass samples and what a diagnostic
     * should show. The raw plane is deliberately not exposed: it is noise by
     * construction and looking at it invites the conclusion that the occlusion
     * pass is broken. */
    rhi::TextureHandle occlusion() const { return occlusionBlurred_; }
    rhi::TextureHandle sceneColour() const { return sceneColour_; }

    /* ---- THE DBUFFER: what the decals decided, before any lighting -------
     *
     * NOT A G-BUFFER. Nothing here describes the world; it describes an
     * OVERRIDE of the world that the lit pass applies to its own material
     * inputs before it lights them. The lighting is still forward, still per
     * material, still one pass.
     *
     *   albedo   rgb = base colour, LINEAR, premultiplied   a = 1 - coverage
     *   normal   rgb = world normal * 0.5 + 0.5, premult.   a = 1 - coverage
     *   surface  r = metal  g = rough  b = emissive mask    a = 1 - coverage
     *
     * Exposed for the texture inspector, which is where a decal that fails to
     * appear stops being four plausible causes and becomes one picture: ink
     * here and nothing on screen is the READ; nothing here is the PASS. */
    rhi::TextureHandle decalAlbedo() const { return decalAlbedo_; }
    rhi::TextureHandle decalNormal() const { return decalNormal_; }
    rhi::TextureHandle decalSurface() const { return decalSurface_; }

    /* ---- THE CUSTOM DEPTH / STENCIL BUFFER ------------------------------
     *
     * A chosen subset of the world, rasterised apart from the scene with each
     * object's ID and its DEPTH — Unreal's custom depth, and as general as
     * theirs: selection outlines, x-ray silhouettes, masking a post effect to
     * particular actors and blur masks are all this buffer with different
     * consumers.
     *
     *   stencil  r = the object's value 0-255, encoded    a = coverage
     *   depth    that object's depth, to compare against the frame's
     *
     * Which objects are in it is asked OF THE RENDERABLE — see
     * RenderableDesc::withCustomStencil — so the engine never learns what a
     * value means, exactly as it never learns what a filter bit means. */
    rhi::TextureHandle customStencil() const { return customStencil_; }
    rhi::TextureHandle customDepth() const { return customDepth_; }

    /* ---- outline quality, and why it is its own dial --------------------
     *
     * HOW FINELY THE TAGGED GEOMETRY IS RASTERISED, as a multiple of the
     * surface — independent of the scene's own supersample, which is fixed.
     * 1, 2, 4 or 8; anything else is clamped to the nearest of those, because
     * the shader's sample pattern is chosen per factor and a factor of 3 has no
     * pattern to choose.
     *
     * WHY IT IS NOT SIMPLY THE SCENE'S FACTOR. The scene supersamples at 2x and
     * that is enough for shaded geometry, whose edges are low contrast against
     * their neighbours. An outline is not: it is saturated ink over whatever is
     * behind it, and the eye reads a step in it that it would never notice on a
     * wall. So the silhouette wants finer sampling than the picture does, and
     * tying the two together would mean paying for the whole scene at 4x to fix
     * a line two pixels wide.
     *
     * WHY IT HAS TO BE THE BUFFER AND NOT A FILTER. A 2x buffer gives each
     * output pixel two distinct sample positions per axis, so a near-vertical
     * edge can only be 0, half or 1 covered — three levels, and three levels on
     * a high-contrast line is a visible staircase. No amount of filtering
     * invents the missing positions; only rasterising finer does. At 4x there
     * are four per axis, which is the point at which a ROTATED sample pattern
     * becomes possible and near-vertical and near-horizontal edges stop being
     * the worst case. See study/topics/rendering/outline_antialiasing.md §8.
     *
     * WHAT IT COSTS IS MEMORY, NOT FILL. The pass draws only tagged objects — a
     * soldier or two — so rasterising them finer is nearly free. The two targets
     * are what grows, and they grow with the square: at 1280x800 the pair is
     * about 33 MB at 2x and 131 MB at 4x. That is the trade this dial exists to
     * let a player make, which is why it is a setting and not a constant.
     *
     * SAMPLE COUNT DOES NOT FOLLOW IT. The shader reads a fixed, rotated subset
     * of each pixel's block rather than all of it, so raising this buys sample
     * POSITIONS without multiplying the pass's cost. */
    ScenePipeline& withOutlineSupersample(uint32_t factor);
    uint32_t outlineSupersample() const { return outlineSupersample_; }

    /* The prepass and everything reconstructed from it are sized to the
     * surface, so a resize rebuilds them. Cheap to call every frame — it
     * returns immediately when the size has not changed. */
    void resize(uint32_t width, uint32_t height);

    /* THE MATERIALS AND THE PROBES USED TO BE ACCESSORS HERE. Both moved, in
     * opposite directions, and the pair of moves is rhi/MIGRATION.md §4.12's
     * first open problem closed:
     *
     *   materials -> RenderAssets, because they are the DEVICE's. Loaded once
     *                from `.mat` files and shared by every scene and every
     *                view; one copy per pipeline would be one per quality
     *                setting, loaded that many times, free to drift.
     *   probes    -> RenderScene, because they are the WORLD's. A probe set
     *                describes where the rooms are and what they reflect, not
     *                a viewpoint. Two players in one world would otherwise each
     *                capture and prefilter the same probes.
     *
     * Ask the assets or the scene. Nothing was deleted; it is filed correctly. */

private:
    /* WHERE THE PROBES ARE NOW: on the scene the view names. Null when a view
     * carries no scene, which is ordinary on the frames before a world exists.
     *
     * NON-CONST, AND IT IS THE ONE THING A PASS WRITES BACK INTO THE WORLD. The
     * capture schedule advances as faces are drawn, and the prefilter marks a
     * probe current. That is genuinely world state — the cubemaps belong to the
     * rooms — rather than a pass reaching into the scene it is drawing, and it
     * is why View holds its scene non-const. Nothing else here mutates one. */
    static DeviceProbeSet* probesOf(const View& view);

    /* THE WORLD'S EXTENT, from the scene and — while the seam lasts — from
     * whatever has not been converted yet, unioned.
     *
     * IT IS THE UNION OF THE RENDERABLES, and it used to be a number the game
     * stated: the lattice plus a tile of margin, which includes all the empty
     * air above an untouched map. The sun's orthographic fit spends resolution
     * on every unit of whatever it is given, so the tighter box is not merely
     * tidier — it is texels. See RenderScene::worldBounds. */
    static Aabb worldBoundsOf(const View& view);

    /* ISSUE A COLLECTED LIST. The transform and the tint go out as the object
     * push and the material is bound per item — the same two things every
     * submitter used to do for itself, in one place that cannot forget either.
     *
     * `bindMaterials` is false for the sun's depth pass, which has no material
     * block bound and whose shader reads position and writes depth. Binding one
     * there would be work for a stage that cannot see it. */
    void drawItems(rhi::ICommandEncoder& encoder, const std::vector<DrawItem>& items,
                   bool bindMaterials) const;

    /* Run whatever the game registered at this point, for this view. Returns
     * immediately when nothing is registered, which is the case in every
     * project that has not needed the hatch. */
    void runHatch(ScenePassPoint point, const View& view);

    /* THE SUN'S ORTHOGRAPHIC BOX, and the two scales the shader needs to read
     * what it wrote.
     *
     * WHY THE SHADER NEEDS MORE THAN A MATRIX. Both of its biases are quantities
     * in world units — a normal offset measured in texels, and a depth bias
     * measured in millimetres of tile — and a depth buffer stores neither. A
     * normalised depth of 0.001 is a hair in a tight projection and half a tile
     * in a loose one, so a bias tuned as a bare constant silently rescales every
     * time the box is refitted. That is precisely how a working bias turns into
     * light leaking from the foot of a wall when the camera zooms out. */
    struct SunProjection {
        Mat4  viewProjection;
        float worldTexelSize = 1.0f;   /* one shadow texel, in world units */
        float depthRange = 1.0f;       /* world units the depth range spans  */
    };

    /* FRAMED TO WHAT THE CAMERA CAN SEE, not to the whole world.
     *
     * This was a box round the entire lattice, which is correct and wastes
     * almost all of the resolution: a map sixty tiles across gets a handful of
     * texels per tile, so every shadow edge is a staircase no amount of
     * filtering can hide. Fitting instead to the camera's frustum — clipped to
     * the world, because the frustum reaches into empty sky the lattice does not
     * occupy — is the same trick FrameRenderer::shadowFocus plays, and it is
     * worth several times the linear texel density on its own.
     *
     * A SPHERE ROUND THAT BOX, NOT THE BOX ITSELF. A sphere is the only shape
     * whose extent does not change as the sun rotates, so texel density — and
     * with it the acne threshold — stays put as the sun sweeps rather than
     * breathing with it.
     *
     * SNAPPED TO WHOLE TEXELS. Without it, moving the camera slides the shadow
     * map's texel grid continuously under every static shadow edge, and the
     * whole scene crawls. The snap happens in the LIGHT's frame, which is why
     * the rotation is built before the centre is quantised.
     *
     * WHAT THIS COSTS: geometry outside the camera's view no longer casts into
     * the map. It cannot be seen, so that is free — but it is the reason the
     * pass still submits the WHOLE world rather than the cutaway, since a wall
     * behind the camera still shadows ground in front of it. */
    /* THE CAMERA COMES FROM THE VIEW AND THE SUN FROM THE FRAME, which is the
     * split this whole design rests on: one world lit one way, seen by however
     * many eyes. Each eye gets its OWN focused shadow map — see the
     * split-screen budget note in rhi/MIGRATION.md §4.11, because that is N
     * shadow maps and N occlusion passes rather than N times the draws. */
    static SunProjection sunProjection(const SceneFrame& frame, const View& view,
                                       Vec3 minimum, Vec3 maximum);

    bool createSceneTargets(uint32_t surfaceWidth, uint32_t surfaceHeight);

    /* THE SCENE TARGETS' SIZE, which is the surface's times the supersample
     * factor. Named rather than multiplied at each use: `targetWidth_` holds
     * the SURFACE's width, and every pass except the resolve works in scene
     * pixels — so the mistake available here is reaching for the member and
     * being wrong by a factor of two, silently, in a screen-space pass. */
    uint32_t sceneWidth() const;
    uint32_t sceneHeight() const;

    /* THE SUN'S DEPTH PASS. It builds the sun's own View from `view` — same
     * scene, same viewer, no cutaway — collects into sunList_, and draws its
     * opaque half. The translucent half is what the transmission pass below
     * wants, which is why one collection serves both. */
    void drawShadowMap(const SceneFrame& frame, const View& view);
    void drawPrepass(const SceneFrame& frame, const View& view);
    /* ---- THE DECALS, INTO THE DBUFFER -----------------------------------
     *
     * After the prepass, whose depth and normals it unprojects, and before the
     * lit pass, which reads what it wrote. One draw per decal: a projector box
     * with back faces only, no depth test and a separate-factor blend, so any
     * number of overlapping decals compose in one equation.
     *
     * NOTHING IS DRAWN INTO THE SCENE HERE. The pass writes a material
     * override; the surface underneath lights once, with its own shadow, its
     * own probe and its own occlusion already in hand. A decal drawn as its own
     * lit quad would have to recompute all three. */
    /* ---- THE CUSTOM DEPTH / STENCIL PASS --------------------------------
     *
     * A depth-only draw of whatever carries a non-zero stencil value, writing
     * that value into a colour channel and the object's depth into a depth
     * attachment. Its eye is the CAMERA'S, which is the whole point: the buffer
     * is only useful because its depth is comparable with the frame's.
     *
     * IT DRAWS NOTHING VISIBLE. This is the half that is awkward to retrofit —
     * a place for selected geometry to rasterise apart from the scene — and
     * every effect built on it is a later pass. drawOutline below is the
     * first. */
    void drawCustomDepth(const SceneFrame& frame, const View& view);

    /* ---- AND THE FIRST THING THAT READS IT ------------------------------
     *
     * A silhouette round the tagged object, composited over the RESOLVED image
     * in display colour. After the tone map, unlike bloom and deliberately so:
     * an outline is interface rather than light, and a designer's colour for it
     * should survive the exposure curve rather than be graded by it. See
     * rhi/post/outline.fs.glsl. */
    void drawOutline(const SceneFrame& frame, const View& view);

    void drawDecals(const SceneFrame& frame, const View& view);

    /* Grows the per-decal block buffer to hold at least this many. */
    bool ensureDecalCapacity(uint32_t decals);

    void drawOcclusion(const SceneFrame& frame, const View& view);
    void drawOcclusionBlur(const SceneFrame& frame, const View& view);
    void drawSky(const SceneFrame& frame, const View& view);
    void drawLitScene(const SceneFrame& frame, const View& view);

    /* What you can see through, after the opaque scene. Blend mode is a
     * MATERIAL property, so which surfaces land here is authored rather than
     * coded — see DeviceMaterials::isTranslucent. */
    void drawTransparent(const SceneFrame& frame, const View& view);

    /* The debug queue's segments, over the finished scene and before the
     * resolve. Nothing when the frame carries no queue or it is empty — and in
     * that case not even a pass is opened, because an empty pass on a tiler
     * still stores and reloads the attachment. */
    void drawDebugLines(const SceneFrame& frame);

    /* Grows the line vertex buffer to hold at least this many vertices. */
    bool ensureDebugCapacity(uint32_t vertexCount);

    /* What the sun becomes crossing anything translucent, into a plane the lit
     * passes tint their sunlight by. Runs immediately after the shadow map,
     * whose matrix and depth it both reuse. */
    void drawShadowTransmission(const SceneFrame& frame, const View& view);

    /* ONE CUBE FACE OF ONE PROBE, a few times per frame — the world as seen
     * from a point in a room, into one slice of the cubemap array.
     *
     * THE SAME LIT AND TRANSPARENT SHADERS THE CAMERA USES, which is the whole
     * economy of it: a reflection shows the sun, the shadows and the glass
     * exactly as the frame does, because it IS the frame from another eye. What
     * differs is a handful of pass state — no prepass, so depth tests Less
     * rather than Equal; no SSAO, because screen space means nothing in a
     * cubemap face; no supersample; and no probes, because the array being
     * written cannot also be read.
     *
     * NO SKY EITHER, and that one is easy to get wrong. The capture clears to
     * TRANSPARENT BLACK and draws geometry only, so the alpha channel says "the
     * world is in this direction". The shader blends to the analytic sky
     * wherever it is zero — which keeps the sky gradient smooth and continuous
     * at 128 pixels a face, where a rendered sky would be a blocky one. */
    void drawProbeCapture(const SceneFrame& frame, const View& view);

    /* ONE PROBE'S MIP CHAIN, GGX-convolved, once all six of its faces are
     * current. Level L ends up holding the probe at roughness L/(levels-1), so
     * the lit pass reads its roughness as a LOD instead of fading the whole
     * term out to a flat sky.
     *
     * PER PROBE, NEVER PER FACE, and that is a hard constraint rather than a
     * batching choice: a GGX lobe at high roughness reaches across face
     * boundaries, so convolving +X while -Z still holds pre-rebuild content
     * bakes stale data into the chain — and bakes it permanently, because the
     * next capture sweep rewrites level 0 and nothing reruns the convolution
     * until all six faces come round again. DeviceProbeSet::probeReadyToPrefilter
     * is what enforces it. */
    void drawProbePrefilter(const View& view);

    /* ---- BLOOM, AS A MIP CHAIN OVER ONE TEXTURE -------------------------
     *
     * Prefilter the scene's bright half into level 0 at half the scene's size,
     * downsample to the bottom, then upsample back to the top ADDING as it
     * goes, and composite level 0 into the scene target additively. Runs
     * between the debug lines and the resolve, so everything before it is in
     * linear radiance and the result is too.
     *
     * ONE TEXTURE WITH MIPS rather than N textures. The RHI already attaches by
     * `mip` and clamps a sampler's LOD range, which is exactly what the probe
     * prefilter needed for the same hazard — reading level N while writing
     * level N-1 of the same texture is undefined unless the sampler provably
     * cannot reach the level being written. Six separate textures would be six
     * allocations, six sizes to keep in step, and no more correct.
     *
     * INTO THE HDR TARGET, BEFORE THE RESOLVE, and that is the whole design
     * decision. Compositing after the tone map — which is what the raylib
     * path's GlowPass does — means the glow is display colour added to display
     * colour, so it cannot be exposed with the rest of the frame, it is wrong
     * under the no-tonemap debug view, and it is tied to the main view's
     * resolution. See study/plans/bloom_emissive.md. */
    void drawBloom(const SceneFrame& frame);

    /* One stage of the chain. `sourceLevel` is what it reads and `targetLevel`
     * what it writes; both name levels of bloomChain_ except at the two ends,
     * where the source is the scene target and the target is. */
    void bloomStage(rhi::PipelineHandle pipeline, rhi::TextureHandle source,
                    rhi::SamplerHandle sampler, uint32_t sourceWidth, uint32_t sourceHeight,
                    rhi::TextureHandle target, uint32_t targetLevel,
                    uint32_t targetWidth, uint32_t targetHeight,
                    const SceneFrame& frame, float intensity, bool additive);

    void drawResolve(const SceneFrame& frame, const View& view);
    void drawBackbuffer(const SceneFrame& frame);

    rhi::IRenderDevice& device_;
    bool ready_ = false;

    /* THE MATERIALS AND WHATEVER ELSE THE DEVICE OWNS. Borrowed; see the note
     * where materials() used to be. The pipeline still uploads materialBlock_
     * below as the DEFAULT, so a draw that binds nothing gets a sane material
     * rather than whatever the last pass left at binding 2. */
    RenderAssets& assets_;

    /* ---- what each view found, kept across frames for its capacity -------
     *
     * THREE LISTS BECAUSE THREE COLLECTIONS ARE LIVE AT ONCE within a frame:
     * the camera's is filled before the prepass and read again by the lit and
     * transparent passes; the sun's is filled by the shadow pass and read again
     * by the transmission pass; a probe face's is filled and consumed inside
     * its own loop. Sharing one would mean re-collecting between passes that
     * are looking at the same list.
     *
     * MEMBERS RATHER THAN LOCALS so the vectors settle at their high-water mark
     * and stop allocating — the same arrangement debugScratch_ has, and for the
     * same reason. */
    SceneDrawList cameraList_;
    SceneDrawList sunList_;
    SceneDrawList probeList_;

    /* ---- the hatch ------------------------------------------------------
     *
     * A FLAT LIST SCANNED PER INSERTION POINT, rather than a bucket per point.
     * It is scanned four times a frame over a vector that is EMPTY in every
     * project that has not needed the hatch, and the alternative — four vectors
     * — is four things to keep in step to save nothing measurable. */
    struct HatchPass {
        ScenePassPoint point = ScenePassPoint::AfterOpaque;
        IScenePass*    pass = nullptr;
    };
    std::vector<HatchPass> hatchPasses_;

    /* SAID ONCE, AT THE FIRST FRAME. See IScenePass.hpp: a hatch used for
     * something ordinary is a signal that the engine is missing a feature, and
     * the signal is worthless if nobody can see it. */
    bool hatchReported_ = false;

    rhi::TextureHandle  shadowDepth_;

    /* Half the depth map, RGBA8 — a colour rather than a fraction, so every
     * material means what its .mat says instead of sharing one global tint.
     * Same memory as a single-channel plane at full size. */
    rhi::TextureHandle  shadowTransmission_;
    rhi::ShaderHandle   transmissionShader_;
    rhi::PipelineHandle transmissionPipeline_;
    rhi::ShaderHandle   depthShader_;
    rhi::PipelineHandle depthPipeline_;
    rhi::BufferHandle   passBlock_;

    /* ---- the G-buffer -------------------------------------------------- */
    rhi::TextureHandle  sceneDepth_;
    rhi::TextureHandle  sceneNormals_;
    rhi::ShaderHandle   prepassShader_;
    rhi::PipelineHandle prepassPipeline_;
    rhi::BufferHandle   materialBlock_;

    /* ---- ambient occlusion ---------------------------------------------
     *
     * THE FIRST PASS THAT READS ANOTHER'S OUTPUT, which is why it is the first
     * to need samplers. Both are NEAREST and clamped: the depth and normal
     * planes are read at exactly the pixel being shaded, and filtering between
     * two neighbouring normals produces a direction that is neither. */
    rhi::TextureHandle  occlusion_;
    rhi::ShaderHandle   occlusionShader_;
    rhi::PipelineHandle occlusionPipeline_;

    /* ---- and the blur that finishes it ----------------------------------
     *
     * NOT AN OPTIONAL SECOND HALF. The occlusion pass rotates its kernel per
     * pixel to turn banding into noise, and this is what removes the noise;
     * shipping one without the other leaves a grainy four-pixel field over
     * every lit surface. It was missing here for several rounds and the
     * artefact was read as a shadow-filtering problem, because that is what it
     * looks like. See rhi/ssao_blur.fs.glsl.
     *
     * A SECOND TARGET rather than a blur in place: a bilateral filter reads its
     * neighbours, so writing into the texture being read would feed already
     * blurred pixels back in on one side and not the other. */
    /* ---- the DBuffer ----------------------------------------------------
     *
     * HALF THE SCENE TARGET, which is the window's own resolution given the 2x
     * supersample. The lit pass samples these bilinearly, which lands decal
     * detail at display resolution — where it is seen. The supersampling exists
     * for the hard geometric edges of untextured boxes, not for texture detail,
     * and a DBuffer at the full scene size would be 48 MB to carry a signal
     * nothing can resolve.
     *
     * THE ALBEDO PLANE IS RGBA16F AND THE OTHER TWO ARE RGBA8, and that split
     * is the one deliberate divergence from the raylib DBuffer — see
     * rhi/dbuffer.glsl, which carries the argument. Colour needs the range;
     * a normal and three 0..1 scalars do not. */
    rhi::TextureHandle  decalAlbedo_;
    rhi::TextureHandle  decalNormal_;
    rhi::TextureHandle  decalSurface_;

    rhi::ShaderHandle   decalShader_;
    rhi::PipelineHandle decalPipeline_;

    /* THE PASS'S OWN BLOCK, and one buffer holding EVERY decal's object block.
     *
     * ONE UPLOAD PER FRAME AND A BIND PER DRAW, rather than an upload per draw.
     * Rewriting one buffer between draws inside a pass is a pipeline stall on
     * every backend that means it, and on a console the kind that does not show
     * up until the frame is built around it — DeviceMaterials refuses the same
     * shape for the same reason. `bindUniformBuffer` takes an offset, so the
     * decals go up together and each draw points at its own slice. */
    rhi::BufferHandle   decalPassBlock_;
    rhi::BufferHandle   decalObjectBlocks_;
    uint32_t            decalObjectCapacity_ = 0;

    /* THE PROJECTOR BOX. A unit cube, positions only — nothing reads a normal,
     * a UV or a colour from it, because the box's own faces are never shaded.
     * The engine's, not the game's: a decal pass without a box to rasterise is
     * not a pass, and asking a project to supply one would be asking it what a
     * decal is. */
    rhi::BufferHandle   decalCubeVertices_;
    rhi::MeshHandle     decalCube_;

    /* KEPT ACROSS FRAMES FOR ITS CAPACITY. Same arrangement as debugScratch_. */
    std::vector<std::uint8_t> decalScratch_;

    /* ---- custom depth / stencil ------------------------------------------
     *
     * AT THE SURFACE'S RESOLUTION, not the scene's. The consumer is a
     * post-resolve pass working in output pixels, so anything finer would be
     * downsampled before it was read — and the depth comparison against
     * sceneDepth_ still works across the two resolutions because a depth value
     * is projection-space and carries no resolution with it.
     *
     * ALLOCATED WHATEVER `features.customDepth` SAYS. Making it conditional
     * would mean a resize path that has to know which views want it, and this
     * is 5 MB on a 1280x800 surface. If that ever matters it is a quality
     * preset, not a per-frame branch — see §4.11. */
    rhi::TextureHandle  customStencil_;
    rhi::TextureHandle  customDepth_;
    rhi::ShaderHandle   customStencilShader_;
    rhi::PipelineHandle customStencilPipeline_;
    rhi::BufferHandle   customStencilBlock_;

    /* ITS OWN MATRIX BUFFER rather than passBlock_'s, which the shadow pass
     * fills with the SUN's. Sharing would work today because the passes run in
     * order and this one re-uploads — and it would break silently the first
     * time somebody reordered them, with every tagged object rasterised from
     * the sun's point of view into a buffer the outline reads as though it were
     * the camera's. Sixty-four bytes to make that unexpressible. */
    rhi::ShaderHandle   outlineShader_;
    rhi::PipelineHandle outlinePipeline_;
    rhi::BufferHandle   outlineBlock_;
    SceneDrawList       customList_;

    rhi::TextureHandle  occlusionBlurred_;
    rhi::ShaderHandle   blurShader_;
    rhi::PipelineHandle blurPipeline_;
    rhi::BufferHandle   blurBlock_;
    rhi::SamplerHandle  pointSampler_;

    /* Bilinear and clamped, for the one read that spans two resolutions — the
     * resolve's downscale off the supersampled scene target. */
    rhi::SamplerHandle  linearSampler_;
    rhi::BufferHandle   kernelBlock_;

    /* ITS OWN BUFFER, sharing binding 1 with the geometry passes but not their
     * storage. Binding 1 means "the block this pass reads", and a screen-space
     * pass needs three matrices where a geometry pass needs one — the pipelines
     * are separate so nothing can read the wrong layout. Reusing one buffer for
     * both was a 224-byte write into 64 bytes, which the device refused. */
    rhi::BufferHandle   occlusionBlock_;

    /* ---- the sky -------------------------------------------------------
     *
     * FIRST INTO THE SCENE TARGET, and it is the reason the lit pass no longer
     * clears the colour attachment: the sky IS the clear. A covering triangle
     * with no depth test, so every geometry pass after it occludes it for free
     * without a skybox mesh, a far plane or a depth-clamp trick. */
    rhi::ShaderHandle   skyShader_;
    rhi::PipelineHandle skyPipeline_;
    rhi::BufferHandle   skyBlock_;

    /* ---- the lit scene and its resolve ---------------------------------
     *
     * RGBA16F, because the pipeline is linear and the sun is genuinely far
     * brighter than a lit wall — eight bits would clip everything above one and
     * flatten the range the tone map exists to compress. */
    rhi::TextureHandle  sceneColour_;
    rhi::ShaderHandle   litShader_;
    rhi::PipelineHandle litPipeline_;
    rhi::BufferHandle   litBlock_;

    /* The translucent material: the lit pipeline with depth-write off and
     * premultiplied blending. One shader for glass, water and anything else
     * whose .mat says `blend translucent`. */
    rhi::ShaderHandle   transparentShader_;
    rhi::PipelineHandle transparentPipeline_;

    /* ---- debug lines ----------------------------------------------------
     *
     * TWO PIPELINES OVER ONE BUFFER, which is the depth test being a baked
     * pipeline state rather than something an encoder can poke. The raylib
     * renderer flips it between two loops and has to flush rlgl's batch in
     * between or the state applies to lines already queued; here the two states
     * are two objects and there is nothing to get wrong.
     *
     * ORDER: depth-tested first, x-ray over the top. An x-ray line that lost to
     * a depth-tested one would not be x-ray. Same reasoning as
     * DebugRenderer.hpp, which is the raylib half of this. */
    rhi::ShaderHandle   debugShader_;
    rhi::PipelineHandle debugDepthPipeline_;
    rhi::PipelineHandle debugXrayPipeline_;

    /* One end of a debug segment: world position and a packed LINEAR colour,
     * sixteen bytes. Nested and private because nothing outside builds one —
     * the queue holds Vec3s and this is the device's shape for them. */
    struct DebugVertex {
        float         x = 0.0f;
        float         y = 0.0f;
        float         z = 0.0f;
        std::uint32_t rgba = 0xFFFFFFFFu;
    };

    rhi::BufferHandle   debugVertices_;
    rhi::MeshHandle     debugMesh_;
    uint32_t            debugCapacity_ = 0;

    /* KEPT ACROSS FRAMES FOR ITS CAPACITY, cleared rather than freed. A debug
     * frame is rebuilt every frame from a queue that is itself rebuilt every
     * frame, so this settles at the high-water mark and then allocates
     * nothing. */
    std::vector<DebugVertex> debugScratch_;

    /* ---- the reflection probes ------------------------------------------
     *
     * THE SET ITSELF IS THE SCENE'S — it describes a world — and the two
     * pipelines below are this object's, because a pipeline object is pass
     * state and the probe set does not open passes. That split is why
     * DeviceProbeSet names no graphics API at all; see its header on how it
     * differs from the raylib ReflectionProbeSet, which has to drive the draw
     * itself. */

    /* THE LIT PIPELINE WITH THE PREPASS TAKEN OUT, and that is the only
     * difference. The camera's lit pass tests Equal against a depth prepass and
     * writes no depth, which is an optimisation available only because the
     * prepass drew exactly the same geometry. A cube face has no prepass — one
     * would cost a second submission of the world per face to save nothing at
     * 128 pixels — so this tests Less and writes depth like an ordinary forward
     * pass. Reusing the camera's pipeline here draws nothing at all: every
     * fragment fails an Equal test against a cleared buffer. */
    rhi::PipelineHandle probeLitPipeline_;

    /* And the translucent half. LessEqual and premultiplied, exactly as the
     * camera's transparent pipeline — a pane in a reflection composites over
     * the room behind it or its coverage alpha lands in the cubemap as a
     * sky-bright hole. The camera's would work; this one exists because it must
     * write depth-test against a buffer this pass wrote rather than one a
     * prepass did, and keeping the pair adjacent is what stops the two drifting
     * apart when the lit pipeline is next edited. */
    rhi::PipelineHandle probeTransparentPipeline_;

    /* A 1x1 WHITE, standing in for the occlusion plane inside a capture. SSAO
     * is screen space — its texture is addressed by gl_FragCoord against the
     * SCENE's size, and inside a 128-pixel cubemap face those coordinates mean
     * nothing. White is "nothing is occluded", which is the honest answer for a
     * pass that has no depth prepass to occlude against. The shaders clamp
     * their fetch to the texture's size so one texel serves. */
    rhi::TextureHandle  whitePixel_;

    /* ---- the probe prefilter --------------------------------------------
     *
     * A screen-space pass with no vertex buffer, run once per (face, level) of
     * one probe. Its block carries the level's roughness, the sample count, the
     * probe layer and the face — see rhi/probe_face.glsl. */
    rhi::ShaderHandle   prefilterShader_;
    rhi::PipelineHandle prefilterPipeline_;
    rhi::BufferHandle   prefilterBlock_;

    /* ---- bloom ----------------------------------------------------------
     *
     * HALF THE SCENE TARGET AND SIX LEVELS. Both are budgets in the §4.11
     * sense: named constants in one place so a quality preset can move them,
     * never assumed by a shader — every stage is told its source and target
     * size, so shortening the chain is a number rather than an edit.
     *
     * RGBA16F, matching the scene target it reads and writes. An 8-bit chain
     * would clip the very values bloom exists to spread, which is the one
     * format mistake available here and the one that makes a bright light's
     * halo the same brightness as a dim one's. */
    rhi::TextureHandle  bloomChain_;
    rhi::ShaderHandle   bloomPrefilterShader_;
    rhi::PipelineHandle bloomPrefilterPipeline_;
    rhi::ShaderHandle   bloomDownShader_;
    rhi::PipelineHandle bloomDownPipeline_;
    rhi::ShaderHandle   bloomUpShader_;

    /* TWO PIPELINES OVER ONE SHADER, and the difference is the blend state — a
     * baked pipeline property rather than something an encoder can poke. The
     * chain's upsamples ACCUMULATE onto the level they land in; the composite
     * accumulates onto the scene. Both are One/One and neither replaces, so
     * this pair exists only because the two write different formats. */
    rhi::PipelineHandle bloomUpPipeline_;
    rhi::PipelineHandle bloomCompositePipeline_;
    rhi::BufferHandle   bloomBlock_;

    /* ONE SAMPLER PER SOURCE LEVEL, LOD-clamped to exactly that level.
     *
     * NOT AN OPTIMISATION AND NOT PEDANTRY: a texture that is simultaneously a
     * colour attachment and a live sampler read is undefined unless the sampler
     * provably cannot reach the level being written, and every stage of this
     * chain reads one level of bloomChain_ while writing another. Same two
     * floats DeviceProbeSet's prefilter needed, for the same reason. */
    std::vector<rhi::SamplerHandle> bloomLevelSamplers_;

    rhi::ShaderHandle   resolveShader_;
    rhi::PipelineHandle resolvePipeline_;
    rhi::BufferHandle   resolveBlock_;

    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;

    /* Defaults to 4x rather than to the scene's 2x, because 2x is the setting
     * that was measured and found wanting — see withOutlineSupersample. A
     * machine that cannot spare the memory turns it down; the header explains
     * what that costs and what it buys. */
    uint32_t outlineSupersample_ = 4;
};

}  // namespace cromwell
