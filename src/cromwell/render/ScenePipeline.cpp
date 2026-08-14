#include "cromwell/render/ScenePipeline.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/render/IGeometrySource.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>

namespace cromwell {
namespace {

using namespace cromwell::rhi;

/* 4096, matching the raylib path — and it only became worth the memory once the
 * projection was focused on the camera's frustum rather than on the whole
 * world. Spread over a sixty-tile map, twice the resolution buys twice as fine
 * a staircase; spread over what the camera can see, it buys an edge.
 *
 * 64 MB at D32F. That is a lot for one buffer and it is the correct trade here:
 * a tactical game is read at a fixed, fairly close zoom, so there is no cascade
 * to spend it on and the single focused map does the whole job. */
constexpr uint32_t kShadowSize = 4096;

/* HALF THE SHADOW MAP, and that is what pays for the extra channels.
 *
 * The transmission plane holds what the sun becomes crossing anything
 * translucent — a colour, not a fraction, so that every material means what its
 * .mat file says rather than sharing one global tint. At RGBA8 and half size it
 * costs 16 MB, exactly what a single-channel plane at full size would.
 *
 * The two planes answer different questions, which is why the trade is sound:
 * depth resolution buys shadow EDGE sharpness, while transmittance is
 * low-frequency by nature — a whole pane is one value — and is filtered over
 * the PCSS disc before anything sees it. */
constexpr uint32_t kTransmissionSize = kShadowSize / 2;

/* ================= WHY THE SCENE IS DRAWN AT TWICE THE WINDOW ==============
 *
 * TWO ON EACH AXIS, so four samples per output pixel, resolved by the tone map
 * scaling back down. The same factor the raylib path uses (ToneMapPass::
 * kSupersampleFactor) and for the same two reasons — one obvious, one not:
 *
 * THE OBVIOUS ONE: nothing else here antialiases. The board is a few thousand
 * hard-edged untextured boxes, so every silhouette is a staircase at 1:1. There
 * is no MSAA (this renders into its own targets, not the backbuffer) and no
 * temporal filter, and the fill cost of four samples over untextured geometry
 * is nothing next to what those edges look like.
 *
 * THE ONE THAT IS EASY TO MISS: it is also what makes the PCSS filter work.
 * That filter rotates its sample disc by a per-pixel hash precisely so its
 * undersampling arrives as noise rather than as a pattern — and noise is only
 * an improvement if something averages it. At 1:1 nothing does, and the shadow
 * reads as fuzz. Four samples per pixel is what turns it back into softness.
 *
 * So the supersample is not a polish pass bolted on at the end; the filter
 * upstream of it was designed assuming it. Dropping it makes shadows visibly
 * worse in a way that looks like a shadow bug and is not. */
constexpr uint32_t kSupersample = 2;

/* A Vec3 into the first three floats of a std140 vec4, leaving the fourth
 * alone. Named rather than written out three times per block: three
 * consecutive index-by-hand assignments is a shape that reads as correct while
 * carrying a copy-paste `.y` where a `.z` belongs, and that mistake produces a
 * plausible wrong colour rather than a failure. */
void writeVec3(float (&destination)[4], Vec3 value)
{
    destination[0] = value.x;
    destination[1] = value.y;
    destination[2] = value.z;
}

/* std140. One mat4, so no padding is needed — the assert exists anyway, because
 * the moment a second member is added it is the only thing between a mismatched
 * layout and fields that silently read as garbage. See CONVENTIONS.md. */
struct PassBlockData {
    Mat4 viewProjection;
};
static_assert(sizeof(PassBlockData) == 64, "std140: mat4 is 16 floats, tightly packed");

/* std140 again, and this one is the shape the padding rule bites on: the shader
 * declares vec4 rather than a bare float precisely so this struct is sixteen
 * bytes rather than four with twelve of invisible padding after it. Declaring
 * `float uRoughness;` there and `float roughness;` here would agree in C++ and
 * disagree with the GPU the moment a second member was added. */
struct MaterialBlockData {
    /* THE SAME EIGHTY BYTES DeviceMaterials writes, and it must stay that way:
     * this is the DEFAULT the pipeline uploads for anything the game does not
     * bind a material for, and both are bound to the same slot reading the same
     * declaration in rhi/material_block.glsl. */
    float factors[4]      = { 0.8f, 0.0f, 1.0f, 1.0f };
    float options[4]      = { 0.0f, 0.0f, 0.5f, 0.0f };
    float opacity[4]      = { 0.08f, 4.0f, 1.0f, 1.0f };
    float tint[4]         = { 1.0f, 1.0f, 1.0f, 0.0f };
    float transmission[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    float sunTransmittance[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
};
static_assert(sizeof(MaterialBlockData) == 96, "matches DeviceMaterials");

/* THE PASS BLOCK THE OCCLUSION SHADER READS. Three matrices and two packed
 * vec4s — and the packing is the point: `uResolutionAndRadius` carries four
 * unrelated scalars because a std140 block pads every member to sixteen bytes
 * anyway, so four floats declared separately would occupy sixty-four. */
struct OcclusionBlockData {
    Mat4  projection;
    Mat4  inverseProjection;
    Mat4  view;
    float resolutionAndRadius[4] = { 0.0f, 0.0f, 0.9f, 0.025f };
    float strength[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(OcclusionBlockData) == 224, "std140: 3 mat4 + 2 vec4");

/* THE SKY'S BLOCK. One matrix to turn a pixel back into a view ray, and the
 * same environment colours the lit pass reads — deliberately the same numbers,
 * because a backdrop that disagreed with the light in front of it is the one
 * mistake this whole arrangement exists to prevent. */
struct SkyBlockData {
    Mat4  inverseViewProjection;
    float resolution[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    float sunDirection[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
    float sunColour[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    float skyZenith[4] = { 0.16f, 0.29f, 0.52f, 0.0f };
    float skyHorizon[4] = { 0.52f, 0.62f, 0.75f, 0.0f };
    float skyGround[4] = { 0.13f, 0.12f, 0.10f, 0.0f };
};
static_assert(sizeof(SkyBlockData) == 160, "std140: mat4 + 6 vec4");

/* THE BLUR'S BLOCK. One matrix — the bilateral rejection unprojects each tap's
 * depth to compare it in view space — and the plane's size. */
struct BlurBlockData {
    Mat4  inverseProjection;
    float resolution[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
};
static_assert(sizeof(BlurBlockData) == 80, "std140: mat4 + vec4");

/* THE LIT PASS'S BLOCK. Two matrices, then seven vec4s carrying what would
 * otherwise be a couple of dozen loose scalars — std140 pads every member to
 * sixteen bytes, so packing them is free and declaring them separately is not.
 *
 * THE COLOURS ARE vec4 AND THE SHADER READS .rgb. A vec3 in std140 is padded to
 * sixteen bytes anyway, so the fourth float costs nothing and declaring the C++
 * side as three floats would put every member after it at a different offset
 * from the one the GPU reads. That mismatch has no diagnostic: the block simply
 * returns the wrong numbers from the first vec3 onwards. */
struct LitBlockData {
    Mat4  viewProjection;
    Mat4  sunViewProjection;
    float sunDirection[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
    float cameraPosition[4] = {};
    float sunRadiance[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    float skyZenith[4] = { 0.16f, 0.29f, 0.52f, 0.0f };
    float skyHorizon[4] = { 0.52f, 0.62f, 0.75f, 0.0f };
    float skyGround[4] = { 0.13f, 0.12f, 0.10f, 0.0f };
    float exposureAndAmbient[4] = { 1.0f, 0.42f, 0.0f, 0.0f };

    /* THE SHADOW MAP'S SCALES, and every one of them is a unit conversion the
     * shader cannot do for itself.
     *
     *   x  one texel in UV          how far a filter tap steps
     *   y  one texel in WORLD units how far the normal offset pushes
     *   z  the depth range in world what a normalised depth bias means
     *   w  tan(sun angular radius)  how wide a penumbra grows with distance
     *
     * Packed into one vec4 because std140 pads every member to sixteen bytes, so
     * four floats declared separately would occupy sixty-four. */
    float shadowScales[4] = { 1.0f, 1.0f, 1.0f, 0.0055f };

    /* x = HOW MANY REFLECTION PROBES ARE LIVE. Per PASS rather than per frame,
     * which is why it lives here and not in DeviceProbeSet's volume block: a
     * probe capture sets it to zero because the array it renders into cannot
     * also be sampled. See rhi/probes.glsl. yzw spare. */
    float probeParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(LitBlockData) == 272, "std140: 2 mat4 + 9 vec4");

struct ResolveBlockData {
    float exposureAndFlags[4] = { 1.0f, 1.0f, 0.0f, 0.0f };   /* z = debug view */

    /* One OUTPUT pixel in UV — the reciprocal of the backbuffer's size, not the
     * scene target's. The resolve is the one pass that spans both resolutions;
     * see kSupersample. zw spare. */
    float outputTexel[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
};
static_assert(sizeof(ResolveBlockData) == 32, "std140: two vec4");

/* THE PREFILTER'S BLOCK — see rhi/probe_face.glsl, which declares its GLSL
 * half. Two vec4s because std140 pads every member to sixteen bytes, so six
 * loose scalars would occupy ninety-six. */
struct PrefilterBlockData {
    float parameters[4] = { 0.0f, 32.0f, 0.0f, 0.0f };  /* roughness, samples, layer, face */
    float faceSize[4]   = { 128.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(PrefilterBlockData) == 32, "std140: two vec4");

/* HOW MANY GGX TAPS PER TEXEL. Karis' presentation uses 1024 for an offline
 * bake; this runs inside a frame, on a probe that is already one of several, so
 * it buys most of the quality for a thirty-second of the cost.
 *
 * IT IS AFFORDABLE BECAUSE EACH LEVEL READS THE ONE ABOVE IT rather than level
 * zero — the source is already partly convolved, so the lobe it is integrating
 * is much narrower than the roughness suggests. Sampling level 0 at every level
 * would need the full 1024 and would still be noisier. */
constexpr int kPrefilterSamples = 32;

/* THE SAMPLE KERNEL. vec4 rather than vec3 because std140 pads every ARRAY
 * ELEMENT to sixteen bytes — a vec3[24] on the GPU has vec4 stride, so a
 * tightly-packed C++ vec3 array would misalign from the fourth element onward
 * and read garbage. The w component is that padding, not data. */
constexpr int kKernelSize = 24;
struct KernelBlockData {
    float samples[kKernelSize][4] = {};
};
static_assert(sizeof(KernelBlockData) == kKernelSize * 16, "std140: vec4 stride per element");

/* A COSINE-WEIGHTED HEMISPHERE, scaled so taps cluster near the shading point.
 *
 * Deterministic rather than random: the same kernel every run means a frame
 * looks identical between launches, which is what makes an A/B comparison
 * against the raylib renderer meaningful at all. The scale curve is the
 * standard one — quadratic, so most samples land close where contact darkening
 * lives, and few reach the full radius. */
KernelBlockData buildKernel()
{
    KernelBlockData kernel;

    /* A fixed low-discrepancy sequence beats rand(): it distributes over the
     * hemisphere evenly rather than clumping, so 24 taps do the work that a
     * random set would need many more of. */
    for (int i = 0; i < kKernelSize; i++) {
        const float index = static_cast<float>(i);

        /* Golden-angle spiral in the xy plane, lifted onto the hemisphere. */
        const float angle = index * 2.39996323f;
        const float radial = std::sqrt((index + 0.5f) / static_cast<float>(kKernelSize));
        const float z = std::sqrt(std::max(0.0f, 1.0f - radial * radial));

        float scale = index / static_cast<float>(kKernelSize);
        scale = 0.1f + 0.9f * scale * scale;

        kernel.samples[i][0] = std::cos(angle) * radial * scale;
        kernel.samples[i][1] = std::sin(angle) * radial * scale;
        kernel.samples[i][2] = z * scale;
        kernel.samples[i][3] = 0.0f;
    }
    return kernel;
}

/* EVERYTHING IN THE LIT BLOCK THAT IS THE FRAME'S RATHER THAN THE PASS'S.
 *
 * THREE PASSES FILL THIS BLOCK — the lit scene, the transparent scene and every
 * probe capture face — and they differ in exactly two members: which matrix
 * they draw through and where the eye is. Everything else is one world's sun,
 * one sky and one shadow fit.
 *
 * WRITTEN ONCE HERE RATHER THAN TWICE AT THE CALL SITES, because the failure
 * mode of the alternative is already recorded in MIGRATION.md's traps: the SSAO
 * pass invented radius, bias and strength instead of borrowing them, and the
 * result read as a broken port rather than as mistuning. A probe capture that
 * lit its world from a slightly different sun would be the same mistake with a
 * much better hiding place, because nobody is looking at the inside of a
 * cubemap. Borrow, do not retype.
 *
 * The caller overrides `viewProjection`, `cameraPosition` and `probeParams`
 * after this returns — those three ARE the pass's. */
LitBlockData buildLitBlock(const SceneFrame& frame, const Mat4& sunViewProjection,
                           float worldTexelSize, float depthRange)
{
    LitBlockData block;

    block.sunViewProjection = sunViewProjection;

    block.shadowScales[0] = 1.0f / static_cast<float>(kShadowSize);
    block.shadowScales[1] = worldTexelSize;
    block.shadowScales[2] = depthRange;
    block.shadowScales[3] = std::tan(frame.sunAngularRadius);

    writeVec3(block.sunDirection, frame.sunDirection.normalised());

    /* THE ENVIRONMENT, COPIED RATHER THAN INTERPRETED. The pipeline does not
     * decide what colour the sun is — that is a property of the world's time of
     * day, which the game owns and SunLight computes. Every number here travels
     * straight through from SceneFrame, which is what makes the same pipeline
     * correct under a different sky model. */
    writeVec3(block.sunRadiance, frame.sunRadiance);
    writeVec3(block.skyZenith,   frame.skyZenith);
    writeVec3(block.skyHorizon,  frame.skyHorizon);
    writeVec3(block.skyGround,   frame.skyGround);

    block.exposureAndAmbient[0] = frame.exposure;
    block.exposureAndAmbient[1] = frame.ambientIntensity;

    return block;
}

}  // namespace

ScenePipeline::ScenePipeline(rhi::IRenderDevice& device)
    : device_(device), materials_(device), probes_(device)
{
}

ScenePipeline::~ScenePipeline()
{
    if (transmissionPipeline_.valid()) device_.destroy(transmissionPipeline_);
    if (transmissionShader_.valid())   device_.destroy(transmissionShader_);
    if (shadowTransmission_.valid())   device_.destroy(shadowTransmission_);

    if (depthPipeline_.valid())   device_.destroy(depthPipeline_);
    if (depthShader_.valid())     device_.destroy(depthShader_);
    if (shadowDepth_.valid())     device_.destroy(shadowDepth_);
    if (passBlock_.valid())       device_.destroy(passBlock_);

    if (prepassPipeline_.valid()) device_.destroy(prepassPipeline_);
    if (prepassShader_.valid())   device_.destroy(prepassShader_);
    if (materialBlock_.valid())   device_.destroy(materialBlock_);
    if (sceneDepth_.valid())      device_.destroy(sceneDepth_);
    if (sceneNormals_.valid())    device_.destroy(sceneNormals_);

    if (occlusionPipeline_.valid()) device_.destroy(occlusionPipeline_);
    if (occlusionShader_.valid())   device_.destroy(occlusionShader_);
    if (pointSampler_.valid())      device_.destroy(pointSampler_);
    if (linearSampler_.valid())     device_.destroy(linearSampler_);
    if (kernelBlock_.valid())       device_.destroy(kernelBlock_);
    if (occlusionBlock_.valid())    device_.destroy(occlusionBlock_);
    if (occlusion_.valid())         device_.destroy(occlusion_);

    if (skyPipeline_.valid())       device_.destroy(skyPipeline_);
    if (skyShader_.valid())         device_.destroy(skyShader_);
    if (skyBlock_.valid())          device_.destroy(skyBlock_);

    if (blurPipeline_.valid())      device_.destroy(blurPipeline_);
    if (blurShader_.valid())        device_.destroy(blurShader_);
    if (blurBlock_.valid())         device_.destroy(blurBlock_);
    if (occlusionBlurred_.valid())  device_.destroy(occlusionBlurred_);

    if (transparentPipeline_.valid()) device_.destroy(transparentPipeline_);
    if (transparentShader_.valid())   device_.destroy(transparentShader_);

    /* The probe pipelines only — the array, its sampler and its block belong to
     * DeviceProbeSet, whose destructor runs after this one. */
    if (probeLitPipeline_.valid())         device_.destroy(probeLitPipeline_);
    if (probeTransparentPipeline_.valid()) device_.destroy(probeTransparentPipeline_);
    if (whitePixel_.valid())               device_.destroy(whitePixel_);
    if (prefilterPipeline_.valid())        device_.destroy(prefilterPipeline_);
    if (prefilterShader_.valid())          device_.destroy(prefilterShader_);
    if (prefilterBlock_.valid())           device_.destroy(prefilterBlock_);

    if (litPipeline_.valid())     device_.destroy(litPipeline_);
    if (litShader_.valid())       device_.destroy(litShader_);
    if (litBlock_.valid())        device_.destroy(litBlock_);
    if (sceneColour_.valid())     device_.destroy(sceneColour_);

    if (resolvePipeline_.valid()) device_.destroy(resolvePipeline_);
    if (resolveShader_.valid())   device_.destroy(resolveShader_);
    if (resolveBlock_.valid())    device_.destroy(resolveBlock_);
}

uint32_t ScenePipeline::sceneWidth() const  { return targetWidth_ * kSupersample; }
uint32_t ScenePipeline::sceneHeight() const { return targetHeight_ * kSupersample; }

bool ScenePipeline::createSceneTargets(uint32_t surfaceWidth, uint32_t surfaceHeight)
{
    /* EVERY SCENE TARGET IS SUPERSAMPLED; only the backbuffer is not. The
     * resolve is where the two resolutions meet, and it is the only pass that
     * has to know there are two. */
    const uint32_t width  = surfaceWidth * kSupersample;
    const uint32_t height = surfaceHeight * kSupersample;

    if (sceneDepth_.valid())   device_.destroy(sceneDepth_);
    if (sceneNormals_.valid()) device_.destroy(sceneNormals_);

    TextureDesc depth;
    depth.name   = "scene depth";
    depth.width  = width;
    depth.height = height;
    depth.format = TextureFormat::D32F;
    depth.usage  = TextureUsageSampled | TextureUsageDepthTarget;
    sceneDepth_ = device_.createTexture(depth);

    TextureDesc normals;
    normals.name   = "scene normals";
    normals.width  = width;
    normals.height = height;

    /* RGBA8, and the alpha is ROUGHNESS. Eight bits per channel is enough for a
     * normal that only orients an occlusion hemisphere; the lit pass computes
     * its own from the geometry rather than reading this one back. */
    normals.format = TextureFormat::RGBA8;
    normals.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    sceneNormals_ = device_.createTexture(normals);

    /* THE LIT SCENE, in RGBA16F. The pipeline is linear and the sun is
     * genuinely far brighter than a lit wall, so eight bits would clip
     * everything above one and flatten exactly the range the tone map exists to
     * compress. */
    TextureDesc colour;
    colour.name   = "scene colour";
    colour.width  = width;
    colour.height = height;
    colour.format = TextureFormat::RGBA16F;
    colour.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    if (sceneColour_.valid()) device_.destroy(sceneColour_);
    sceneColour_ = device_.createTexture(colour);

    /* THE OCCLUSION PLANE. RGBA8 rather than R8 because readTexture and the
     * texture previews both read RGBA, and one channel of a screen-sized target
     * is not the memory this frame is short of. */
    TextureDesc ao;
    ao.name   = "occlusion";
    ao.width  = width;
    ao.height = height;
    ao.format = TextureFormat::RGBA8;
    ao.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    if (occlusion_.valid()) device_.destroy(occlusion_);
    occlusion_ = device_.createTexture(ao);

    /* THE BLUR'S DESTINATION, same format and size. Two targets rather than one
     * because a bilateral filter reads its neighbours — blurring in place would
     * feed already-blurred pixels back in from one side and raw ones from the
     * other, which is a directional smear rather than a blur. */
    ao.name = "occlusion blurred";
    if (occlusionBlurred_.valid()) device_.destroy(occlusionBlurred_);
    occlusionBlurred_ = device_.createTexture(ao);

    /* THE SURFACE'S SIZE, NOT THE TARGETS'. This is what resize() compares
     * against and what the resolve divides by, and both want the number of real
     * pixels on screen. Storing the supersampled size here instead would make
     * the resolve sample a quarter of the image and every future reader of these
     * fields wrong by a factor of two. */
    targetWidth_  = surfaceWidth;
    targetHeight_ = surfaceHeight;

    return sceneDepth_.valid() && sceneNormals_.valid() && occlusion_.valid()
        && occlusionBlurred_.valid() && sceneColour_.valid();
}

void ScenePipeline::resize(uint32_t width, uint32_t height)
{
    if (!ready_ || width == 0 || height == 0) return;
    if (width == targetWidth_ && height == targetHeight_) return;

    if (!createSceneTargets(width, height))
        LOGGER.error("ScenePipeline: could not resize the scene targets to {}x{}", width, height);
}

bool ScenePipeline::initialise()
{
    if (ready_) return true;

    /* THE SAME LOADER THE raylib PATH USES, for the source text only — it
     * probes the asset roots and splices #include, which the new dialect still
     * uses. What it does NOT do here is compile: a device shader is the
     * device's to build. */
    const std::string vertexSource   = ShaderLibrary::preprocess("rhi/depth_only.vs.glsl");
    const std::string fragmentSource = ShaderLibrary::preprocess("rhi/depth_only.fs.glsl");

    if (vertexSource.empty() || fragmentSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/depth_only shaders not found");
        return false;
    }

    depthShader_ = device_.createShader("depth_only", vertexSource.c_str(),
                                        fragmentSource.c_str());
    if (!depthShader_.valid()) return false;

    TextureDesc shadow;
    shadow.name   = "shadow map";
    shadow.width  = kShadowSize;
    shadow.height = kShadowSize;

    /* D32F, not D24S8: there is no stencil to carry, and a 32-bit float depth
     * is what makes reverse-Z available later without changing the format. */
    shadow.format = TextureFormat::D32F;
    shadow.usage  = TextureUsageSampled | TextureUsageDepthTarget;

    shadowDepth_ = device_.createTexture(shadow);
    if (!shadowDepth_.valid()) return false;

    /* THE TRANSMISSION PLANE — what the sun becomes crossing anything
     * translucent, so a window casts a coloured patch rather than a hole.
     *
     * A SEPARATE TARGET, NOT AN ATTACHMENT ON THE SHADOW PASS, and it has to be:
     * GL clips a framebuffer to the smallest of its attachments, so hanging a
     * half-size plane off the shadow pass would render the depth map at half
     * resolution too. The pass that fills this does its own depth compare
     * against the map instead — see rhi/transmission.fs.glsl. */
    TextureDesc transmission;
    transmission.name   = "shadow transmission";
    transmission.width  = kTransmissionSize;
    transmission.height = kTransmissionSize;
    transmission.format = TextureFormat::RGBA8;
    transmission.usage  = TextureUsageSampled | TextureUsageRenderTarget;

    shadowTransmission_ = device_.createTexture(transmission);
    if (!shadowDepth_.valid()) return false;

    PipelineDesc pipeline;
    pipeline.name         = "shadow";
    pipeline.shader       = depthShader_;
    pipeline.vertexLayout = MeshVertexBuffer::deviceLayout();

    /* NO COLOUR ATTACHMENT — a real depth-only pass, which the raylib path
     * cannot express because rlgl has no way to set the draw buffer to NONE. */
    pipeline.colourCount = 0;
    pipeline.depthFormat = TextureFormat::D32F;

    pipeline.depth.test    = true;
    pipeline.depth.write   = true;
    pipeline.depth.compare = CompareFunc::Less;

    /* BACK FACES CULLED, so the map stores the faces the sun can SEE.
     *
     * This was CullMode::Front — second-depth shadow mapping, which stores the
     * far side of every caster and is a genuinely good trick: the bias needed to
     * stop surface acne no longer has to cover the thickness of a wall.
     *
     * It is the wrong choice HERE, for two reasons that are both about the rest
     * of the pipeline rather than about the technique.
     *
     * THE BIASES ARE NOT ITS. Both of the lit pass's — the world-unit depth bias
     * and the normal offset — are lifted from common/shadow.glsl, which is tuned
     * against a map storing front faces. Under second-depth they are far larger
     * than needed, and an over-large bias detaches a shadow from its caster.
     *
     * IT MOVES EVERY BLOCKER. PCSS sizes its penumbra from the distance to the
     * blocker, and second-depth reports every blocker as nearer by the caster's
     * thickness along the sun. Every shadow in the frame comes out sharper than
     * it should be — and a shadow sharper than its texel grid can express is
     * exactly a staircase.
     *
     * The raylib path never sets a cull mode here, so it takes rlgl's default of
     * back-face culling. Matching it is what makes the two comparable at all;
     * revisiting second-depth is a job for after parity, with the biases retuned
     * in the same change. */
    pipeline.raster.cull = CullMode::Back;

    depthPipeline_ = device_.createPipeline(pipeline);
    if (!depthPipeline_.valid()) return false;

    /* ---- the transmission plane's pipeline -------------------------------
     *
     * NO DEPTH ATTACHMENT AND NO DEPTH STATE. The test that would normally keep
     * a pane behind a wall from recording its tint is done in the shader,
     * against the shadow map — because this target is a different size and
     * cannot share the map's framebuffer. See rhi/transmission.fs.glsl.
     *
     * NO CULLING, because a pane is a thin surface the sun may meet from either
     * side and both faces tint equally. */
    const std::string transmissionVertex =
        ShaderLibrary::preprocess("rhi/transmission.vs.glsl");
    const std::string transmissionFragment =
        ShaderLibrary::preprocess("rhi/transmission.fs.glsl");

    if (transmissionVertex.empty() || transmissionFragment.empty()) {
        LOGGER.error("ScenePipeline: rhi/transmission shaders not found");
        return false;
    }

    transmissionShader_ = device_.createShader("transmission", transmissionVertex.c_str(),
                                               transmissionFragment.c_str());
    if (!transmissionShader_.valid()) return false;

    PipelineDesc transmissionPipeline;
    transmissionPipeline.name         = "transmission";
    transmissionPipeline.shader       = transmissionShader_;
    transmissionPipeline.vertexLayout = MeshVertexBuffer::deviceLayout();

    transmissionPipeline.colourFormats[0] = TextureFormat::RGBA8;
    transmissionPipeline.colourCount = 1;

    transmissionPipeline.depth.test  = false;
    transmissionPipeline.depth.write = false;
    transmissionPipeline.raster.cull = CullMode::None;

    transmissionPipeline_ = device_.createPipeline(transmissionPipeline);
    if (!transmissionPipeline_.valid()) return false;

    BufferDesc block;
    block.name   = "pass block";
    block.bytes  = sizeof(PassBlockData);
    block.usage  = BufferUsageUniform;
    block.access = BufferAccess::CpuToGpuPerFrame;

    passBlock_ = device_.createBuffer(block);
    if (!passBlock_.valid()) return false;

    /* ---- the depth prepass --------------------------------------------- */

    const std::string prepassVertex   = ShaderLibrary::preprocess("rhi/prepass.vs.glsl");
    const std::string prepassFragment = ShaderLibrary::preprocess("rhi/prepass.fs.glsl");

    if (prepassVertex.empty() || prepassFragment.empty()) {
        LOGGER.error("ScenePipeline: rhi/prepass shaders not found");
        return false;
    }

    prepassShader_ = device_.createShader("prepass", prepassVertex.c_str(),
                                          prepassFragment.c_str());
    if (!prepassShader_.valid()) return false;

    PipelineDesc prepass;
    prepass.name         = "prepass";
    prepass.shader       = prepassShader_;
    prepass.vertexLayout = MeshVertexBuffer::deviceLayout();
    prepass.colourFormats[0] = TextureFormat::RGBA8;
    prepass.colourCount = 1;
    prepass.depthFormat = TextureFormat::D32F;

    prepass.depth.test    = true;
    prepass.depth.write   = true;
    prepass.depth.compare = CompareFunc::Less;

    /* BLENDING OFF, AND IT MUST STAY OFF. This buffer's alpha is ROUGHNESS
     * rather than coverage, so blending would mix two surfaces' material
     * parameters per pixel and produce a value belonging to neither. The
     * default BlendState is already disabled; it is stated here because the
     * consequence of someone enabling it is a G-buffer that looks almost
     * right. */
    prepass.blend.enabled = false;

    /* BACK FACES KEPT, unlike the shadow pass. A cutaway exposes the underside
     * of floors and the inside of walls, and those fragments need normals — the
     * shader flips them when gl_FrontFacing is false. Culling them would leave
     * holes in the occlusion exactly where a building was opened up. */
    prepass.raster.cull = CullMode::None;

    prepassPipeline_ = device_.createPipeline(prepass);
    if (!prepassPipeline_.valid()) return false;

    BufferDesc material;
    material.name   = "material block";
    material.bytes  = sizeof(MaterialBlockData);
    material.usage  = BufferUsageUniform;
    material.access = BufferAccess::CpuToGpuPerFrame;

    materialBlock_ = device_.createBuffer(material);
    if (!materialBlock_.valid()) return false;

    /* ---- ambient occlusion ---------------------------------------------- */

    const std::string occlusionSource = ShaderLibrary::preprocess("rhi/ssao.fs.glsl");
    if (occlusionSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/ssao.fs.glsl not found");
        return false;
    }

    /* THE FULLSCREEN VERTEX STAGE IS SHARED and lives here rather than on disk:
     * it is three lines that synthesise a covering triangle from gl_VertexID,
     * every screen-space pass wants exactly it, and a file per pass would be
     * five copies to keep in step. */
    /* gl_VertexID, NOT gl_VertexIndex. The two spell the same thing in GLSL and
     * Vulkan-GLSL respectively, and this source is compiled straight by the GL
     * driver today rather than going through SPIR-V — the Vulkan spelling needs
     * GL_KHR_vulkan_glsl and is rejected without it.
     *
     * IT IS THE FIRST CONCRETE COST OF NOT HAVING THE OFFLINE TOOLCHAIN YET.
     * glslang would take the Vulkan spelling and SPIRV-Cross would emit the GL
     * one, so neither the shader nor this comment would exist. Until then the
     * dialect is "GLSL 450 that a GL driver accepts", which is CONVENTIONS.md's
     * rules minus the handful of Vulkan-only builtins. */
    /* Declared before the occlusion pass so the resolve can share it too. */
    static const char* const kFullscreenVertex = R"(#version 450 core
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

    occlusionShader_ = device_.createShader("ssao", kFullscreenVertex, occlusionSource.c_str());
    if (!occlusionShader_.valid()) return false;

    PipelineDesc occlusion;
    occlusion.name   = "ssao";
    occlusion.shader = occlusionShader_;

    /* NO VERTEX LAYOUT — the covering triangle reads no attributes. */
    occlusion.colourFormats[0] = TextureFormat::RGBA8;
    occlusion.colourCount = 1;

    /* NO DEPTH AT ALL. A screen-space pass writes every pixel it covers, and
     * leaving the default DepthState on would test against a buffer this pass
     * has no attachment for — which discards every fragment on some drivers and
     * reads as "the pass did nothing". */
    occlusion.depth.test  = false;
    occlusion.depth.write = false;
    occlusion.raster.cull = CullMode::None;

    occlusionPipeline_ = device_.createPipeline(occlusion);
    if (!occlusionPipeline_.valid()) return false;

    /* ---- and the blur that pays off its rotation ------------------------- */

    const std::string blurSource = ShaderLibrary::preprocess("rhi/ssao_blur.fs.glsl");
    if (blurSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/ssao_blur shader not found");
        return false;
    }

    blurShader_ = device_.createShader("ssao blur", kFullscreenVertex, blurSource.c_str());
    if (!blurShader_.valid()) return false;

    PipelineDesc blur = occlusion;   /* same state exactly: screen pass, no depth */
    blur.name   = "ssao blur";
    blur.shader = blurShader_;

    blurPipeline_ = device_.createPipeline(blur);
    if (!blurPipeline_.valid()) return false;

    /* ---- the sky --------------------------------------------------------- */

    const std::string skySource = ShaderLibrary::preprocess("rhi/sky.fs.glsl");
    if (skySource.empty()) {
        LOGGER.error("ScenePipeline: rhi/sky shader not found");
        return false;
    }

    skyShader_ = device_.createShader("sky", kFullscreenVertex, skySource.c_str());
    if (!skyShader_.valid()) return false;

    PipelineDesc sky = occlusion;   /* screen pass, no depth, no vertex layout */
    sky.name   = "sky";
    sky.shader = skyShader_;

    /* RGBA16F, NOT RGBA8 — this one writes into the scene target rather than
     * the occlusion plane, and the sun disc it draws is forty times brighter
     * than the sky around it. Inheriting the occlusion pass's 8-bit format here
     * would clip the disc to white and throw away the range the tone map exists
     * to compress. */
    sky.colourFormats[0] = TextureFormat::RGBA16F;

    skyPipeline_ = device_.createPipeline(sky);
    if (!skyPipeline_.valid()) return false;

    BufferDesc skyDesc;
    skyDesc.name   = "sky block";
    skyDesc.bytes  = sizeof(SkyBlockData);
    skyDesc.usage  = BufferUsageUniform;
    skyDesc.access = BufferAccess::CpuToGpuPerFrame;

    skyBlock_ = device_.createBuffer(skyDesc);
    if (!skyBlock_.valid()) return false;

    /* POINT SAMPLING AND CLAMPED. The depth and normal planes are read at
     * exactly the pixel being shaded; filtering between two neighbouring
     * normals produces a direction that is neither, and wrapping at the edge
     * would sample the far side of the screen. */
    SamplerDesc point;
    point.minify  = FilterMode::Nearest;
    point.magnify = FilterMode::Nearest;
    point.mip     = FilterMode::Nearest;
    point.wrapU   = WrapMode::ClampToEdge;
    point.wrapV   = WrapMode::ClampToEdge;
    point.wrapW   = WrapMode::ClampToEdge;

    pointSampler_ = device_.createSampler(point);
    if (!pointSampler_.valid()) return false;

    /* THE RESOLVE'S SAMPLER, and its only user. Same clamping, opposite filter:
     * the downscale to the backbuffer wants a bilinear tap to average the 2x2
     * source block that each output pixel covers. See drawResolve. */
    SamplerDesc linear = point;
    linear.minify  = FilterMode::Linear;
    linear.magnify = FilterMode::Linear;

    linearSampler_ = device_.createSampler(linear);
    if (!linearSampler_.valid()) return false;

    BufferDesc kernelDesc;
    kernelDesc.name   = "ssao kernel";
    kernelDesc.bytes  = sizeof(KernelBlockData);
    kernelDesc.usage  = BufferUsageUniform;

    /* WRITTEN ONCE. The kernel never changes, so it is uploaded here and not
     * touched again — the frequency table in CONVENTIONS.md exists so that this
     * is a different binding from the per-frame block rather than 384 bytes
     * re-sent every frame to say the same thing. */
    kernelDesc.access = BufferAccess::CpuToGpuOnce;

    kernelBlock_ = device_.createBuffer(kernelDesc);
    if (!kernelBlock_.valid()) return false;

    BufferDesc occlusionDesc;
    occlusionDesc.name   = "ssao pass block";
    occlusionDesc.bytes  = sizeof(OcclusionBlockData);
    occlusionDesc.usage  = BufferUsageUniform;
    occlusionDesc.access = BufferAccess::CpuToGpuPerFrame;

    occlusionBlock_ = device_.createBuffer(occlusionDesc);
    if (!occlusionBlock_.valid()) return false;

    /* ITS OWN BUFFER at the same binding, for the reason the occlusion block has
     * one: binding 1 means "the block this pass reads", and the blur needs one
     * matrix where the occlusion pass needs three. */
    BufferDesc blurDesc;
    blurDesc.name   = "ssao blur block";
    blurDesc.bytes  = sizeof(BlurBlockData);
    blurDesc.usage  = BufferUsageUniform;
    blurDesc.access = BufferAccess::CpuToGpuPerFrame;

    blurBlock_ = device_.createBuffer(blurDesc);
    if (!blurBlock_.valid()) return false;
    if (!occlusionBlock_.valid()) return false;

    const KernelBlockData kernel = buildKernel();
    device_.updateBuffer(kernelBlock_, &kernel, sizeof kernel, 0);

    /* ---- the lit scene --------------------------------------------------- */

    const std::string litVertex   = ShaderLibrary::preprocess("rhi/lit.vs.glsl");
    const std::string litFragment = ShaderLibrary::preprocess("rhi/lit.fs.glsl");
    if (litVertex.empty() || litFragment.empty()) {
        LOGGER.error("ScenePipeline: rhi/lit shaders not found");
        return false;
    }

    litShader_ = device_.createShader("lit", litVertex.c_str(), litFragment.c_str());
    if (!litShader_.valid()) return false;

    PipelineDesc lit;
    lit.name         = "lit";
    lit.shader       = litShader_;
    lit.vertexLayout = MeshVertexBuffer::deviceLayout();

    /* RGBA16F — see the note on sceneColour_. */
    lit.colourFormats[0] = TextureFormat::RGBA16F;
    lit.colourCount = 1;
    lit.depthFormat = TextureFormat::D32F;

    /* EQUAL, NOT LESS, and depth WRITES OFF. The prepass has already written
     * every visible fragment's depth, so this pass shades exactly the surfaces
     * that survived it — no overdraw, and no chance of a different depth
     * ordering between the two passes. Writing depth again would be redundant;
     * testing Less would drop every fragment, since none is nearer than the
     * value the prepass stored for it. */
    lit.depth.test    = true;
    lit.depth.write   = false;
    lit.depth.compare = CompareFunc::Equal;

    /* BACK FACES KEPT, matching the prepass — a cutaway exposes the undersides
     * of floors and the shader flips their normals. Culling here but not there
     * would leave depth written for fragments this pass never shades, which
     * under an Equal test is a hole. */
    lit.raster.cull = CullMode::None;

    litPipeline_ = device_.createPipeline(lit);
    if (!litPipeline_.valid()) return false;

    /* ---- the transparent pass -------------------------------------------
     *
     * THE LIT PIPELINE WITH THREE CHANGES, and each one is forced:
     *
     * DEPTH TESTED BUT NOT WRITTEN. A translucent surface must be hidden by
     * opaque geometry in front of it, so the test stays; writing would let the
     * nearest pane occlude the one behind it, and two panes seen through each
     * other is the ordinary case in a building.
     *
     * LESS-OR-EQUAL, NOT EQUAL. The opaque passes test Equal against the
     * prepass, which is an optimisation available only because the prepass drew
     * exactly them. Glass is deliberately absent from the prepass — that is
     * what lets the room behind a window be shaded at all — so there is no
     * prepass depth to match and the test is an ordinary one.
     *
     * PREMULTIPLIED BLENDING. The shader has already scaled its diffuse by
     * coverage and deliberately left its specular alone, so the blender must
     * ADD what it is given rather than scale it again. Ordinary SrcAlpha
     * blending here would multiply the sky reflection by a 6% coverage and
     * erase the one cue that makes a pane read as glass rather than a hole. */
    const std::string transparentSource =
        ShaderLibrary::preprocess("rhi/transparent.fs.glsl");
    if (transparentSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/transparent shader not found");
        return false;
    }

    transparentShader_ = device_.createShader("transparent", litVertex.c_str(),
                                              transparentSource.c_str());
    if (!transparentShader_.valid()) return false;

    PipelineDesc transparent = lit;
    transparent.name   = "transparent";
    transparent.shader = transparentShader_;

    transparent.depth.test    = true;
    transparent.depth.write   = false;
    transparent.depth.compare = CompareFunc::LessEqual;

    transparent.blend = BlendState::premultiplied();

    transparentPipeline_ = device_.createPipeline(transparent);
    if (!transparentPipeline_.valid()) return false;
    if (!litPipeline_.valid()) return false;

    /* ---- the probe capture's two pipelines -------------------------------
     *
     * THE SAME SHADERS, WITH THE PREPASS TAKEN OUT. The camera's lit pass tests
     * Equal against a depth prepass and writes no depth — an optimisation
     * available only because the prepass drew exactly the same geometry. A cube
     * face has no prepass, because a second submission of the world per face
     * would cost more than it saves at 128 pixels, so this is an ordinary
     * forward pass: Less, and depth written.
     *
     * REUSING THE CAMERA'S PIPELINE HERE DRAWS NOTHING AT ALL, and it fails
     * silently: every fragment tests Equal against a buffer cleared to 1.0,
     * every fragment is discarded, and six black faces are exactly what a probe
     * that could not be attached also produces. Two pipelines is the price of
     * that ambiguity never arising. */
    PipelineDesc probeLit = lit;
    probeLit.name = "probe lit";
    probeLit.depth.test    = true;
    probeLit.depth.write   = true;
    probeLit.depth.compare = CompareFunc::Less;

    probeLitPipeline_ = device_.createPipeline(probeLit);
    if (!probeLitPipeline_.valid()) return false;

    /* The translucent half of a capture. LessEqual against the depth this pass
     * itself wrote, and premultiplied — a pane inside a reflection has to
     * composite over the room behind it, or its coverage alpha lands in the
     * cubemap as a hole the sampler then fills with sky. */
    PipelineDesc probeTransparent = transparent;
    probeTransparent.name = "probe transparent";
    probeTransparent.depth.test    = true;
    probeTransparent.depth.write   = false;
    probeTransparent.depth.compare = CompareFunc::LessEqual;

    probeTransparentPipeline_ = device_.createPipeline(probeTransparent);
    if (!probeTransparentPipeline_.valid()) return false;

    BufferDesc litDesc;
    litDesc.name   = "lit block";
    litDesc.bytes  = sizeof(LitBlockData);
    litDesc.usage  = BufferUsageUniform;
    litDesc.access = BufferAccess::CpuToGpuPerFrame;
    litBlock_ = device_.createBuffer(litDesc);
    if (!litBlock_.valid()) return false;

    /* NO COMPARISON SAMPLER ANY MORE, and its absence is deliberate rather than
     * an omission.
     *
     * There was one here: LessEqual with linear filtering, so the hardware would
     * average the RESULTS of four depth tests and hand back percentage-closer
     * filtering for free. It is the right idea and it produced blocky, speckled
     * shadow edges, because GL does not require linear filtering of a 32-bit
     * float depth format and D32F is what this map is. Where the driver falls
     * back to nearest, every tap is binary and the filter quantises.
     *
     * The lit shader now filters by hand off the plain point sampler above —
     * see shadowTap in rhi/lit.fs.glsl for the full account. Anything reviving
     * this needs to answer the format question first, per backend. */

    /* ---- the resolve ----------------------------------------------------- */

    const std::string resolveSource = ShaderLibrary::preprocess("rhi/tonemap.fs.glsl");
    if (resolveSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/tonemap.fs.glsl not found");
        return false;
    }

    resolveShader_ = device_.createShader("tonemap", kFullscreenVertex, resolveSource.c_str());
    if (!resolveShader_.valid()) return false;

    PipelineDesc resolve;
    resolve.name   = "tonemap";
    resolve.shader = resolveShader_;

    /* THE BACKBUFFER'S FORMAT. RGBA8 rather than the scene's 16F — this is the
     * pass that leaves linear space. */
    resolve.colourFormats[0] = TextureFormat::RGBA8;
    resolve.colourCount = 1;
    resolve.depth.test  = false;
    resolve.depth.write = false;
    resolve.raster.cull = CullMode::None;

    resolvePipeline_ = device_.createPipeline(resolve);
    if (!resolvePipeline_.valid()) return false;

    BufferDesc resolveDesc;
    resolveDesc.name   = "resolve block";
    resolveDesc.bytes  = sizeof(ResolveBlockData);
    resolveDesc.usage  = BufferUsageUniform;
    resolveDesc.access = BufferAccess::CpuToGpuPerFrame;
    resolveBlock_ = device_.createBuffer(resolveDesc);
    if (!resolveBlock_.valid()) return false;

    /* One block per surface kind, at PbrMaterial's defaults. The game overrides
     * whichever it wants a different response from — see DeviceMaterials. */
    if (!materials_.initialise()) return false;

    /* ---- what a capture binds where SSAO would be ------------------------
     *
     * ONE WHITE TEXEL. The lit shaders read the occlusion plane by
     * gl_FragCoord, which means nothing inside a 128-pixel cube face, and they
     * clamp the fetch to the bound texture's size — so a 1x1 white answers
     * "nothing is occluded" for every fragment of a capture. That is the honest
     * answer: there is no depth prepass behind a capture to occlude against.
     *
     * NOT SKIPPED BY LEAVING THE SLOT UNBOUND. A sampler bound to nothing reads
     * as black on most drivers and as whatever the last pass left there on
     * some, and black occlusion is a cubemap in which every room is a cave. */
    TextureDesc white;
    white.name   = "white";
    white.width  = 1;
    white.height = 1;
    white.format = TextureFormat::RGBA8;
    white.usage  = TextureUsageSampled;
    whitePixel_ = device_.createTexture(white);
    if (!whitePixel_.valid()) return false;

    const uint8_t whitePixels[4] = { 255, 255, 255, 255 };
    device_.updateTexture(whitePixel_, whitePixels);

    /* ---- the probe prefilter ---------------------------------------------
     *
     * A screen-space pass into one (probe, face, level) of the cube array. The
     * fullscreen vertex stage is shared with every other screen pass; the
     * fragment stage does the GGX convolution. */
    const std::string prefilterSource = ShaderLibrary::preprocess("rhi/probe_prefilter.fs.glsl");
    if (prefilterSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/probe_prefilter.fs.glsl not found");
        return false;
    }

    prefilterShader_ = device_.createShader("probe prefilter", kFullscreenVertex,
                                            prefilterSource.c_str());
    if (!prefilterShader_.valid()) return false;

    PipelineDesc prefilter;
    prefilter.name   = "probe prefilter";
    prefilter.shader = prefilterShader_;

    /* RGBA16F — it writes into the probe array, which is HDR for the same
     * reason the scene target is. Inheriting an 8-bit format here would clip
     * every reflection of a sunlit wall to white. */
    prefilter.colourFormats[0] = TextureFormat::RGBA16F;
    prefilter.colourCount = 1;
    prefilter.depth.test  = false;
    prefilter.depth.write = false;
    prefilter.raster.cull = CullMode::None;

    prefilterPipeline_ = device_.createPipeline(prefilter);
    if (!prefilterPipeline_.valid()) return false;

    BufferDesc prefilterDesc;
    prefilterDesc.name   = "probe prefilter block";
    prefilterDesc.bytes  = sizeof(PrefilterBlockData);
    prefilterDesc.usage  = BufferUsageUniform;
    prefilterDesc.access = BufferAccess::CpuToGpuPerFrame;
    prefilterBlock_ = device_.createBuffer(prefilterDesc);
    if (!prefilterBlock_.valid()) return false;

    /* ---- and the probes themselves ---------------------------------------
     *
     * NOT FATAL. A device with no cubemap arrays — macOS's capped GL, a future
     * software backend — draws every surface against the analytic sky, which is
     * a flatter frame and not a broken one. It is the same fallback a board
     * with no probes placed on it already takes, so there is exactly one code
     * path for both. */
    if (!probes_.create())
        LOGGER.warn("ScenePipeline: no reflection probes - surfaces keep the analytic sky");

    ready_ = true;

    /* A PLACEHOLDER SIZE. The first render() resizes to the real surface; this
     * only exists so the targets are never invalid between here and there. */
    if (!createSceneTargets(1280, 720)) return false;

    LOGGER.info("ScenePipeline: shadow {0}x{0}, prepass, ssao + blur, sky, lit and "
                "resolve ready ({1}x supersample)", kShadowSize, kSupersample);
    return true;
}

ScenePipeline::SunProjection ScenePipeline::sunProjection(const SceneFrame& frame,
                                                          Vec3 minimum, Vec3 maximum)
{
    const Vec3 worldCentre = (minimum + maximum) * 0.5f;
    const Vec3 worldExtent = maximum - minimum;
    const float worldDiagonal = worldExtent.length();

    /* ---- 1. what the camera can see, in world space ----------------------
     *
     * READ BACK OFF THE MATRICES rather than taken as extra fields on
     * SceneFrame. The frustum is entirely determined by the view and projection
     * the caller already supplied, and a second description of it is a second
     * thing that can disagree — a fit computed from a stale fov is a shadow map
     * aimed slightly wrong, which looks like a bias problem.
     *
     * From Mat4::perspective: at(1,1) is 1/tan(fovY/2) and at(0,0) is that over
     * the aspect ratio. From Mat4::lookAt, the rotation part's ROWS are the
     * camera's basis — right, up, and minus forward. */
    /* IS THERE A CAMERA AT ALL? A perspective matrix puts -1 at (3,2) — that is
     * what carries view-space z into clip w and makes the divide a divide. An
     * identity there means the caller filled no camera this frame, which is a
     * real case: SceneFrame's matrices are only written when one exists.
     *
     * Fitting a frustum to an identity projection produces a box of unit size at
     * the origin, and a shadow map covering one cubic metre of empty space is
     * indistinguishable from a broken one. The whole-world fit is the honest
     * answer, and is what this function did unconditionally before. */
    const bool hasCamera = frame.projection.at(3, 2) != 0.0f;

    const float tanHalfFovY = frame.projection.at(1, 1) != 0.0f
                            ? 1.0f / frame.projection.at(1, 1) : 1.0f;
    const float aspect = frame.projection.at(0, 0) != 0.0f
                       ? frame.projection.at(1, 1) / frame.projection.at(0, 0) : 1.0f;

    const Vec3 right{ frame.view.at(0, 0), frame.view.at(0, 1), frame.view.at(0, 2) };
    const Vec3 up{ frame.view.at(1, 0), frame.view.at(1, 1), frame.view.at(1, 2) };
    const Vec3 forward{ -frame.view.at(2, 0), -frame.view.at(2, 1), -frame.view.at(2, 2) };

    /* NOT THE REAL FAR PLANE. The camera's is a thousand units out so that
     * nothing ever clips; fitting the sun's box to a frustum that long would
     * hand back every texel the focus was meant to win. The board's own
     * diagonal, with a margin, is as far as anything worth shadowing can be. */
    const float shadowDistance = worldDiagonal * 1.2f;

    Vec3 boxMinimum{ 1.0e30f, 1.0e30f, 1.0e30f };
    Vec3 boxMaximum{ -1.0e30f, -1.0e30f, -1.0e30f };

    for (int slice = 0; slice < 2; slice++) {
        const float distance = slice == 0 ? 0.1f : shadowDistance;
        const float halfHeight = tanHalfFovY * distance;
        const float halfWidth  = halfHeight * aspect;
        const Vec3  middle = frame.cameraPosition + forward * distance;

        for (int corner = 0; corner < 4; corner++) {
            const float x = (corner & 1) != 0 ? 1.0f : -1.0f;
            const float y = (corner & 2) != 0 ? 1.0f : -1.0f;
            const Vec3 point = middle + right * (halfWidth * x) + up * (halfHeight * y);

            boxMinimum = Vec3{ std::min(boxMinimum.x, point.x), std::min(boxMinimum.y, point.y),
                               std::min(boxMinimum.z, point.z) };
            boxMaximum = Vec3{ std::max(boxMaximum.x, point.x), std::max(boxMaximum.y, point.y),
                               std::max(boxMaximum.z, point.z) };
        }
    }

    /* CLIPPED TO THE WORLD. The frustum reaches into empty sky and past the far
     * edge of the board; the lattice does not, and fitting to the sky throws the
     * resolution away exactly as fitting to the whole world did. */
    boxMinimum = Vec3{ std::max(boxMinimum.x, minimum.x), std::max(boxMinimum.y, minimum.y),
                       std::max(boxMinimum.z, minimum.z) };
    boxMaximum = Vec3{ std::min(boxMaximum.x, maximum.x), std::min(boxMaximum.y, maximum.y),
                       std::min(boxMaximum.z, maximum.z) };

    Vec3  centre = (boxMinimum + boxMaximum) * 0.5f;
    float radius = (boxMaximum - boxMinimum).length() * 0.5f;

    /* THE CAMERA IS LOOKING AWAY FROM THE BOARD, so the intersection is empty
     * and every number above is nonsense. Falling back to the whole world is
     * the coarse fit this function used to do unconditionally — correct, just
     * blunt, which is exactly right for a frame nobody is reading the board on. */
    if (!hasCamera || boxMinimum.x > boxMaximum.x || boxMinimum.y > boxMaximum.y ||
        boxMinimum.z > boxMaximum.z) {
        centre = worldCentre;
        radius = worldDiagonal * 0.5f;
    }

    if (radius < 0.5f) radius = 0.5f;

    /* ---- 2. the light's frame, and the snap into it ----------------------
     *
     * ORIENTATION ONLY, EYE AT THE ORIGIN. The light's axes have to be a FIXED
     * frame for the centre to be quantised in; building the view around the
     * moving centre instead puts the snap in a space that moves with the thing
     * being snapped, which does exactly nothing.
     *
     * ===================== UP IS +Y, AND IT MATTERS A LOT ==================
     *
     * This was +Z, on the reasoning that the sun looks steeply downward and an
     * up vector parallel to the view direction makes the look-at cross two
     * parallel vectors — NaN everywhere and an empty map. That danger is real
     * and it is already handled elsewhere: SunLight clamps elevation to 4..86
     * degrees precisely so the sun is never straight down, which is what makes
     * +Y safe here and is why the raylib path has used it all along.
     *
     * WHAT +Z COST, because it is not obvious and it took a shadow-map dump to
     * see: the up vector fixes the ORIENTATION OF THE TEXEL GRID. Both paths
     * fitted the same sphere, at the same radius, with the same world-units-
     * per-texel — every number matched — and yet the maps disagreed over a
     * third of their texels, because the grid was rotated differently under the
     * same geometry. The world projected as a narrow parallelogram using about
     * 60% of the map where the raylib path's filled 95% of it as a square.
     *
     * A rotated grid is not a cosmetic difference. A shadow edge aliases
     * against the texel lattice it is sampled on, so changing the lattice's
     * angle changes where every staircase falls — which is exactly what "the
     * new shadows are jagged and the old ones are not" looks like when every
     * measurable parameter is identical.
     *
     * The lesson is the general one: matching the NUMBERS is not matching the
     * BASIS they are expressed in. */
    const Vec3 travel = frame.sunDirection.normalised();
    const Mat4 lightRotation =
        Mat4::lookAt(Vec3{ 0.0f, 0.0f, 0.0f }, travel, Vec3{ 0.0f, 1.0f, 0.0f });

    SunProjection result;
    result.worldTexelSize = (radius * 2.0f) / static_cast<float>(kShadowSize);

    Vec3 lightCentre = lightRotation.transformPoint(centre);
    lightCentre.x = std::floor(lightCentre.x / result.worldTexelSize) * result.worldTexelSize;
    lightCentre.y = std::floor(lightCentre.y / result.worldTexelSize) * result.worldTexelSize;

    /* ---- 3. the box ------------------------------------------------------
     *
     * DEEP ENOUGH TO REACH ANY CASTER UP-SUN, AND NO MORE. Slack is not free:
     * the shader's biases are world-unit quantities divided by this range, so a
     * range twice as deep makes every bias twice as large in world terms — and
     * an over-large bias detaches a shadow from its caster, which is the light
     * creeping out from the foot of a wall. */
    const float depth = std::max(worldDiagonal, 1.0f);
    result.depthRange = depth * 2.0f;

    /* NEGATED Z, because the light looks down -z in its own frame and the
     * orthographic near/far are distances along the view direction. */
    const Mat4 projection = Mat4::orthographic(
        lightCentre.x - radius, lightCentre.x + radius,
        lightCentre.y - radius, lightCentre.y + radius,
        -lightCentre.z - depth, -lightCentre.z + depth);

    result.viewProjection = projection * lightRotation;
    return result;
}

void ScenePipeline::drawShadowMap(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("shadow map");

    Vec3 minimum;
    Vec3 maximum;
    geometry.worldBounds(minimum, maximum);

    /* An empty world, or a caller that handed over a zero sun. Either produces
     * a degenerate matrix; skipping is the honest response and leaves last
     * frame's depth rather than a NaN one. */
    const Vec3 extent = maximum - minimum;
    if (extent.length() < 1.0e-4f) return;
    if (frame.sunDirection.length() < 1.0e-4f) return;

    PassBlockData block;
    block.viewProjection = sunProjection(frame, minimum, maximum).viewProjection;
    device_.updateBuffer(passBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name          = "shadow map";
    pass.hasDepth      = true;
    pass.depth.texture = shadowDepth_;
    pass.depth.load    = LoadAction::Clear;
    pass.depth.clearTo = 1.0f;

    /* STORED, because the lit pass will sample it. The moment nothing does,
     * this becomes Discard and a tiler stops writing it out. */
    pass.depth.store = StoreAction::Store;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(depthPipeline_);
    encoder.bindUniformBuffer(1, passBlock_);

    geometry.submit(encoder, GeometryPass::Shadow);

    device_.endPass(encoder);
}

void ScenePipeline::drawShadowTransmission(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("transmission");

    if (!shadowTransmission_.valid() || !shadowDepth_.valid()) return;

    /* THE SUN'S MATRIX, ALREADY IN passBlock_ from the shadow pass above. This
     * runs immediately after it and nothing between them touches that buffer;
     * recomputing the fit would be a second chance for the two to disagree
     * about where the sun is. */

    PassDesc pass;
    pass.name = "transmission";
    pass.colours[0].texture = shadowTransmission_;
    pass.colours[0].load    = LoadAction::Clear;
    pass.colours[0].store   = StoreAction::Store;

    /* WHITE IS OPEN AIR — the sun arrives unchanged where nothing translucent
     * stood in its way. Clearing to black would say every texel is behind
     * something opaque, which is the whole world in shadow. */
    pass.colours[0].clearTo = ClearColour{ 1.0f, 1.0f, 1.0f, 1.0f };
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(transmissionPipeline_);
    encoder.bindTexture(0, shadowDepth_, pointSampler_);
    encoder.bindUniformBuffer(1, passBlock_);

    geometry.submit(encoder, GeometryPass::Transparent);

    device_.endPass(encoder);
}

/* HOW MANY CUBE FACES A FRAME PAYS FOR.
 *
 * ONE IN THE STEADY STATE, and that is the entire reason the schedule walks
 * (probe, face) pairs rather than whole probes: the cost is one extra scene
 * render per frame no matter how many rooms the map has. The price is
 * staleness — 6 x probeCount frames for a full sweep — and on a tactical camera
 * watching a world that only changes when something is destroyed, nobody sees
 * it.
 *
 * FOUR WHILE STALE, which is the case that would otherwise be visible: after a
 * placement every layer holds a world that no longer exists, and a sixteen-room
 * board would take 1.6 seconds at one a frame to stop showing the old one. At
 * four it is 0.4, which reads as a settle rather than as a bug.
 *
 * Both are face counts and not milliseconds on purpose. A face is one draw of
 * the world at 128 pixels with no SSAO, no supersample and no prepass — it is
 * bounded by vertex work rather than fill, so it costs what it costs regardless
 * of the window's size, and a budget in time would have to be measured on a
 * machine nobody else has. */
constexpr int kProbeFacesPerFrame = 1;
constexpr int kProbeFacesWhileStale = 4;

void ScenePipeline::drawProbeCapture(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("probe capture");

    if (!frame.reflections) return;
    if (!probes_.valid() || probes_.probeCount() == 0) return;

    /* THE SUN'S FIT, RECOMPUTED FROM THE SAME FUNCTION THE SHADOW PASS USED.
     * A capture shades against the shadow map this frame already wrote, so it
     * needs that map's matrix and its two world-unit scales — and reading them
     * off a second, differently-fitted call would offset every shadow inside
     * every reflection from the geometry casting it. */
    Vec3 minimum;
    Vec3 maximum;
    geometry.worldBounds(minimum, maximum);
    const SunProjection shadow = sunProjection(frame, minimum, maximum);

    const int faces = probes_.stale() ? kProbeFacesWhileStale : kProbeFacesPerFrame;

    for (int i = 0; i < faces; i++) {
        DeviceProbeSet::Face face;
        if (!probes_.nextFace(face)) break;

        LitBlockData block = buildLitBlock(frame, shadow.viewProjection,
                                           shadow.worldTexelSize, shadow.depthRange);

        /* THE THREE THINGS THAT ARE THE PASS'S RATHER THAN THE FRAME'S. */
        block.viewProjection = face.viewProjection;
        writeVec3(block.cameraPosition, face.eye);

        /* NO PROBES INSIDE A PROBE. The array being written cannot also be
         * read, and it would be wrong if it could: a reflection of the
         * reflections compounds its own error on every sweep, and each bounce
         * arrives a full sweep later than the one before it. */
        block.probeParams[0] = 0.0f;

        device_.updateBuffer(litBlock_, &block, sizeof block, 0);

        PassDesc pass;
        pass.name = "probe face";

        pass.colours[0].texture = probes_.texture();

        /* THE SLICE, which is what a cube array attachment wants: one flat
         * layer-face number, not a (probe, face) pair. Getting this wrong does
         * not fail — every probe simply writes onto layer 0, every interior
         * samples an empty layer and falls back to the sky, and the one
         * populated layer holds whichever room was captured last. That reads as
         * "the reflections are wrong" and gives no hint that the write rather
         * than the read is at fault. */
        pass.colours[0].layer = static_cast<uint32_t>(face.slice);
        pass.colours[0].load  = LoadAction::Clear;
        pass.colours[0].store = StoreAction::Store;

        /* TRANSPARENT BLACK, and the alpha is the load-bearing part: it is how
         * the sampler tells "world in this direction" from "open sky". Clearing
         * to a sky colour instead would put a flat, blocky, 128-pixel sky in
         * every reflection where the smooth analytic one belongs. */
        pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 0.0f };
        pass.colourCount = 1;

        pass.hasDepth      = true;
        pass.depth.texture = probes_.captureDepth();
        pass.depth.load    = LoadAction::Clear;
        pass.depth.clearTo = 1.0f;

        /* DISCARDED. Nothing samples a capture's depth — there is no occlusion
         * pass and nothing reconstructs a position from it — so one buffer
         * serves all ninety-six slices and a tiler never writes it out. */
        pass.depth.store = StoreAction::Discard;

        ICommandEncoder& encoder = device_.beginPass(pass);
        encoder.bindPipeline(probeLitPipeline_);

        /* WHITE WHERE THE OCCLUSION PLANE GOES — see whitePixel_. */
        encoder.bindTexture(1, whitePixel_, pointSampler_);
        encoder.bindTexture(2, shadowDepth_, pointSampler_);
        encoder.bindTexture(3, shadowTransmission_, pointSampler_);

        /* THE 1x1 STAND-IN AT THE PROBE SLOT, not the array being drawn into.
         * The shader's probe count is zero so nothing reads it — but a texture
         * that is simultaneously a colour attachment and bound to a live
         * sampler is undefined on every backend, and "the driver probably will
         * not mind" is not a thing to leave in a render loop. */
        encoder.bindTexture(4, probes_.emptyTexture(), probes_.sampler());

        encoder.bindUniformBuffer(0, probes_.block());
        encoder.bindUniformBuffer(1, litBlock_);
        encoder.bindUniformBuffer(2, materialBlock_);

        geometry.submit(encoder, GeometryPass::ProbeOpaque);

        /* THEN WHAT YOU CAN SEE THROUGH, in the same pass — it reads the depth
         * and the colour the opaque half just wrote, exactly as the camera's
         * transparent pass reads the lit scene's. */
        encoder.bindPipeline(probeTransparentPipeline_);
        geometry.submit(encoder, GeometryPass::ProbeTransparent);

        device_.endPass(encoder);

    }
}

void ScenePipeline::drawProbePrefilter()
{
    CW_PROFILE_ZONE_N("probe prefilter");

    if (!probes_.valid() || !prefilterPipeline_.valid()) return;

    /* ONE PROBE PER FRAME AT MOST, and only one whose six faces are all current.
     * See the header: a lobe that reaches across faces cannot be run against a
     * probe that is half rebuilt. */
    const int probe = probes_.probeReadyToPrefilter();
    if (probe < 0) return;

    for (int level = 1; level < DeviceProbeSet::kMipLevels; level++) {
        /* ROUGHNESS IS THE LEVEL'S POSITION IN THE CHAIN, which is the contract
         * the lit shader reads back as `roughness * (levels - 1)`. Level 0 is
         * the capture itself and is left alone — it is already the mirror. */
        const float roughness = static_cast<float>(level)
                              / static_cast<float>(DeviceProbeSet::kMipLevels - 1);

        /* THE SOURCE IS THE LEVEL ABOVE, bound through a sampler clamped to it
         * so this pass cannot read the level it is writing. */
        const rhi::SamplerHandle source = probes_.levelSampler(level - 1);

        const uint32_t size = static_cast<uint32_t>(DeviceProbeSet::kFaceSize >> level);

        for (int face = 0; face < 6; face++) {
            PrefilterBlockData block;
            block.parameters[0] = roughness;
            block.parameters[1] = static_cast<float>(kPrefilterSamples);
            block.parameters[2] = static_cast<float>(probe);
            block.parameters[3] = static_cast<float>(face);
            block.faceSize[0]   = static_cast<float>(size);
            device_.updateBuffer(prefilterBlock_, &block, sizeof block, 0);

            PassDesc pass;
            pass.name = "probe prefilter";

            pass.colours[0].texture = probes_.texture();

            /* THE SLICE AND THE LEVEL. `layer` picks the (probe, face) slice
             * exactly as the capture does; `mip` is what makes this a chain
             * rather than six more copies of level zero. */
            pass.colours[0].layer = static_cast<uint32_t>(probe * 6 + face);
            pass.colours[0].mip   = static_cast<uint32_t>(level);

            /* DontCare: the covering triangle writes every texel of the level. */
            pass.colours[0].load  = LoadAction::DontCare;
            pass.colours[0].store = StoreAction::Store;
            pass.colourCount = 1;

            ICommandEncoder& encoder = device_.beginPass(pass);
            encoder.bindPipeline(prefilterPipeline_);
            encoder.bindTexture(0, probes_.texture(), source);
            encoder.bindUniformBuffer(1, prefilterBlock_);
            encoder.drawFullscreen();
            device_.endPass(encoder);
        }
    }

    probes_.markPrefiltered(probe);
}

void ScenePipeline::drawPrepass(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("prepass");

    if (!sceneDepth_.valid() || !sceneNormals_.valid()) return;

    PassBlockData block;
    block.viewProjection = frame.projection * frame.view;
    device_.updateBuffer(passBlock_, &block, sizeof block, 0);

    /* ONE ROUGHNESS FOR EVERY SURFACE, for now. The raylib prepass pushes a
     * per-material value between draws, which needs the material library and
     * the per-bucket submission the lit pass will bring with it. Until then a
     * single plausible value fills the alpha plane honestly rather than leaving
     * it undefined. */
    MaterialBlockData material;
    device_.updateBuffer(materialBlock_, &material, sizeof material, 0);

    PassDesc pass;
    pass.name = "prepass";

    pass.colours[0].texture = sceneNormals_;
    pass.colours[0].load    = LoadAction::Clear;
    pass.colours[0].store   = StoreAction::Store;

    /* CLEARED TO A FLAT UPWARD NORMAL AND ZERO ROUGHNESS rather than to black.
     * Black decodes to (-1,-1,-1) once the *2-1 is undone, which is not a unit
     * vector and makes the occlusion pass build a hemisphere round a direction
     * that does not exist — noise in exactly the pixels where nothing was
     * drawn. */
    pass.colours[0].clearTo = ClearColour{ 0.5f, 1.0f, 0.5f, 0.0f };
    pass.colourCount = 1;

    pass.hasDepth      = true;
    pass.depth.texture = sceneDepth_;
    pass.depth.load    = LoadAction::Clear;
    pass.depth.clearTo = 1.0f;
    pass.depth.store   = StoreAction::Store;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(prepassPipeline_);
    encoder.bindUniformBuffer(1, passBlock_);
    encoder.bindUniformBuffer(2, materialBlock_);

    geometry.submit(encoder, GeometryPass::Prepass);

    device_.endPass(encoder);
}

void ScenePipeline::drawOcclusion(const SceneFrame& frame)
{
    CW_PROFILE_ZONE_N("ssao");

    if (!occlusion_.valid() || !sceneDepth_.valid() || !sceneNormals_.valid()) return;

    OcclusionBlockData block;
    block.projection        = frame.projection;
    block.inverseProjection = frame.projection.inverse();
    block.view              = frame.view;

    /* THE SCENE TARGET'S SIZE, NOT THE SURFACE'S — this pass draws into the
     * supersampled occlusion plane and addresses the supersampled depth and
     * normal planes, so every pixel it converts to a UV is one of those. Handing
     * it the window size instead put every sample at half the intended
     * coordinate, which does not look like a resolution mistake: the occlusion
     * simply lands in the wrong place, subtly, and reads as SSAO being noisy. */
    block.resolutionAndRadius[0] = static_cast<float>(sceneWidth());
    block.resolutionAndRadius[1] = static_cast<float>(sceneHeight());

    /* THE SAME THREE NUMBERS AmbientOcclusion::Tuning CARRIES, and they must
     * stay that way. These were invented here — radius 0.9, bias 0.025,
     * strength 1.0 — against the raylib path's 0.45, 0.008 and 0.9, and the
     * result was visibly different in a way that reads as a broken port rather
     * than as mistuning.
     *
     * THE BIAS IS THE ONE THAT SHOWS. It is the threshold below which a tap is
     * treated as the surface itself rather than an occluder, so three times too
     * large rejects exactly the close occluders that produce CONTACT darkening
     * — which is most of what the effect is for. The doubled radius then
     * spreads what survives into a wash.
     *
     * RADIUS IS IN WORLD UNITS — tiles, here — rather than pixels, so the
     * effect does not change size when the camera moves.
     *
     * THEY BELONG ON SceneFrame, borrowed from the live AmbientOcclusion the
     * way the sun is, so the dev panel's sliders reach both renderers. Until
     * that is wired they are its defaults, copied. */
    block.resolutionAndRadius[2] = 0.45f;
    block.resolutionAndRadius[3] = 0.008f;
    block.strength[0] = 1.0f;

    /* THE PASS BLOCK IS A DIFFERENT SHAPE FROM THE GEOMETRY PASSES', and shares
     * their binding. That is legal and deliberate: binding 1 means "the block
     * this pass reads", and a screen-space pass needs three matrices where a
     * geometry pass needs one. The pipelines are separate, so nothing can read
     * the wrong layout — which is exactly what a baked pipeline object buys. */
    device_.updateBuffer(occlusionBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "ssao";
    pass.colours[0].texture = occlusion_;

    /* DontCare, not Clear: the shader writes every pixel it covers and the
     * covering triangle covers all of them, so clearing first is a full-screen
     * write thrown away. On a tiler it is the difference between a pass that
     * loads the attachment and one that does not touch memory at all. */
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(occlusionPipeline_);
    encoder.bindTexture(0, sceneDepth_, pointSampler_);
    encoder.bindTexture(1, sceneNormals_, pointSampler_);
    encoder.bindUniformBuffer(1, occlusionBlock_);
    encoder.bindUniformBuffer(3, kernelBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

void ScenePipeline::drawOcclusionBlur(const SceneFrame& frame)
{
    CW_PROFILE_ZONE_N("ssao blur");

    if (!occlusion_.valid() || !occlusionBlurred_.valid() || !sceneDepth_.valid()) return;

    BlurBlockData block;
    block.inverseProjection = frame.projection.inverse();
    block.resolution[0] = static_cast<float>(sceneWidth());
    block.resolution[1] = static_cast<float>(sceneHeight());
    device_.updateBuffer(blurBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "ssao blur";
    pass.colours[0].texture = occlusionBlurred_;

    /* DontCare: the covering triangle writes every pixel. */
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(blurPipeline_);
    encoder.bindTexture(0, occlusion_, pointSampler_);
    encoder.bindTexture(1, sceneDepth_, pointSampler_);
    encoder.bindUniformBuffer(1, blurBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

void ScenePipeline::drawSky(const SceneFrame& frame)
{
    CW_PROFILE_ZONE_N("sky");

    if (!sceneColour_.valid()) return;

    SkyBlockData block;

    /* THE FULL INVERSE, not inverseRigid. A view-projection is not a rigid
     * transform — the projection is the whole point of it — and the fast
     * inverse would return a matrix that looks plausible and unprojects every
     * pixel to the wrong ray. */
    block.inverseViewProjection = (frame.projection * frame.view).inverse();

    block.resolution[0] = static_cast<float>(sceneWidth());
    block.resolution[1] = static_cast<float>(sceneHeight());

    const Vec3 sun = frame.sunDirection.normalised();
    writeVec3(block.sunDirection, sun);
    writeVec3(block.sunColour,  frame.sunRadiance);
    writeVec3(block.skyZenith,  frame.skyZenith);
    writeVec3(block.skyHorizon, frame.skyHorizon);
    writeVec3(block.skyGround,  frame.skyGround);

    device_.updateBuffer(skyBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "sky";
    pass.colours[0].texture = sceneColour_;

    /* DontCare, because this pass IS the clear: the covering triangle writes
     * every pixel of the target, so clearing first would be a full-screen write
     * thrown away. */
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    /* NO DEPTH ATTACHMENT AT ALL. Without one there is nothing to test against
     * and nothing to write, so the sky lands under the whole frame and the
     * geometry drawn after it occludes it for free — no skybox mesh, no far
     * plane, no depth-clamp trick. */

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(skyPipeline_);
    encoder.bindUniformBuffer(1, skyBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

void ScenePipeline::drawLitScene(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("lit scene");

    if (!sceneColour_.valid() || !sceneDepth_.valid()) return;

    Vec3 minimum;
    Vec3 maximum;
    geometry.worldBounds(minimum, maximum);

    /* THE SAME PROJECTION THE SHADOW PASS USED, recomputed rather than cached —
     * it is a frustum fit against a lookup that would have to be invalidated
     * whenever the sun, the camera or the world moved. If the two ever
     * disagreed, every shadow would be offset from the geometry casting it,
     * which is the hardest shadow bug to diagnose from a picture.
     *
     * IT IS ALSO WHERE THE SCALES COME FROM. Both are outputs of the same fit
     * and are meaningless against a different one, so recomputing the matrix and
     * reading the numbers off the SAME call is what stops a refit and a bias
     * getting a frame out of step. */
    const SunProjection shadow = sunProjection(frame, minimum, maximum);

    /* THE SAME BUILDER THE PROBE CAPTURE USES, so a reflection is lit by this
     * frame's sun rather than by a second one that drifted. See buildLitBlock. */
    LitBlockData block = buildLitBlock(frame, shadow.viewProjection,
                                       shadow.worldTexelSize, shadow.depthRange);

    block.viewProjection = frame.projection * frame.view;
    writeVec3(block.cameraPosition, frame.cameraPosition);

    /* HOW MANY PROBES THE SHADER SHOULD CONSIDER — zero when the dev panel's
     * reflections switch is off, which turns the ambient specular back into the
     * analytic sky with no second code path to keep in step. */
    block.probeParams[0] = frame.reflections
                         ? static_cast<float>(probes_.probeCount()) : 0.0f;

    device_.updateBuffer(litBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "lit scene";

    pass.colours[0].texture = sceneColour_;

    /* LOADED, NOT CLEARED — the sky pass has already filled this target and
     * clearing here would paint over it. This was a Clear to a dim blue-grey
     * while there was no sky, which is exactly the kind of placeholder that
     * survives the thing it stood in for. */
    pass.colours[0].load  = LoadAction::Load;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    /* THE PREPASS'S DEPTH, LOADED not cleared — this pass tests Equal against
     * what the prepass wrote, so clearing it here would discard every fragment
     * and produce an empty frame. */
    pass.hasDepth      = true;
    pass.depth.texture = sceneDepth_;
    pass.depth.load    = LoadAction::Load;
    pass.depth.store   = StoreAction::Store;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(litPipeline_);
    /* THE BLURRED PLANE, not the raw one. The raw output is noise by
     * construction — see drawOcclusionBlur. */
    encoder.bindTexture(1, occlusionBlurred_, pointSampler_);

    /* THE SHADOW MAP, RAW AND UNFILTERED — one binding, and slot 0 is left
     * empty rather than carrying a comparison sampler.
     *
     * Both halves of PCSS want it this way. The blocker search asks "what is in
     * the way and how far in front of me is it", which a comparison sampler
     * cannot answer at all — it returns a visibility fraction, not a depth —
     * and which a FILTERED read answers wrongly, since an averaged depth across
     * a silhouette describes a surface that is not there. The filter taps want
     * raw depth too, because they do their own compare-then-interpolate; see
     * shadowTap in the shader for why that is by hand and not by the sampler. */
    encoder.bindTexture(2, shadowDepth_, pointSampler_);
    encoder.bindTexture(3, shadowTransmission_, pointSampler_);

    /* THE REFLECTION PROBES — the cubemap array at slot 4 and the volumes at
     * binding 0. BOUND UNCONDITIONALLY, even with no probes placed and even
     * with reflections switched off, because a pipeline's bindings are the same
     * every frame: branching here would leave slot 4 holding whatever a
     * previous pass put there on exactly the frames nothing checks it. The
     * count in the block is what turns the term off, not the binding. */
    encoder.bindTexture(4, probes_.valid() ? probes_.texture() : probes_.emptyTexture(),
                        probes_.sampler());
    encoder.bindUniformBuffer(0, probes_.block());

    encoder.bindUniformBuffer(1, litBlock_);

    /* THE SAME BLOCK THE PREPASS FILLED, at the same binding. One material for
     * the whole world while there is no material library — see the note in
     * drawPrepass, which writes it. Bound here rather than re-uploaded because
     * nothing between the two passes touches it. */
    encoder.bindUniformBuffer(2, materialBlock_);

    geometry.submit(encoder, GeometryPass::Lit);

    device_.endPass(encoder);
}

void ScenePipeline::drawResolve(const SceneFrame& frame)
{
    CW_PROFILE_ZONE_N("resolve");

    if (!sceneColour_.valid()) return;

    ResolveBlockData block;

    /* THE FRAME'S EXPOSURE, not a constant. It was 1.0 while the lit pass wrote
     * albedo straight out and the brightest fragment in the scene was one — a
     * range the curve barely bends. Now the sun carries its real radiance and
     * the same fragment is several times that, which is what the operator's
     * shoulder is for and what makes the exposure the number that matters. */
    block.exposureAndFlags[0] = frame.exposure;
    block.exposureAndFlags[1] = 1.0f;   /* the filmic curve on */
    block.exposureAndFlags[2] = static_cast<float>(frame.debugView);

    block.outputTexel[0] = targetWidth_ > 0 ? 1.0f / static_cast<float>(targetWidth_) : 1.0f;
    block.outputTexel[1] = targetHeight_ > 0 ? 1.0f / static_cast<float>(targetHeight_) : 1.0f;
    device_.updateBuffer(resolveBlock_, &block, sizeof block, 0);

    /* THE BACKBUFFER: an attachment carrying no texture. Past this line
     * everything is display colour. */
    PassDesc pass;
    pass.name = "resolve";
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(resolvePipeline_);

    /* LINEAR, NOT POINT, and it is the linear filter that does the resolve. The
     * scene target is exactly twice the backbuffer on each axis, so one bilinear
     * tap at an output pixel's centre lands dead between four source texels and
     * returns their average — the whole supersample resolved by the sampler,
     * with no loop and no second target. A point sampler here would keep one
     * sample in four and throw the other three away, which is the supersample
     * paid for and not collected. */
    encoder.bindTexture(0, sceneColour_, linearSampler_);

    /* THE OCCLUSION PLANE, for the diagnostic view. Bound unconditionally
     * because a pipeline's bindings are the same every frame — branching on the
     * debug view here would leave slot 1 holding whatever a previous pass put
     * there on the frames the view is off. */
    encoder.bindTexture(1, occlusionBlurred_, linearSampler_);
    encoder.bindUniformBuffer(1, resolveBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

void ScenePipeline::drawBackbuffer(const SceneFrame& frame)
{
    /* A PASS WITH AN ATTACHMENT CARRYING NO TEXTURE IS THE BACKBUFFER — that is
     * how a screen pass states its load and store actions, since the backbuffer
     * is not a texture the engine owns. See framebufferFor in the GL backend. */
    PassDesc pass;
    pass.name = "backbuffer";
    pass.colours[0].load  = LoadAction::Clear;
    pass.colours[0].store = StoreAction::Store;
    pass.colours[0].clearTo = ClearColour{ frame.clearColour[0], frame.clearColour[1],
                                           frame.clearColour[2], frame.clearColour[3] };
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);

    /* Nothing draws to the screen yet: no pass that produces a picture has been
     * converted. The shadow map above runs and is correct, and there is no lit
     * pass to sample it — the honest state of a migration one pass in. */

    device_.endPass(encoder);
}

void ScenePipeline::render(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("scene pipeline");

    if (!ready_) return;

    /* THE SUN FIRST, because the lit pass samples what it writes. Then the
     * camera's own depth and normals, which the occlusion and decal passes are
     * both unprojected from. The order is the pipeline's and not the caller's,
     * which is the point of the caller not being able to express one. */
    drawShadowMap(frame, geometry);

    /* AND WHAT GOT THROUGH IT. Immediately after, because it depth-tests
     * against the map the pass above just wrote. */
    drawShadowTransmission(frame, geometry);

    /* THEN THE REFLECTION PROBES, and the position in the order is forced from
     * both sides.
     *
     * AFTER THE SUN, because a capture is the lit pass from another eye: it
     * samples the shadow map and the transmission plane the two calls above
     * just wrote, so running it first would put last frame's shadows inside
     * this frame's reflections. On the first frame it would put none at all.
     *
     * BEFORE EVERYTHING THE CAMERA DRAWS, because it writes litBlock_ with its
     * own matrix and its own eye. drawLitScene re-uploads that block from
     * scratch, so the capture cannot leave anything behind — but only in this
     * direction. Moving it after the lit pass would leave the transparent pass,
     * which deliberately reuses the block rather than re-uploading it, drawing
     * the camera's glass through a cube face's projection. */
    drawProbeCapture(frame, geometry);

    /* AND THE CHAIN OVER WHAT IT WROTE. Immediately after, because it reads the
     * faces that pass just captured — and it runs at most once per frame, on a
     * probe whose six faces are all current, so most frames it returns without
     * opening a pass at all. */
    drawProbePrefilter();

    drawPrepass(frame, geometry);

    /* Occlusion AFTER the prepass, because it is unprojected from what the
     * prepass wrote — depth to reconstruct a position, normals to orient the
     * sampling hemisphere. */
    drawOcclusion(frame);

    /* AND THE BLUR THAT FINISHES IT. Not separable from the pass above: the
     * occlusion kernel is rotated per pixel to turn banding into noise, and
     * this is what removes the noise. Running one without the other leaves a
     * grainy four-pixel field over every lit surface. */
    drawOcclusionBlur(frame);

    /* THE SKY, BEFORE ANY COLOUR GOES INTO THE SCENE TARGET — it is what the
     * lit pass loads rather than clears. After the depth passes rather than
     * before them only because it does not interact with depth at all; putting
     * it here keeps the two geometry passes adjacent. */
    drawSky(frame);

    /* THEN THE PICTURE. The lit pass tests Equal against the prepass's depth
     * and samples both the shadow map and the occlusion plane, so it has to
     * follow all three. The resolve leaves linear space and is therefore last —
     * anything drawn after it is drawing in display colour, which is where the
     * HUD and the overlays will go. */
    drawLitScene(frame, geometry);

    /* THEN WHAT YOU CAN SEE THROUGH, after the opaque scene because it reads
     * what is already in the colour buffer. A blended surface drawn before the
     * wall behind it has nothing to see through to and reads as a tinted
     * solid. */
    drawTransparent(frame, geometry);

    drawResolve(frame);
}

void ScenePipeline::drawTransparent(const SceneFrame& frame, IGeometrySource& geometry)
{
    CW_PROFILE_ZONE_N("transparent");

    if (!sceneColour_.valid() || !sceneDepth_.valid()) return;

    /* THE SAME BLOCK THE LIT PASS FILLED, unchanged and not re-uploaded. The
     * transparent pass is the lit pass over different geometry with a different
     * blend state; the sun, the sky and the shadow projection are the frame's,
     * not the pass's. */

    PassDesc pass;
    pass.name = "transparent";

    pass.colours[0].texture = sceneColour_;
    pass.colours[0].load    = LoadAction::Load;
    pass.colours[0].store   = StoreAction::Store;
    pass.colourCount = 1;

    /* DEPTH TESTED, NOT WRITTEN — see the pipeline's DepthState for why. */
    pass.hasDepth      = true;
    pass.depth.texture = sceneDepth_;
    pass.depth.load    = LoadAction::Load;
    pass.depth.store   = StoreAction::Store;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(transparentPipeline_);
    encoder.bindTexture(1, occlusionBlurred_, pointSampler_);
    encoder.bindTexture(2, shadowDepth_, pointSampler_);
    encoder.bindTexture(3, shadowTransmission_, pointSampler_);

    /* THE PROBES, AND THIS IS THE PASS THEY WERE BUILT FOR. Glass at roughness
     * 0.05 sits far below the ramp where the term fades back to the analytic
     * sky, so a window here shows its room's cubemap essentially at full
     * strength — which is most of what makes a pane read as a pane rather than
     * as a hole. See rhi/transparent.fs.glsl. */
    encoder.bindTexture(4, probes_.valid() ? probes_.texture() : probes_.emptyTexture(),
                        probes_.sampler());
    encoder.bindUniformBuffer(0, probes_.block());

    encoder.bindUniformBuffer(1, litBlock_);
    encoder.bindUniformBuffer(2, materialBlock_);

    geometry.submit(encoder, GeometryPass::Transparent);

    device_.endPass(encoder);
}

}  // namespace cromwell
