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
 * WHAT IS THE GAME'S: the meshes, and where the world is. Both arrive through
 * IGeometrySource, which is one interface with two methods.
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
 *   decals        not yet
 *   probes        done (one cubemap per room in a cube array, captured round
 *                 robin a face at a time, parallax-corrected and selected per
 *                 pixel - no prefiltered mip chain, so the term slides back to
 *                 the analytic sky as roughness rises)
 *   lit scene     done (untextured - real sun and sky, shadow, occlusion,
 *                 vertex colour; no albedo/normal/roughness maps yet)
 *   tone map      done (and it resolves the 2x supersample - see kSupersample)
 *
 * Passes are added here as they are converted. The game-side renderer that
 * drives this stays the same size as each one lands, which is the property
 * worth watching.
 */
#pragma once

#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

/* Forward-declared rather than included: SceneFrame only carries a pointer to
 * one, and DebugDraw drags in the whole segment queue for callers that never
 * ask for a debug line. */
class DebugDraw;

class IGeometrySource;
namespace rhi { class IRenderDevice; }

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

    /* The viewpoint the camera passes draw from, once there are any. */
    Mat4 view;
    Mat4 projection;

    /* Where the eye is, in world space. Derivable from `view` by inverting it,
     * and carried anyway — the lit pass needs it every fragment for the view
     * vector, and inverting a matrix to recover a number the caller already had
     * is work done to lose information. */
    Vec3 cameraPosition;

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
    explicit ScenePipeline(rhi::IRenderDevice& device);
    ~ScenePipeline();

    ScenePipeline(const ScenePipeline&) = delete;
    ScenePipeline& operator=(const ScenePipeline&) = delete;

    /* Loads shaders and builds targets. Call once, after the device exists.
     * False means the frame cannot be drawn — it has already logged which stage
     * failed, and the caller should fall back rather than retry. */
    bool initialise();

    /* One frame. `geometry` supplies the meshes and the world's extent. */
    void render(const SceneFrame& frame, IGeometrySource& geometry);

    /* THE FRAME'S INTERMEDIATES, for a later pass to sample and for a
     * diagnostic to look at. Invalid until initialise() has succeeded.
     *
     * The G-buffer's ALPHA IS ROUGHNESS, not coverage — see the converted
     * prepass fragment shader. Anything sampling it should know that before it
     * reads the fourth channel. */
    rhi::TextureHandle shadowMap() const { return shadowDepth_; }
    rhi::TextureHandle sceneDepth() const { return sceneDepth_; }
    rhi::TextureHandle sceneNormals() const { return sceneNormals_; }
    /* THE BLURRED ONE, which is what the lit pass samples and what a diagnostic
     * should show. The raw plane is deliberately not exposed: it is noise by
     * construction and looking at it invites the conclusion that the occlusion
     * pass is broken. */
    rhi::TextureHandle occlusion() const { return occlusionBlurred_; }
    rhi::TextureHandle sceneColour() const { return sceneColour_; }

    /* The prepass and everything reconstructed from it are sized to the
     * surface, so a resize rebuilds them. Cheap to call every frame — it
     * returns immediately when the size has not changed. */
    void resize(uint32_t width, uint32_t height);

    /* THE MATERIALS, for the game to author and for its submitter to bind.
     *
     * Non-const because what a surface is made of is the GAME's to say — the
     * pipeline owns the blocks and the binding index, not the values in them.
     * The submitter passes this back down to bind per bucket; see
     * IGeometrySource and RhiStatics::submit. */
    DeviceMaterials&       materials()       { return materials_; }
    const DeviceMaterials& materials() const { return materials_; }

    /* THE REFLECTION PROBES, for the game to PLACE and for a diagnostic to
     * read. Non-const for the same reason materials() is: the pipeline owns the
     * array texture, the schedule and the capture pass, and none of those know
     * what a room is. Which volumes exist is a question about the game's world
     * — a flooded cell partition here, a portal graph or hand-placed volumes
     * elsewhere — so the game calls clear() and addProbe() and the engine never
     * learns why. See game/render/scene/ProbePlacement.hpp. */
    DeviceProbeSet&       probes()       { return probes_; }
    const DeviceProbeSet& probes() const { return probes_; }

private:
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
    static SunProjection sunProjection(const SceneFrame& frame, Vec3 minimum, Vec3 maximum);

    bool createSceneTargets(uint32_t surfaceWidth, uint32_t surfaceHeight);

    /* THE SCENE TARGETS' SIZE, which is the surface's times the supersample
     * factor. Named rather than multiplied at each use: `targetWidth_` holds
     * the SURFACE's width, and every pass except the resolve works in scene
     * pixels — so the mistake available here is reaching for the member and
     * being wrong by a factor of two, silently, in a screen-space pass. */
    uint32_t sceneWidth() const;
    uint32_t sceneHeight() const;

    void drawShadowMap(const SceneFrame& frame, IGeometrySource& geometry);
    void drawPrepass(const SceneFrame& frame, IGeometrySource& geometry);
    void drawOcclusion(const SceneFrame& frame);
    void drawOcclusionBlur(const SceneFrame& frame);
    void drawSky(const SceneFrame& frame);
    void drawLitScene(const SceneFrame& frame, IGeometrySource& geometry);

    /* What you can see through, after the opaque scene. Blend mode is a
     * MATERIAL property, so which surfaces land here is authored rather than
     * coded — see DeviceMaterials::isTranslucent. */
    void drawTransparent(const SceneFrame& frame, IGeometrySource& geometry);

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
    void drawShadowTransmission(const SceneFrame& frame, IGeometrySource& geometry);

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
    void drawProbeCapture(const SceneFrame& frame, IGeometrySource& geometry);

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
    void drawProbePrefilter();
    void drawResolve(const SceneFrame& frame);
    void drawBackbuffer(const SceneFrame& frame);

    rhi::IRenderDevice& device_;
    bool ready_ = false;

    /* One block per surface kind. The pipeline still uploads materialBlock_
     * below as the DEFAULT, so a submitter that binds nothing still draws with
     * a sane material rather than whatever the last pass left at binding 2. */
    DeviceMaterials materials_;

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
     * THE SET OWNS THE ARRAY, THE VOLUMES AND THE SCHEDULE; the two pipelines
     * below are the pipeline's, because a pipeline object is pass state and the
     * probe set does not open passes. That split is why DeviceProbeSet names no
     * graphics API at all — see its header on how it differs from the raylib
     * ReflectionProbeSet, which has to drive the draw itself. */
    DeviceProbeSet      probes_;

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

    rhi::ShaderHandle   resolveShader_;
    rhi::PipelineHandle resolvePipeline_;
    rhi::BufferHandle   resolveBlock_;

    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;
};

}  // namespace cromwell
