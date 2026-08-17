#include "cromwell/render/ScenePipeline.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/decal/DeviceDecalSet.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"

#include <cstring>
#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/debug/DebugDraw.hpp"
#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/render/ObjectPush.hpp"
#include "cromwell/render/RenderAssets.hpp"
#include "cromwell/render/RenderScene.hpp"
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
/* ---- the debug line vertex ----------------------------------------------
 *
 * Position and a packed colour, sixteen bytes. No normal, no UV: a debug
 * segment is already in world space and there is nothing to light or sample.
 * The struct is ScenePipeline::DebugVertex; only the stride is needed here, and
 * the two are pinned together by the assert in ensureDebugCapacity. */
constexpr uint32_t kDebugVertexStride = 16;

VertexLayout debugLineLayout()
{
    VertexLayout layout;
    layout.stride = kDebugVertexStride;
    layout.attributeCount = 2;
    layout.attributes[0] = { 0, 0,  VertexFormat::Float3 };
    layout.attributes[1] = { 1, 12, VertexFormat::UByte4Normalised };
    return layout;
}

/* PACKED LINEAR, NOT sRGB — and this is the opposite of what the raylib debug
 * renderer does, for a reason worth stating.
 *
 * That one encodes to sRGB because it draws into an already tone-mapped frame.
 * This draws into the LINEAR scene target, ahead of the resolve, so the value
 * written has to be linear radiance and the filmic curve turns it back into the
 * colour that was asked for. Encoding here would apply the curve twice and
 * every debug colour would come out pale.
 *
 * Eight bits of a linear value crushes the darks, which for saturated debug
 * primaries is invisible and for a dim grey line would not be. Debug colours
 * are saturated primaries on purpose — see debugColour in DebugDraw.hpp. */
std::uint32_t packLinear(const DebugColour& colour)
{
    const auto quantise = [](float value) {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
    };
    return quantise(colour.r) | (quantise(colour.g) << 8)
         | (quantise(colour.b) << 16) | (quantise(colour.a) << 24);
}

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

    /* x = WHICH LIGHTING TERMS ARE SUPPRESSED, as RenderEffects' bits in a
     * float. yzw spare.
     *
     * A FLOAT CARRYING A BIT MASK, which deserves a word rather than a wince.
     * std140 would take an `int` here perfectly well, but every other member of
     * this block is a float vector and a mixed block is one more thing for the
     * C++ and GLSL halves to disagree about silently. The mask uses five bits;
     * a float represents every integer to 2^24 exactly, so there is no rounding
     * to reason about and the shader's `int(x + 0.5)` is exact.
     *
     * ZERO IS THE ORDINARY IMAGE. See SceneFrame::effectSuppress on why the
     * polarity is suppression rather than enablement — a pass that forgets to
     * fill this gets the real frame, not a black one. */
    float effectMask[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    /* x = decals on, y = what a full emissive mask is worth in linear radiance.
     * See rhi/scene_block.glsl, which is the other half of this contract. */
    float decalParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(LitBlockData) == 304, "std140: 2 mat4 + 11 vec4");

/* The outline's block. The C++ half of rhi/post/outline.fs.glsl.
 *
 * TWO SIZES, NOT ONE, and that is the whole reason this block grew a vec4. The
 * pass draws into the resolved frame at surface resolution but READS the custom
 * stencil at the supersampled one, so it needs the source's dimensions to fetch
 * texels and the ratio between them to know how many belong to each output
 * pixel. Carrying only the output size — which is what this held before — is
 * what made the shader assume the two were the same. */
struct OutlineBlockData {
    /* x = id, y = thickness in OUTPUT pixels, zw = the STENCIL's size in texels */
    float outline[4]  = { 0.0f, 2.0f, 1.0f, 1.0f };

    /* x = stencil texels per output pixel on each axis — the outline's own
     *     supersample, which chooses the shader's sample pattern.
     * y = stencil texels per SCENE-DEPTH texel on each axis. One when the two
     *     factors match, two at 4x, four at 8x. This is what keeps the occlusion
     *     test honest: the shader reduces a y-by-y block of custom depth down to
     *     the scene's grid before comparing, so the two sides of the comparison
     *     always describe the same footprint. Getting a sub-texel offset in here
     *     is the whole bug this pass was rebuilt to remove.
     * zw = the SCENE DEPTH's size in texels, for clamping those fetches. */
    float sampling[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    float visible[4]  = { 1.0f, 0.85f, 0.30f, 1.0f };
    float occluded[4] = { 0.35f, 0.55f, 0.90f, 0.75f };
};
static_assert(sizeof(OutlineBlockData) == 64, "std140: four vec4");

/* ---- the decal pass's two blocks -----------------------------------------
 *
 * The C++ half of rhi/decal_blocks.glsl, and one contract written twice — there
 * is no reflection on the explicit backends to check the two agree, so the
 * static_asserts below and that file are what keep them honest.
 *
 * PASS FREQUENCY AND OBJECT FREQUENCY, bindings 1 and 3 from CONVENTIONS.md's
 * table. The pass block is uploaded once for the whole decal pass; the object
 * block is one slice of a buffer holding every decal. */
struct DecalPassBlockData {
    Mat4  viewProjection;
    Mat4  inverseViewProjection;
    float resolution[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
};
static_assert(sizeof(DecalPassBlockData) == 144, "std140: 2 mat4 + 1 vec4");

/* ---- the visibility capture's own block ----------------------------------
 * The C++ half of rhi/scene/decal_visibility.vs.glsl. One face's matrix and the
 * point it was taken from; the fragment stage measures from that point. */
struct DecalCaptureBlockData {
    Mat4  viewProjection;
    float origin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   /* xyz world, w = reach */
};
static_assert(sizeof(DecalCaptureBlockData) == 80, "std140: 1 mat4 + 1 vec4");

/* HOW MANY DECALS CAN CARRY A CAPTURE AT ONCE, and how big each face is.
 *
 * SIXTY-FOUR AND THIRTY-TWO, which is 1.5 MB of R32F. The count is a budget
 * rather than a limit: past it the oldest capture is evicted and that decal
 * falls back to inking everything inside its box, which is the pass's old
 * behaviour and visibly wrong only where something solid is in the way.
 *
 * THIRTY-TWO PIXELS A FACE IS NOT A COMPROMISE. The question being asked of
 * this texture is "is anything in the way", not "where exactly is its edge" —
 * an occluder that fits inside a texel at the distance it sits is smaller than
 * the decal's own texel, and the shader's relative bias is sized to match. */
constexpr int kDecalCaptureSlots = 64;
constexpr uint32_t kDecalCaptureSize = 64;

/* THE ARC ONE TEXEL SUBTENDS, which is what the shader's bias is built from.
 * A cube face spans 90 degrees across its width, so a texel covers 2/size of
 * the distance to whatever it is looking at. Sent to the shader rather than
 * restated there — see uWrap.z. */
constexpr float kDecalCaptureTexelArc = 2.0f / static_cast<float>(kDecalCaptureSize);

/* CLEARED TO SOMETHING FAR BIGGER THAN ANY BOX. An empty direction has to mean
 * "nothing in the way" — clearing to zero would say every direction is blocked
 * at the origin and no decal would ever ink anything. */
constexpr float kDecalCaptureEmpty = 1.0e6f;

/* HOW FAR OFF THE SURFACE THE CAPTURE IS TAKEN. Measured from the placement
 * surface itself, every ray along that surface grazes it and the comparison
 * turns into shadow acne on the decal's own receiver. Five centimetres is
 * clear of that and still inside any room the decal could be in. */
constexpr float kDecalCaptureLift = 0.05f;

/* AND THE CAPTURE'S NEAR PLANE, WHICH MUST BE FAR SMALLER THAN THAT LIFT.
 *
 * THIS IS THE BUG THAT MADE CORNERS BLEED, and it is worth stating plainly
 * because nothing about it looks like a decal problem. A perspective capture
 * deletes everything closer to the eye than its near plane. Reusing the probe's
 * — 5 cm, which for a probe floating in the middle of a room is nothing at all
 * — put the near plane exactly where a decal's own lift puts the origin, so any
 * surface within 5 cm of it was clipped out of the cube entirely.
 *
 * In a corner that surface is the adjoining wall. The capture then reads
 * "nothing in the way" in precisely the directions the wall occupies, and the
 * decal projects straight through it onto whatever the camera can see beyond —
 * a patch of the mark on the far side, at corners only, because a fold is the
 * only place geometry comes that close to the origin.
 *
 * Half a centimetre: under any lift, under any wall, and a capture that reaches
 * a couple of metres still has all the depth precision it needs across that
 * range. */
constexpr float kDecalCaptureNear = 0.005f;

struct DecalObjectBlockData {
    Mat4  model;
    Mat4  inverseModel;
    float tint[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float factors[4] = { 0.9f, 0.0f, 1.0f, 1.0f };   /* rough, metal, nrm, opacity */
    float fade[4]    = { 0.40f, 0.70f, 0.15f, 0.0f };
    float wrap[4]    = { 1.0f, 0.0f, 0.0f, 0.0f };

    /* xyz = where this decal's visibility capture was taken from, w = which
     * cube of the array holds it. wrap[1] says whether it has one at all. */
    float capture[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(DecalObjectBlockData) == 208, "std140: 2 mat4 + 5 vec4");

/* HOW FAR APART TWO DECALS' BLOCKS SIT IN THE BUFFER, and it is not
 * sizeof(DecalObjectBlockData).
 *
 * A uniform buffer BINDING OFFSET must be a multiple of the device's alignment
 * — GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, which is 256 on most desktop drivers
 * and is allowed to be larger. Packing the blocks tightly at 192 and binding at
 * that stride is rejected outright on a strict driver and, on a lax one,
 * silently rounded — which hands every decal after the first its neighbour's
 * transform, so the decals land in the wrong places and nothing errors.
 *
 * 256 IS A CONSTANT RATHER THAN A QUERY because the RHI does not expose the
 * alignment and 256 satisfies every desktop and console target; a backend that
 * needed more would be the one to say so. The waste is 64 bytes per decal on a
 * board that has tens of them. */
constexpr uint32_t kDecalBlockStride = 256;
static_assert(kDecalBlockStride >= sizeof(DecalObjectBlockData),
              "a decal's block must fit inside its stride");

/* ---- bloom's budgets, in one place ---------------------------------------
 *
 * HALF THE SCENE TARGET at level 0. The scene is already supersampled 2x, so
 * this is the window's own resolution — bloom is a low-frequency signal and
 * spending four times the bandwidth on it buys nothing visible.
 *
 * SIX LEVELS, so the widest contribution is spread over 1/64th of the image.
 * Past that the level is a handful of texels and each further step adds a
 * flat wash rather than a tail. Both numbers are §4.11 budgets: named here,
 * never assumed by a shader, so a quality preset can move them. */
constexpr uint32_t kBloomDownscale = 2;
constexpr uint32_t kBloomLevels = 6;

/* std140: three vec4, and the same block for all three bloom shaders so none
 * of them can disagree about what a lane means. */
struct BloomBlockData {
    float params[4]      = { 1.1f, 0.55f, 1.0f, 1.0f };  /* threshold, knee, intensity, radius */
    float sourceTexel[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   /* 1/size, then size    */
    float targetSize[4]  = { 1.0f, 1.0f, 0.0f, 0.0f };
};
static_assert(sizeof(BloomBlockData) == 48, "std140: three vec4");

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

    /* WHICH LIGHTING TERMS ARE SWITCHED OFF. Filled HERE rather than at each of
     * the two call sites, so the camera's lit pass and a probe capture cannot
     * disagree about it — a probe capturing a scene lit differently from the
     * one on screen would bake that difference in and keep it for a sweep after
     * the switch was put back, which reads as the probes being broken. */
    block.effectMask[0] = static_cast<float>(frame.effectSuppress);

    /* THE DECALS' SWITCH AND THEIR EMISSIVE SCALE. Here rather than at the call
     * sites for the same reason the effect mask is: a probe capture lighting
     * the world differently from the frame on screen would bake that difference
     * into a cubemap and keep it for a sweep after the switch was put back. */
    block.decalParams[0] = frame.decals ? 1.0f : 0.0f;
    block.decalParams[1] = frame.decalEmissiveScale;

    return block;
}

}  // namespace

ScenePipeline::ScenePipeline(rhi::IRenderDevice& device, RenderAssets& assets)
    : device_(device), assets_(assets)
{
}

/* ---- where the probes live now, and how a pass reaches them --------------*/

DeviceProbeSet* ScenePipeline::probesOf(const View& view)
{
    RenderScene* scene = view.scene();
    return scene != nullptr ? &scene->probes() : nullptr;
}

Aabb ScenePipeline::worldBoundsOf(const View& view)
{
    const RenderScene* scene = view.scene();
    if (scene == nullptr) return Aabb{ Vec3{ 1.0f, 1.0f, 1.0f }, Vec3{ -1.0f, -1.0f, -1.0f } };

    return scene->worldBounds();
}

void ScenePipeline::drawItems(rhi::ICommandEncoder& encoder,
                              const std::vector<DrawItem>& items, bool bindMaterials) const
{
    /* WHAT LAST WAS BOUND, so a run of items sharing a material binds it once.
     * The sort already grouped them — see RenderScene.cpp's opaqueSortKey — so
     * this turns that grouping into the state changes it was for. An invalid id
     * never matches a real one, so a run of unmaterialed items does not
     * accidentally suppress the next real bind. */
    MaterialId bound;

    for (const DrawItem& item : items) {
        if (bindMaterials && item.material != bound) {
            assets_.materials().bind(encoder, item.material);
            bound = item.material;
        }

        /* THE TRANSFORM AND THE TINT, AS THE OBJECT PUSH. Every submitter used
         * to do this for itself, and RhiStatics' header explains at length why
         * skipping it is not safe even for identity: push constants persist on
         * the bound program, so an item that pushed nothing would inherit the
         * previous one's matrix and colour. Here there is no path that can
         * forget, because there is one loop. */
        ObjectPush push;
        push.model = item.transform;
        push.tint[0] = item.tint.x;
        push.tint[1] = item.tint.y;
        push.tint[2] = item.tint.z;
        push.tint[3] = item.tint.w;

        /* AND WHAT THE OBJECT IS, for the custom depth pass. Pushed by the same
         * loop for the same reason: a pass that set it per draw for itself
         * would be a second place to forget, and an item that pushed nothing
         * would inherit the previous object's id — which in a buffer whose
         * whole purpose is telling objects apart is the worst possible
         * failure. */
        push.object[0] = static_cast<float>(item.customStencil);

        encoder.pushConstants(&push, sizeof push);
        encoder.draw(item.mesh);
    }
}

void ScenePipeline::addPass(ScenePassPoint point, IScenePass& pass)
{
    hatchPasses_.push_back(HatchPass{ point, &pass });
}

void ScenePipeline::runHatch(ScenePassPoint point, const View& view)
{
    if (hatchPasses_.empty()) return;

    ScenePassContext context;
    context.device = &device_;
    context.view = &view;
    context.resources.sceneColour = sceneColour_;
    context.resources.sceneDepth = sceneDepth_;
    context.resources.sceneNormals = sceneNormals_;
    context.resources.occlusion = occlusionBlurred_;
    context.resources.shadowMap = shadowDepth_;
    context.resources.shadowTransmission = shadowTransmission_;
    context.resources.sceneWidth = sceneWidth();
    context.resources.sceneHeight = sceneHeight();
    context.resources.surfaceWidth = targetWidth_;
    context.resources.surfaceHeight = targetHeight_;

    for (const HatchPass& entry : hatchPasses_) {
        if (entry.point != point || entry.pass == nullptr) continue;

        /* NAMED IN A CAPTURE TOOL, so a custom pass is attributable in
         * RenderDoc and Nsight without the game having to remember to do it.
         * The engine knows the name; it should spend it. */
        CW_PROFILE_ZONE_N("hatch pass");
        entry.pass->execute(context);
    }
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

    if (outlinePipeline_.valid())       device_.destroy(outlinePipeline_);
    if (outlineShader_.valid())         device_.destroy(outlineShader_);
    if (outlineBlock_.valid())          device_.destroy(outlineBlock_);
    if (customStencilPipeline_.valid()) device_.destroy(customStencilPipeline_);
    if (customStencilShader_.valid())   device_.destroy(customStencilShader_);
    if (customStencilBlock_.valid())    device_.destroy(customStencilBlock_);
    if (customDepth_.valid())           device_.destroy(customDepth_);
    if (customStencil_.valid())         device_.destroy(customStencil_);

    if (decalPipeline_.valid())      device_.destroy(decalPipeline_);
    if (decalShader_.valid())        device_.destroy(decalShader_);
    if (decalCube_.valid())          device_.destroy(decalCube_);
    if (decalCubeVertices_.valid())  device_.destroy(decalCubeVertices_);
    if (decalObjectBlocks_.valid())  device_.destroy(decalObjectBlocks_);
    if (decalCaptureBlock_.valid())   device_.destroy(decalCaptureBlock_);
    if (decalCaptureSampler_.valid()) device_.destroy(decalCaptureSampler_);
    if (decalCapturePipeline_.valid()) device_.destroy(decalCapturePipeline_);
    if (decalCaptureShader_.valid())  device_.destroy(decalCaptureShader_);
    if (decalCaptureDepth_.valid())   device_.destroy(decalCaptureDepth_);
    if (decalVisibility_.valid())     device_.destroy(decalVisibility_);
    if (decalPassBlock_.valid())     device_.destroy(decalPassBlock_);
    if (decalSurface_.valid())       device_.destroy(decalSurface_);
    if (decalNormal_.valid())        device_.destroy(decalNormal_);
    if (decalAlbedo_.valid())        device_.destroy(decalAlbedo_);

    if (bloomCompositePipeline_.valid()) device_.destroy(bloomCompositePipeline_);
    if (bloomUpPipeline_.valid())        device_.destroy(bloomUpPipeline_);
    if (bloomUpShader_.valid())          device_.destroy(bloomUpShader_);
    if (bloomDownPipeline_.valid())      device_.destroy(bloomDownPipeline_);
    if (bloomDownShader_.valid())        device_.destroy(bloomDownShader_);
    if (bloomPrefilterPipeline_.valid()) device_.destroy(bloomPrefilterPipeline_);
    if (bloomPrefilterShader_.valid())   device_.destroy(bloomPrefilterShader_);
    if (bloomBlock_.valid())             device_.destroy(bloomBlock_);
    if (bloomChain_.valid())             device_.destroy(bloomChain_);
    for (SamplerHandle sampler : bloomLevelSamplers_)
        if (sampler.valid()) device_.destroy(sampler);
    bloomLevelSamplers_.clear();

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

    /* ---- and the bloom chain, at half the scene and six levels deep -------
     *
     * SIZED FROM THE SCENE TARGET AND REBUILT WITH IT, which is what keeps a
     * resize from leaving the chain describing the previous window. The
     * dimensions are clamped to one rather than allowed to reach zero: six
     * halvings of a small window would otherwise ask the device for a
     * zero-width level, which is a creation failure that reads as bloom simply
     * not appearing.
     *
     * RGBA16F, matching what it reads and what it writes back into. An 8-bit
     * chain would clip exactly the values this pass exists to spread. */
    TextureDesc bloomChain;
    bloomChain.name      = "bloom chain";
    bloomChain.width     = std::max(width / kBloomDownscale, 1u);
    bloomChain.height    = std::max(height / kBloomDownscale, 1u);
    bloomChain.mipLevels = kBloomLevels;
    bloomChain.format    = TextureFormat::RGBA16F;
    bloomChain.usage     = TextureUsageSampled | TextureUsageRenderTarget;

    if (bloomChain_.valid()) device_.destroy(bloomChain_);
    bloomChain_ = device_.createTexture(bloomChain);

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

    /* ---- the DBuffer, at HALF the scene target ---------------------------
     *
     * Which is the surface's own resolution, given the 2x supersample. The lit
     * pass samples these bilinearly and lands decal detail at display
     * resolution, where it is seen; the supersample exists for the hard
     * geometric edges of untextured boxes, not for texture detail. At the full
     * scene size these three would be 48 MB carrying a signal nothing can
     * resolve.
     *
     * THE ALBEDO IS RGBA16F AND THE OTHER TWO ARE RGBA8. Colour needs the
     * range — see rhi/dbuffer.glsl on why this path keeps the plane linear
     * where the raylib one stores it sRGB-encoded in eight bits — and a normal
     * plus three 0..1 scalars have none to lose. */
    const uint32_t decalWidth  = std::max(surfaceWidth, 1u);
    const uint32_t decalHeight = std::max(surfaceHeight, 1u);

    TextureDesc dbuffer;
    dbuffer.name   = "dbuffer albedo";
    dbuffer.width  = decalWidth;
    dbuffer.height = decalHeight;
    dbuffer.format = TextureFormat::RGBA16F;
    dbuffer.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    if (decalAlbedo_.valid()) device_.destroy(decalAlbedo_);
    decalAlbedo_ = device_.createTexture(dbuffer);

    dbuffer.format = TextureFormat::RGBA8;

    dbuffer.name = "dbuffer normal";
    if (decalNormal_.valid()) device_.destroy(decalNormal_);
    decalNormal_ = device_.createTexture(dbuffer);

    dbuffer.name = "dbuffer surface";
    if (decalSurface_.valid()) device_.destroy(decalSurface_);
    decalSurface_ = device_.createTexture(dbuffer);

    /* ---- custom depth / stencil, SUPERSAMPLED like every other scene target
     *
     * THESE WERE AT THE SURFACE'S RESOLUTION AND IT WAS WRONG TWICE OVER. The
     * reasoning for the smaller size was that the consumer is a post-resolve
     * pass working in output pixels, so anything finer would only be
     * downsampled before it was read. Both halves of that turn out to be the
     * bug rather than the saving:
     *
     * ONE — THE OUTLINE HAD NO COVERAGE TO AVERAGE. One texel per output pixel
     * means the edge test answers yes or no and the silhouette is a 1-bit mask,
     * which stair-steps at any resolution. Four texels per output pixel is what
     * lets the shader count them and emit a fraction, and a fraction is the
     * only thing that antialiases an edge. Unreal reaches the same five levels
     * with 4x MSAA on a private editor target and averages the samples the same
     * way; we already pay for the supersample, so ours is free.
     *
     * TWO — THE DEPTH COMPARISON WAS MEASURING THE WRONG PLACE. sceneDepth_ is
     * supersampled. Sampling a 2x buffer and a 1x buffer at one UV does not
     * read the same point: GL_NEAREST takes floor(u * size), so the scene tap
     * landed on the same corner of the 2x2 block every time — a quarter of a
     * pixel down and right of where the custom depth was taken. A consistent
     * offset, not a wobble, and on a sloped surface a real depth difference.
     * That is what the old kBias in the shader was hiding. Matching the sizes
     * makes both fetches the same texel and the comparison exact.
     *
     * AND THE FACTOR IS ITS OWN DIAL, not the scene's — see
     * withOutlineSupersample. Matching the scene at 2x removes the bug above but
     * still leaves only two distinct sample positions per axis, which is three
     * coverage levels on a near-vertical edge and a staircase the eye finds on
     * high-contrast ink. Four positions is what a rotated sample pattern needs,
     * so the default is 4x and the dial goes to 8x. It is floored at the scene's
     * factor precisely so reason TWO above can never come back.
     *
     * The cost is one RGBA8 and one D32F at the square of the factor. See
     * study/topics/rendering/outline_antialiasing.md for the two engines this
     * was read from and why neither solves it with an AA pass. */
    const uint32_t tagWidth  = std::max(surfaceWidth  * outlineSupersample_, 1u);
    const uint32_t tagHeight = std::max(surfaceHeight * outlineSupersample_, 1u);

    TextureDesc tag;
    tag.name   = "custom stencil";
    tag.width  = tagWidth;
    tag.height = tagHeight;
    tag.format = TextureFormat::RGBA8;
    tag.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    if (customStencil_.valid()) device_.destroy(customStencil_);
    customStencil_ = device_.createTexture(tag);

    tag.name   = "custom depth";
    tag.format = TextureFormat::D32F;
    if (customDepth_.valid()) device_.destroy(customDepth_);
    customDepth_ = device_.createTexture(tag);

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

ScenePipeline& ScenePipeline::withOutlineSupersample(uint32_t factor)
{
    /* SNAPPED TO 2, 4 OR 8, AND NEVER BELOW THE SCENE'S OWN FACTOR.
     *
     * The floor is the part that is not a matter of taste. A stencil COARSER
     * than sceneDepth_ cannot have its occlusion test aligned by any means: the
     * outline would be comparing an object depth on one grid against a scene
     * depth on a finer one, which is exactly the quarter-pixel defect that made
     * the old kBias necessary. A quality dial must not have that at its low end,
     * so the cheapest setting is PARITY — already correct, not merely cheap.
     *
     * The snapping is because the shader picks its sample pattern per factor and
     * a factor of three has no rotated pattern to pick. */
    uint32_t snapped = kSupersample;
    if      (factor >= 8) snapped = 8;
    else if (factor >= 4) snapped = 4;

    if (snapped == outlineSupersample_) return *this;
    outlineSupersample_ = snapped;

    /* Only rebuild once there is something to rebuild — set before initialise()
     * this is just a stored preference, and createSceneTargets would be sizing
     * targets against a surface nobody has reported yet. */
    if (ready_ && targetWidth_ != 0 && targetHeight_ != 0)
        createSceneTargets(targetWidth_, targetHeight_);

    return *this;
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
    const std::string vertexSource   = ShaderLibrary::preprocess("rhi/scene/depth_only.vs.glsl");
    const std::string fragmentSource = ShaderLibrary::preprocess("rhi/scene/depth_only.fs.glsl");

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
        ShaderLibrary::preprocess("rhi/scene/transmission.vs.glsl");
    const std::string transmissionFragment =
        ShaderLibrary::preprocess("rhi/scene/transmission.fs.glsl");

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

    const std::string prepassVertex   = ShaderLibrary::preprocess("rhi/scene/prepass.vs.glsl");
    const std::string prepassFragment = ShaderLibrary::preprocess("rhi/scene/prepass.fs.glsl");

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

    const std::string occlusionSource = ShaderLibrary::preprocess("rhi/post/ssao.fs.glsl");
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

    const std::string blurSource = ShaderLibrary::preprocess("rhi/post/ssao_blur.fs.glsl");
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

    const std::string skySource = ShaderLibrary::preprocess("rhi/scene/sky.fs.glsl");
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

    const std::string litVertex   = ShaderLibrary::preprocess("rhi/scene/lit.vs.glsl");
    const std::string litFragment = ShaderLibrary::preprocess("rhi/scene/lit.fs.glsl");
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
        ShaderLibrary::preprocess("rhi/scene/transparent.fs.glsl");
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

    /* ---- debug lines, two pipelines over one buffer ----------------------
     *
     * NOT FATAL IF THEY FAIL. Every other pipeline here is the frame; these are
     * a diagnostic, and a renderer that refused to start because a debug shader
     * was missing would be one nobody could use to find out why. The draw is
     * skipped and the log says so. */
    const std::string debugVertex   = ShaderLibrary::preprocess("rhi/scene/debug_line.vs.glsl");
    const std::string debugFragment = ShaderLibrary::preprocess("rhi/scene/debug_line.fs.glsl");

    if (debugVertex.empty() || debugFragment.empty()) {
        LOGGER.warn("ScenePipeline: rhi/scene/debug_line shaders not found - "
                    "debug lines will not be drawn");
    } else {
        debugShader_ = device_.createShader("debug line", debugVertex.c_str(),
                                            debugFragment.c_str());
    }

    if (debugShader_.valid()) {
        PipelineDesc debug;
        debug.name         = "debug line";
        debug.shader       = debugShader_;
        debug.vertexLayout = debugLineLayout();

        debug.colourFormats[0] = TextureFormat::RGBA16F;   /* the scene target */
        debug.colourCount      = 1;
        debug.depthFormat      = TextureFormat::D32F;

        debug.raster.primitive = PrimitiveType::Lines;
        debug.raster.cull      = CullMode::None;

        /* STRAIGHT ALPHA, so a caller can fade a line out. Nothing in the debug
         * API asks for it yet; it costs nothing and its absence would be a
         * surprise the first time someone passes a colour with an alpha. */
        debug.blend = BlendState::alpha();

        /* DEPTH TESTED, NEVER WRITTEN. A line that wrote depth would occlude the
         * next one, and the twelve edges of a debug box would hide each other at
         * every corner. */
        debug.depth.test    = true;
        debug.depth.write   = false;
        debug.depth.compare = CompareFunc::LessEqual;

        debugDepthPipeline_ = device_.createPipeline(debug);

        debug.name       = "debug line xray";
        debug.depth.test = false;

        debugXrayPipeline_ = device_.createPipeline(debug);

        if (!debugDepthPipeline_.valid() || !debugXrayPipeline_.valid())
            LOGGER.warn("ScenePipeline: a debug line pipeline could not be built");
    }

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

    /* ---- the decals ------------------------------------------------------ */

    const std::string decalVertexSource =
        ShaderLibrary::preprocess("rhi/scene/decal.vs.glsl");
    const std::string decalSource = ShaderLibrary::preprocess("rhi/scene/decal.fs.glsl");

    if (decalVertexSource.empty() || decalSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/scene/decal shaders not found");
        return false;
    }

    decalShader_ = device_.createShader("decal", decalVertexSource.c_str(),
                                        decalSource.c_str());
    if (!decalShader_.valid()) return false;

    PipelineDesc decal;
    decal.name   = "decals";
    decal.shader = decalShader_;

    /* POSITION ONLY. Nothing reads a normal, a UV or a colour from the box —
     * its own faces are never shaded, and every fragment recovers the real
     * surface underneath from the depth buffer instead. */
    decal.vertexLayout.stride = sizeof(float) * 3;
    decal.vertexLayout.attributeCount = 1;
    decal.vertexLayout.attributes[0] = { 0, 0, VertexFormat::Float3 };

    /* THREE PLANES, and the formats must match createSceneTargets exactly — a
     * pipeline bakes them, so a mismatch is an incomplete framebuffer rather
     * than a wrong picture. */
    decal.colourFormats[0] = TextureFormat::RGBA16F;
    decal.colourFormats[1] = TextureFormat::RGBA8;
    decal.colourFormats[2] = TextureFormat::RGBA8;
    decal.colourCount = 3;

    /* BACK FACES ONLY, and it is the front faces that are wrong rather than a
     * preference: they are clipped away the moment the camera enters the box,
     * which for a decal the size of a road tile is most of the time. */
    decal.raster.cull = CullMode::Front;

    /* NO DEPTH AT ALL. The box is a bounding volume and the shader decides
     * which fragments survive against its own bounds; leaving the test on would
     * reject exactly those whose receiver is in front of the box's far side. */
    decal.depth.test  = false;
    decal.depth.write = false;

    /* SEPARATE FACTORS, AND THIS IS THE WHOLE DBUFFER.
     *
     *   rgb   = src.rgb + dst.rgb * src.a    (over, premultiplied)
     *   alpha =           dst.a   * src.a    (transmittance multiplies)
     *
     * so N overlapping decals leave dst.a holding exactly the fraction of the
     * base material still showing through, and dst.rgb their own contribution
     * already summed. The lit shader's whole decode is then one fused
     * multiply-add. Getting the alpha factors wrong here does not fail — it
     * makes the second decal over a spot replace the first instead of
     * compositing with it. */
    decal.blend.enabled      = true;
    decal.blend.sourceColour = BlendFactor::One;
    decal.blend.destColour   = BlendFactor::SrcAlpha;
    decal.blend.sourceAlpha  = BlendFactor::Zero;
    decal.blend.destAlpha    = BlendFactor::SrcAlpha;

    decalPipeline_ = device_.createPipeline(decal);
    if (!decalPipeline_.valid()) return false;

    /* ---- the visibility capture ------------------------------------------
     *
     * A cube of distances per decal, rendered from where that decal was thrown,
     * so the pass can tell a stair riser from the far side of a wall. See the
     * members in the header and rhi/scene/decal_visibility.vs.glsl. */
    const std::string captureVertexSource =
        ShaderLibrary::preprocess("rhi/scene/decal_visibility.vs.glsl");
    const std::string captureSource =
        ShaderLibrary::preprocess("rhi/scene/decal_visibility.fs.glsl");

    if (captureVertexSource.empty() || captureSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/scene/decal_visibility shaders not found");
        return false;
    }

    decalCaptureShader_ = device_.createShader("decal visibility",
                                               captureVertexSource.c_str(),
                                               captureSource.c_str());
    if (!decalCaptureShader_.valid()) return false;

    TextureDesc visibility;
    visibility.name   = "decal visibility";
    visibility.width  = kDecalCaptureSize;
    visibility.height = kDecalCaptureSize;
    visibility.layers = kDecalCaptureSlots * 6;
    visibility.cube   = true;

    /* R32F, AND THE RANGE IS THE REASON RATHER THAN THE PRECISION. The value is
     * a world distance compared against another world distance; an 8-bit
     * normalised format would have to agree on a scale with the shader, and a
     * half float loses a centimetre at forty metres — which is inside the bias,
     * but only by accident, and nothing here is short of memory. */
    visibility.format = TextureFormat::R32F;
    visibility.usage  = TextureUsageSampled | TextureUsageRenderTarget;
    decalVisibility_ = device_.createTexture(visibility);
    if (!decalVisibility_.valid()) return false;

    /* ONE DEPTH BUFFER FOR EVERY FACE OF EVERY SLOT. It sorts the nearest
     * surface within one face and is never read afterwards, so it is cleared at
     * the start of each face and discarded at the end — the same arrangement
     * the probe capture's depth has, for the same reason. */
    TextureDesc captureDepth;
    captureDepth.name   = "decal visibility depth";
    captureDepth.width  = kDecalCaptureSize;
    captureDepth.height = kDecalCaptureSize;
    captureDepth.format = TextureFormat::D32F;
    captureDepth.usage  = TextureUsageDepthTarget;
    decalCaptureDepth_ = device_.createTexture(captureDepth);
    if (!decalCaptureDepth_.valid()) return false;

    SamplerDesc captureSampler;

    /* POINT, and it has to be. Filtering a distance field across a silhouette
     * averages the near surface with whatever is kilometres behind it and
     * produces a distance that describes neither — a halo of ink around every
     * occluder's edge, which is exactly the artefact this texture exists to
     * remove. */
    captureSampler.minify  = FilterMode::Nearest;
    captureSampler.magnify = FilterMode::Nearest;
    captureSampler.mip     = FilterMode::Nearest;
    captureSampler.wrapU     = WrapMode::ClampToEdge;
    captureSampler.wrapV     = WrapMode::ClampToEdge;
    captureSampler.wrapW     = WrapMode::ClampToEdge;
    decalCaptureSampler_ = device_.createSampler(captureSampler);
    if (!decalCaptureSampler_.valid()) return false;

    PipelineDesc capture;
    capture.name   = "decal visibility";
    capture.shader = decalCaptureShader_;

    /* POSITION ONLY — the fragment stage wants a world position and gets it
     * from the vertex stage's own transform, not from an attribute. */
    capture.vertexLayout.stride = sizeof(float) * 3;
    capture.vertexLayout.attributeCount = 1;
    capture.vertexLayout.attributes[0] = { 0, 0, VertexFormat::Float3 };

    capture.colourFormats[0] = TextureFormat::R32F;
    capture.colourCount = 1;
    capture.depthFormat = TextureFormat::D32F;

    /* THE DEPTH TEST IS WHAT MAKES THE STORED VALUE THE NEAREST SURFACE, which
     * is the only one that can occlude anything. */
    capture.depth.test    = true;
    capture.depth.write   = true;
    capture.depth.compare = CompareFunc::Less;

    /* BOTH SIDES KEPT, matching the prepass. A cutaway exposes the inside of a
     * wall and the underside of a floor, and a decal thrown into a room that
     * has been opened up still has to be occluded by the walls that remain. */
    capture.raster.cull = CullMode::None;

    decalCapturePipeline_ = device_.createPipeline(capture);
    if (!decalCapturePipeline_.valid()) return false;

    BufferDesc captureBlock;
    captureBlock.name   = "decal capture block";
    captureBlock.bytes  = sizeof(DecalCaptureBlockData);
    captureBlock.usage  = BufferUsageUniform;
    captureBlock.access = BufferAccess::CpuToGpuPerFrame;
    decalCaptureBlock_ = device_.createBuffer(captureBlock);
    if (!decalCaptureBlock_.valid()) return false;

    decalCaptureIds_.assign(kDecalCaptureSlots, -1);
    decalCaptureOrigins_.assign(kDecalCaptureSlots, Vec3{});

    BufferDesc decalPass;
    decalPass.name   = "decal pass block";
    decalPass.bytes  = sizeof(DecalPassBlockData);
    decalPass.usage  = BufferUsageUniform;
    decalPass.access = BufferAccess::CpuToGpuPerFrame;
    decalPassBlock_ = device_.createBuffer(decalPass);
    if (!decalPassBlock_.valid()) return false;

    /* ---- THE PROJECTOR BOX ------------------------------------------------
     *
     * A unit cube spanning -0.5 to +0.5, thirty-six positions, built here
     * rather than through BoxEmitter — that one produces normals, UVs and
     * colours nothing reads, and it takes a raylib Color, which this file must
     * not name. Twelve triangles written out is smaller than the conversion
     * would be.
     *
     * ============ EVERY FACE IS WOUND CCW SEEN FROM OUTSIDE ================
     *
     * Which is what `raster.winding` (CounterClockwise, the default) and
     * `raster.cull = Front` above together mean: GL throws away the face whose
     * OUTSIDE is toward the camera and keeps its far side, so the box is always
     * rasterised by its BACK faces and the camera may sit inside it.
     *
     * THE FIRST VERSION OF THIS TABLE HAD FOUR OF THE SIX WOUND THE OTHER WAY,
     * AND THE SYMPTOM WAS NOT AN INSIDE-OUT BOX. That is the trap worth writing
     * down. Inconsistent winding on a bounding volume does not read as a
     * modelling error, because nothing ever SHADES these faces — they exist
     * only to make the rasteriser visit the pixels the box covers. What it does
     * instead is punch HOLES in that coverage:
     *
     *   the correctly wound faces contributed their FAR side, as intended
     *   the reversed ones contributed their NEAR side, culling the far one
     *
     * and for a convex solid the near set and the far set each cover the whole
     * silhouette on their own, but a MIXTURE covers neither. Any pixel whose
     * nearest face was reversed and whose farthest face was not is visited by
     * nothing at all, so the decal is simply absent there — in wedges bounded
     * by projected cube edges and by the diagonal each face is split along,
     * moving as the camera moves, since which face is nearest is a property of
     * the view. It reads as the decal clipping or tearing rather than as
     * anything to do with the box, and no amount of staring at the fragment
     * shader finds it.
     *
     * SO CHECK THE CROSS PRODUCT, NOT THE PICTURE: for each triangle
     * (v1-v0) x (v2-v0) must point OUT of the cube, away from the origin. */
    const float kUnitCube[] = {
        /* -X */ -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
                 -0.5f,-0.5f,-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f,
        /* +X */  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
                  0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
        /* -Y */ -0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
                 -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
        /* +Y */ -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
                 -0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,
        /* -Z */ -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
                 -0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
        /* +Z */ -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
                 -0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    };

    BufferDesc cube;
    cube.name   = "decal cube";
    cube.bytes  = sizeof kUnitCube;
    cube.usage  = BufferUsageVertex;
    cube.access = BufferAccess::CpuToGpuOnce;
    decalCubeVertices_ = device_.createBuffer(cube);
    if (!decalCubeVertices_.valid()) return false;

    device_.updateBuffer(decalCubeVertices_, kUnitCube, sizeof kUnitCube, 0);

    decalCube_ = device_.createMesh(decal.vertexLayout, decalCubeVertices_,
                                    sizeof kUnitCube / (sizeof(float) * 3));
    if (!decalCube_.valid()) return false;

    /* ---- custom depth / stencil, and the outline that reads it ----------
     *
     * THE VERTEX STAGE IS depth_only's, unchanged. It takes a pass-level matrix
     * and an object transform and writes depth, which is exactly this pass —
     * the only difference from the shadow map is whose eye the matrix is and
     * what the fragment stage writes. Two passes sharing a vertex shader is the
     * point of it taking a PASS matrix rather than a frame one. */
    const std::string tagSource =
        ShaderLibrary::preprocess("rhi/scene/custom_stencil.fs.glsl");
    const std::string outlineSource =
        ShaderLibrary::preprocess("rhi/post/outline.fs.glsl");

    if (tagSource.empty() || outlineSource.empty()) {
        LOGGER.error("ScenePipeline: rhi custom_stencil / outline shaders not found");
        return false;
    }

    customStencilShader_ = device_.createShader("custom stencil", vertexSource.c_str(),
                                                tagSource.c_str());
    if (!customStencilShader_.valid()) return false;

    PipelineDesc tagPipeline;
    tagPipeline.name             = "custom depth";
    tagPipeline.shader           = customStencilShader_;
    tagPipeline.vertexLayout     = MeshVertexBuffer::deviceLayout();
    tagPipeline.colourFormats[0] = TextureFormat::RGBA8;
    tagPipeline.colourCount      = 1;
    tagPipeline.depthFormat      = TextureFormat::D32F;

    /* ORDINARY DEPTH, WRITTEN. Unlike the lit pass there is no prepass to test
     * Equal against — this IS a prepass, of a different subset. */
    tagPipeline.depth.test    = true;
    tagPipeline.depth.write   = true;
    tagPipeline.depth.compare = CompareFunc::Less;

    customStencilPipeline_ = device_.createPipeline(tagPipeline);
    if (!customStencilPipeline_.valid()) return false;

    BufferDesc tagBlock;
    tagBlock.name   = "custom depth block";
    tagBlock.bytes  = sizeof(PassBlockData);
    tagBlock.usage  = BufferUsageUniform;
    tagBlock.access = BufferAccess::CpuToGpuPerFrame;
    customStencilBlock_ = device_.createBuffer(tagBlock);
    if (!customStencilBlock_.valid()) return false;

    outlineShader_ = device_.createShader("outline", kFullscreenVertex,
                                          outlineSource.c_str());
    if (!outlineShader_.valid()) return false;

    PipelineDesc outline;
    outline.name             = "outline";
    outline.shader           = outlineShader_;
    outline.colourFormats[0] = TextureFormat::RGBA8;
    outline.colourCount      = 1;
    outline.depth.test       = false;
    outline.depth.write      = false;
    outline.raster.cull      = CullMode::None;

    /* STRAIGHT ALPHA over the resolved frame. The shader emits a designer's
     * colour and an alpha, not premultiplied radiance — it is ink, and this is
     * the blend ink wants. */
    outline.blend.enabled      = true;
    outline.blend.sourceColour = BlendFactor::SrcAlpha;
    outline.blend.destColour   = BlendFactor::OneMinusSrcAlpha;
    outline.blend.sourceAlpha  = BlendFactor::One;
    outline.blend.destAlpha    = BlendFactor::OneMinusSrcAlpha;

    outlinePipeline_ = device_.createPipeline(outline);
    if (!outlinePipeline_.valid()) return false;

    BufferDesc outlineDesc;
    outlineDesc.name   = "outline block";
    outlineDesc.bytes  = sizeof(OutlineBlockData);
    outlineDesc.usage  = BufferUsageUniform;
    outlineDesc.access = BufferAccess::CpuToGpuPerFrame;
    outlineBlock_ = device_.createBuffer(outlineDesc);
    if (!outlineBlock_.valid()) return false;

    /* ---- bloom ----------------------------------------------------------
     *
     * THREE SHADERS AND FOUR PIPELINES, and the count is worth explaining
     * because it looks like one too many. The prefilter and the downsample are
     * genuinely different filters; the upsample is one shader used twice, once
     * writing RGBA16F into the chain and once writing RGBA16F into the scene
     * target — two pipelines because a pipeline bakes its target format and its
     * blend state, and neither can be changed from an encoder. */
    const std::string bloomPrefilterSource =
        ShaderLibrary::preprocess("rhi/post/bloom_prefilter.fs.glsl");
    const std::string bloomDownSource =
        ShaderLibrary::preprocess("rhi/post/bloom_down.fs.glsl");
    const std::string bloomUpSource =
        ShaderLibrary::preprocess("rhi/post/bloom_up.fs.glsl");

    if (bloomPrefilterSource.empty() || bloomDownSource.empty() || bloomUpSource.empty()) {
        LOGGER.error("ScenePipeline: rhi/post/bloom_*.fs.glsl not found");
        return false;
    }

    bloomPrefilterShader_ =
        device_.createShader("bloom prefilter", kFullscreenVertex, bloomPrefilterSource.c_str());
    bloomDownShader_ =
        device_.createShader("bloom down", kFullscreenVertex, bloomDownSource.c_str());
    bloomUpShader_ =
        device_.createShader("bloom up", kFullscreenVertex, bloomUpSource.c_str());

    if (!bloomPrefilterShader_.valid() || !bloomDownShader_.valid() || !bloomUpShader_.valid())
        return false;

    PipelineDesc bloom;
    bloom.name             = "bloom prefilter";
    bloom.shader           = bloomPrefilterShader_;
    bloom.colourFormats[0] = TextureFormat::RGBA16F;
    bloom.colourCount      = 1;
    bloom.depth.test       = false;
    bloom.depth.write      = false;
    bloom.raster.cull      = CullMode::None;

    bloomPrefilterPipeline_ = device_.createPipeline(bloom);
    if (!bloomPrefilterPipeline_.valid()) return false;

    bloom.name   = "bloom down";
    bloom.shader = bloomDownShader_;
    bloomDownPipeline_ = device_.createPipeline(bloom);
    if (!bloomDownPipeline_.valid()) return false;

    /* ADDITIVE FROM HERE ON. The upsample outputs only its own contribution and
     * the blender sums it onto what the level already holds — see
     * bloom_up.fs.glsl on why the shader cannot do that itself. ONE / ONE on
     * both colour and alpha; the alpha of a bloom chain is unread, and leaving
     * it at the default would have it fighting the colour blend for no reason. */
    bloom.name              = "bloom up";
    bloom.shader            = bloomUpShader_;
    bloom.blend.enabled     = true;
    bloom.blend.sourceColour = BlendFactor::One;
    bloom.blend.destColour   = BlendFactor::One;
    bloom.blend.sourceAlpha  = BlendFactor::One;
    bloom.blend.destAlpha    = BlendFactor::One;

    bloomUpPipeline_ = device_.createPipeline(bloom);
    if (!bloomUpPipeline_.valid()) return false;

    /* The same shader and the same blend, into the scene target. A separate
     * pipeline because the format is baked in, and because naming it makes the
     * composite show up as its own line in a capture. */
    bloom.name = "bloom composite";
    bloomCompositePipeline_ = device_.createPipeline(bloom);
    if (!bloomCompositePipeline_.valid()) return false;

    BufferDesc bloomDesc;
    bloomDesc.name   = "bloom block";
    bloomDesc.bytes  = sizeof(BloomBlockData);
    bloomDesc.usage  = BufferUsageUniform;
    bloomDesc.access = BufferAccess::CpuToGpuPerFrame;
    bloomBlock_ = device_.createBuffer(bloomDesc);
    if (!bloomBlock_.valid()) return false;

    /* ONE SAMPLER PER SOURCE LEVEL, pinned to that level. See the member's note
     * — this is what makes reading level N while writing N±1 of the same
     * texture defined rather than merely working today. Linear, because every
     * bloom stage is a filter and a point tap would defeat the whole chain. */
    bloomLevelSamplers_.reserve(kBloomLevels);
    for (uint32_t level = 0; level < kBloomLevels; level++) {
        SamplerDesc levelSampler;
        levelSampler.minify  = FilterMode::Linear;
        levelSampler.magnify = FilterMode::Linear;
        levelSampler.mip     = FilterMode::Nearest;
        levelSampler.wrapU   = WrapMode::ClampToEdge;
        levelSampler.wrapV   = WrapMode::ClampToEdge;
        levelSampler.wrapW   = WrapMode::ClampToEdge;
        levelSampler.minLod  = static_cast<float>(level);
        levelSampler.maxLod  = static_cast<float>(level);

        const SamplerHandle handle = device_.createSampler(levelSampler);
        if (!handle.valid()) return false;
        bloomLevelSamplers_.push_back(handle);
    }

    /* ---- the resolve ----------------------------------------------------- */

    const std::string resolveSource = ShaderLibrary::preprocess("rhi/post/tonemap.fs.glsl");
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

    /* THE MATERIALS ARE NOT BROUGHT UP HERE ANY MORE. They belong to the device
     * rather than to a viewpoint, so RenderAssets::initialise builds them once
     * for every pipeline and every scene that will ever share them. Doing it
     * here would rebuild the whole table per quality preset — see
     * RenderAssets.hpp on the three lifetimes. */

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
    const std::string prefilterSource = ShaderLibrary::preprocess("rhi/scene/probe_prefilter.fs.glsl");
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
     * THE ARRAY IS THE SCENE'S AND IS CREATED THERE. A probe set describes a
     * world, so it is brought up with the world rather than with a viewpoint —
     * see RenderScene::initialise, which also carries the note about why a
     * device with no cubemap arrays is a flatter frame rather than a broken
     * one. What stays here are the two pipelines that DRAW into it, because a
     * pipeline object is pass state. */

    ready_ = true;

    /* A PLACEHOLDER SIZE. The first render() resizes to the real surface; this
     * only exists so the targets are never invalid between here and there. */
    if (!createSceneTargets(1280, 720)) return false;

    LOGGER.info("ScenePipeline: shadow {0}x{0}, prepass, ssao + blur, sky, lit and "
                "resolve ready ({1}x supersample)", kShadowSize, kSupersample);
    return true;
}

ScenePipeline::SunProjection ScenePipeline::sunProjection(const SceneFrame& frame,
                                                          const View& view,
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
    const bool hasCamera = view.projectionMatrix().at(3, 2) != 0.0f;

    const float tanHalfFovY = view.projectionMatrix().at(1, 1) != 0.0f
                            ? 1.0f / view.projectionMatrix().at(1, 1) : 1.0f;
    const float aspect = view.projectionMatrix().at(0, 0) != 0.0f
                       ? view.projectionMatrix().at(1, 1) / view.projectionMatrix().at(0, 0) : 1.0f;

    const Vec3 right{ view.viewMatrix().at(0, 0), view.viewMatrix().at(0, 1), view.viewMatrix().at(0, 2) };
    const Vec3 up{ view.viewMatrix().at(1, 0), view.viewMatrix().at(1, 1), view.viewMatrix().at(1, 2) };
    const Vec3 forward{ -view.viewMatrix().at(2, 0), -view.viewMatrix().at(2, 1), -view.viewMatrix().at(2, 2) };

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
        const Vec3  middle = view.position() + forward * distance;

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

void ScenePipeline::drawShadowMap(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("shadow map");

    const Aabb world = worldBoundsOf(view);
    const Vec3 minimum = world.min;
    const Vec3 maximum = world.max;

    /* An empty world, or a caller that handed over a zero sun. Either produces
     * a degenerate matrix; skipping is the honest response and leaves last
     * frame's depth rather than a NaN one. */
    const Vec3 extent = maximum - minimum;
    if (world.empty() || extent.length() < 1.0e-4f) return;
    if (frame.sunDirection.length() < 1.0e-4f) return;

    const SunProjection sun = sunProjection(frame, view, minimum, maximum);

    PassBlockData block;
    block.viewProjection = sun.viewProjection;
    device_.updateBuffer(passBlock_, &block, sizeof block, 0);

    /* ---- THE SUN'S OWN VIEW, DERIVED FROM THE CAMERA'S -------------------
     *
     * SAME SCENE, SAME VIEWER, NO CUTAWAY, AND THAT LAST PART IS THE WHOLE
     * REASON THE FILTER LIVES ON THE VIEW. What casts a shadow is a question
     * about the world, not about where the player is standing — and letting the
     * camera's storey cut reach here is what made the lighting change when the
     * player changed floor. Today that correctness is a property of
     * View::derived rather than of a caller remembering; the game cannot
     * express the wrong thing any more. See CutawayView.hpp for the episode.
     *
     * IT IS COLLECTED ONCE AND READ TWICE. The opaque half is this pass; the
     * translucent half is the transmission plane immediately after, which
     * wants exactly the surfaces that transmit rather than block. One
     * collection, two consumers, and no way for the two to disagree about which
     * geometry the sun sees. */
    const View sunView = view.derived(ViewKind::Sun, Mat4(), sun.viewProjection, view.position());

    if (RenderScene* scene = view.scene()) scene->collect(sunView, sunList_);
    else sunList_.clear();

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

    /* NO MATERIALS: this shader reads position and writes depth, so binding one
     * would be work for a stage that cannot see it.
     *
     * SWITCHED OFF, THE PASS STILL RUNS AND STILL CLEARS. The lit pass samples
     * this texture every frame whatever the switch says, so skipping the pass
     * would leave the last shadowed frame bound - the feature would freeze
     * rather than turn off. A map cleared to 1.0 is "nothing is in the way". */
    if (frame.shadows) drawItems(encoder, sunList_.opaque(), /*bindMaterials=*/false);


    device_.endPass(encoder);
}

void ScenePipeline::drawShadowTransmission(const SceneFrame& frame, const View& view)
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

    /* THE SUN'S TRANSLUCENT HALF, collected by the pass above — see the note
     * there. Materials ARE bound here: this shader tints by what the surface is
     * made of, which is the whole reason the plane is RGBA8 and not a single
     * fraction. */
    drawItems(encoder, sunList_.translucent(), /*bindMaterials=*/true);


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

void ScenePipeline::drawProbeCapture(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("probe capture");

    if (!frame.reflections) return;

    DeviceProbeSet* probeSet = probesOf(view);
    if (probeSet == nullptr) return;

    DeviceProbeSet& probes = *probeSet;
    if (!probes.valid() || probes.probeCount() == 0) return;

    /* THE SUN'S FIT, RECOMPUTED FROM THE SAME FUNCTION THE SHADOW PASS USED.
     * A capture shades against the shadow map this frame already wrote, so it
     * needs that map's matrix and its two world-unit scales — and reading them
     * off a second, differently-fitted call would offset every shadow inside
     * every reflection from the geometry casting it. */
    const Aabb world = worldBoundsOf(view);
    const SunProjection shadow = sunProjection(frame, view, world.min, world.max);

    const int faces = probes.stale() ? kProbeFacesWhileStale : kProbeFacesPerFrame;

    for (int i = 0; i < faces; i++) {
        DeviceProbeSet::Face face;
        if (!probes.nextFace(face)) break;

        /* ---- THIS FACE'S OWN VIEW ---------------------------------------
         *
         * DERIVED FROM THE CAMERA'S, so it carries the viewer and drops the
         * cutaway — the same rule the sun's view follows and with a sharper
         * consequence. A probe capturing under the iso level records the sky
         * and the street where its own ceiling and the floor above should be,
         * so an indoor room's reflections brighten every time the player
         * changes storey, and the cause is nowhere near the symptom because the
         * frame that changed is one nobody is looking at.
         *
         * AND IT IS A REAL FRUSTUM. A cube face is a 90-degree perspective, so
         * culling against it discards five sixths of the world per face rather
         * than submitting all of it six times — which is most of what a capture
         * used to cost. The old path could not do this at all: it handed the
         * whole world to every face because the engine did not own the list. */
        const View faceView = view.derived(ViewKind::ProbeFace, Mat4(),
                                           face.viewProjection, face.eye);

        if (RenderScene* scene = view.scene()) scene->collect(faceView, probeList_);
        else probeList_.clear();

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

        /* NO DECALS INSIDE A CAPTURE EITHER, for the reason above: the planes
         * describe the camera's screen and a cube face is not one. */
        block.decalParams[0] = 0.0f;

        device_.updateBuffer(litBlock_, &block, sizeof block, 0);

        PassDesc pass;
        pass.name = "probe face";

        pass.colours[0].texture = probes.texture();

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
        pass.depth.texture = probes.captureDepth();
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
        encoder.bindTexture(4, probes.emptyTexture(), probes.sampler());

        /* THE DBUFFER SLOTS, FILLED WITH THE 1x1 WHITE — and this is the same
         * story SSAO has one field up. The decal planes are SCREEN SPACE and a
         * 128-pixel cube face has no screen; worse, the plane being sampled is
         * the camera's, so a capture reading it would ink the whole face with
         * whichever decal happened to sit at the top left of the player's view.
         *
         * WHITE READS AS ALPHA 1, which readDecals treats as "nothing was ever
         * blended here" and early-outs on. So the stand-in is not merely safe,
         * it is the exact identity of the decode. The block's uDecalParams.x is
         * zeroed below as well; either alone would do and both cost nothing. */
        encoder.bindTexture(5, whitePixel_, pointSampler_);
        encoder.bindTexture(6, whitePixel_, pointSampler_);
        encoder.bindTexture(7, whitePixel_, pointSampler_);

        encoder.bindUniformBuffer(0, probes.block());
        encoder.bindUniformBuffer(1, litBlock_);
        encoder.bindUniformBuffer(2, materialBlock_);

        drawItems(encoder, probeList_.opaque(), /*bindMaterials=*/true);

        /* THEN WHAT YOU CAN SEE THROUGH, in the same pass — it reads the depth
         * and the colour the opaque half just wrote, exactly as the camera's
         * transparent pass reads the lit scene's. */
        encoder.bindPipeline(probeTransparentPipeline_);
        drawItems(encoder, probeList_.translucent(), /*bindMaterials=*/true);

        device_.endPass(encoder);

    }
}

void ScenePipeline::drawProbePrefilter(const View& view)
{
    CW_PROFILE_ZONE_N("probe prefilter");

    DeviceProbeSet* probeSet = probesOf(view);
    if (probeSet == nullptr) return;

    DeviceProbeSet& probes = *probeSet;
    if (!probes.valid() || !prefilterPipeline_.valid()) return;

    /* ONE PROBE PER FRAME AT MOST, and only one whose six faces are all current.
     * See the header: a lobe that reaches across faces cannot be run against a
     * probe that is half rebuilt. */
    const int probe = probes.probeReadyToPrefilter();
    if (probe < 0) return;

    for (int level = 1; level < DeviceProbeSet::kMipLevels; level++) {
        /* ROUGHNESS IS THE LEVEL'S POSITION IN THE CHAIN, which is the contract
         * the lit shader reads back as `roughness * (levels - 1)`. Level 0 is
         * the capture itself and is left alone — it is already the mirror. */
        const float roughness = static_cast<float>(level)
                              / static_cast<float>(DeviceProbeSet::kMipLevels - 1);

        /* THE SOURCE IS THE LEVEL ABOVE, bound through a sampler clamped to it
         * so this pass cannot read the level it is writing. */
        const rhi::SamplerHandle source = probes.levelSampler(level - 1);

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

            pass.colours[0].texture = probes.texture();

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
            encoder.bindTexture(0, probes.texture(), source);
            encoder.bindUniformBuffer(1, prefilterBlock_);
            encoder.drawFullscreen();
            device_.endPass(encoder);
        }
    }

    probes.markPrefiltered(probe);
}

/* ---- the custom depth / stencil pass -------------------------------------
 *
 * Depth-only geometry, tagged. Every renderable carrying a non-zero stencil
 * value is drawn from the CAMERA'S eye into a small colour target holding that
 * value, with its own depth attachment.
 *
 * IT PRODUCES NOTHING VISIBLE, and that is what it is for. This is the half
 * that is awkward to retrofit — somewhere for selected geometry to rasterise
 * apart from the scene, with a depth a later pass can compare against the
 * frame's. Every effect is then one full-screen shader that reads it.
 *
 * CLEARED TO ZERO ALPHA, which is what makes a stencil value of ZERO a real id
 * rather than "nothing here". A consumer tests coverage first; see the shader.
 */
void ScenePipeline::drawCustomDepth(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("custom depth");

    if (!customStencil_.valid() || !customStencilPipeline_.valid()) return;

    PassDesc pass;
    pass.name = "custom depth";
    pass.colours[0].texture = customStencil_;
    pass.colours[0].load    = LoadAction::Clear;
    pass.colours[0].store   = StoreAction::Store;
    pass.colours[0].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 0.0f };
    pass.colourCount = 1;

    pass.hasDepth      = true;
    pass.depth.texture = customDepth_;
    pass.depth.load    = LoadAction::Clear;
    pass.depth.clearTo = 1.0f;
    pass.depth.store   = StoreAction::Store;

    /* THE PASS RUNS AND CLEARS EVEN WITH NOTHING TAGGED. The outline samples
     * these two every frame whatever happens — a pipeline's bindings are the
     * same every frame — so skipping would leave the last frame's silhouette on
     * screen after the selection changed. The same freeze-rather-than-disable
     * trap the probes and the decals both record. */
    const bool draw = frame.customDepth && view.scene() != nullptr;

    if (draw) {
        /* THE CAMERA'S OWN EYE, DERIVED ONLY FOR THE KIND. Not a new viewpoint:
         * the buffer's entire value is that its depth is comparable with the
         * frame's, and a derived matrix would silently destroy that. */
        const View tagged = view.derived(ViewKind::CustomDepth, view.viewMatrix(),
                                         view.projectionMatrix(), view.position());

        view.scene()->collect(tagged, customList_);
    } else {
        customList_.clear();
    }

    PassBlockData block;
    block.viewProjection = view.viewProjection();
    device_.updateBuffer(customStencilBlock_, &block, sizeof block, 0);

    ICommandEncoder& encoder = device_.beginPass(pass);

    if (!customList_.opaque().empty()) {
        encoder.bindPipeline(customStencilPipeline_);
        encoder.bindUniformBuffer(1, customStencilBlock_);

        /* NO MATERIALS. The fragment stage writes an id and nothing else, so
         * binding one would be work for a stage that cannot see it — the same
         * reason the shadow map does not bind them. */
        drawItems(encoder, customList_.opaque(), /*bindMaterials=*/false);
    }

    device_.endPass(encoder);
}

/* ---- and the first thing that reads it -----------------------------------
 *
 * A silhouette over the RESOLVED image, in display colour. See the shader on
 * why that is the opposite of bloom's decision and why both are right.
 */
void ScenePipeline::drawOutline(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("outline");

    if (!outlinePipeline_.valid() || !customStencil_.valid()) return;

    /* NOTHING TAGGED IS NOTHING TO DRAW, and here — unlike the pass above —
     * skipping is correct rather than dangerous. This pass writes into the
     * view's target with a blend, so not running it leaves the resolved frame
     * exactly as the resolve left it. */
    if (!frame.customDepth || frame.outlineStencil <= 0) return;

    /* THE STENCIL'S SIZE, NOT THE TARGET'S. The shader fetches texels out of the
     * supersampled buffer while its fragments are output pixels, so it needs the
     * source's dimensions and the ratio; handing it targetWidth_ here is exactly
     * the confusion that made the two resolutions disagree. Thickness stays in
     * OUTPUT pixels — a designer's "two pixels wide" means on screen — and the
     * shader scales it by the ratio itself. */
    OutlineBlockData block;
    block.outline[0]  = static_cast<float>(frame.outlineStencil);
    block.outline[1]  = frame.outlineThickness;
    block.outline[2]  = static_cast<float>(targetWidth_ * outlineSupersample_);
    block.outline[3]  = static_cast<float>(targetHeight_ * outlineSupersample_);

    /* INTEGER BY CONSTRUCTION, because withOutlineSupersample floors the factor
     * at the scene's. A ratio below one would mean the stencil is coarser than
     * the depth it is compared against, and there would be no sound reduction
     * for the shader to do. */
    block.sampling[0] = static_cast<float>(outlineSupersample_);
    block.sampling[1] = static_cast<float>(outlineSupersample_ / kSupersample);
    block.sampling[2] = static_cast<float>(sceneWidth());
    block.sampling[3] = static_cast<float>(sceneHeight());

    for (int i = 0; i < 4; i++) {
        block.visible[i]  = frame.outlineVisible[i];
        block.occluded[i] = frame.outlineOccluded[i];
    }
    device_.updateBuffer(outlineBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "outline";
    pass.colours[0].texture = view.target();

    /* LOADED, NOT CLEARED. This is ink over a finished picture. */
    pass.colours[0].load  = LoadAction::Load;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(outlinePipeline_);

    if (view.hasViewport())
        encoder.setViewport(view.viewportX(), view.viewportY(),
                            view.viewportWidth(), view.viewportHeight());

    /* POINT SAMPLERS ON ALL THREE, and it is not a preference. The stencil
     * channel holds an ID: filtering two neighbouring ids produces a number
     * that is NEITHER, so a bilinear tap along the boundary between two
     * soldiers would report a third soldier that does not exist. The same
     * argument the occlusion pass makes about filtering two normals.
     *
     * THE SHADER NO LONGER DEPENDS ON THAT, and the samplers stay anyway. Every
     * tap in there is a texelFetch, which takes an integer texel and ignores
     * filtering entirely — so the invariant is now enforced by the shader rather
     * than by whoever binds it, which is where it belongs. Binding a linear
     * sampler here would stop being a silent correctness bug; leaving these
     * point keeps the intent legible and costs nothing. */
    encoder.bindTexture(0, customStencil_, pointSampler_);
    encoder.bindTexture(1, customDepth_, pointSampler_);
    encoder.bindTexture(2, sceneDepth_, pointSampler_);
    encoder.bindUniformBuffer(1, outlineBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

void ScenePipeline::drawPrepass(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("prepass");

    if (!sceneDepth_.valid() || !sceneNormals_.valid()) return;

    PassBlockData block;
    block.viewProjection = view.viewProjection();
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

    /* THE OPAQUE HALF OF THE CAMERA'S LIST, collected once at the top of the
     * frame and read by this pass, the lit pass and — its other half — the
     * transparent one. Glass is deliberately NOT here: the lit pass tests Equal
     * against what this wrote, so a pane's depth here means the wall behind it
     * is never shaded, and no amount of blending can put back geometry that was
     * never drawn. That used to be a rule a submitter had to know; now it falls
     * out of the material's blend mode putting the pane in the other bucket. */
    drawItems(encoder, cameraList_.opaque(), /*bindMaterials=*/false);


    device_.endPass(encoder);
}

/* ---- the decals, into the DBuffer ----------------------------------------
 *
 * One draw per decal, and the four pieces of state are all load-bearing:
 *
 *   BACK FACES ONLY. Front faces vanish the moment the camera enters the box,
 *   which for a decal the size of a road tile is most of the time. Back faces
 *   are always present and always cover the box's full screen extent.
 *
 *   NO DEPTH TEST, NO DEPTH WRITE. The box is a bounding volume, not geometry;
 *   which fragments survive is decided in the shader against the box's own
 *   bounds. Leaving the test on would reject exactly the fragments whose
 *   receiving surface sits in front of the box's far side — which is all of
 *   them.
 *
 *   SEPARATE BLEND FACTORS. rgb over-blends premultiplied while alpha
 *   MULTIPLIES, so the planes accumulate the decals' colour in rgb and the base
 *   material's surviving fraction in alpha. One equation, any number of
 *   overlapping decals, and the lit shader's decode is a single fused
 *   multiply-add.
 *
 *   CLEARED TO (0, 0, 0, 1) — no ink, base fully intact. That is the identity
 *   of the blend above, which is why "no decals this frame" and "the pass did
 *   not run" have to be the same picture and are.
 *
 * IT STILL RUNS WITH NOTHING TO DRAW, and that is not waste. The planes are
 * bound by the lit pipeline every frame whatever happens, so skipping the clear
 * would leave LAST frame's ink on screen after the last decal was removed —
 * the freeze-rather-than-disable trap RenderEffects.hpp records. A clear of
 * three half-resolution targets is the cost of that switch meaning what it
 * says.
 */
/* ONE BUFFER FOR EVERY DECAL'S BLOCK, grown with headroom and never shrunk.
 *
 * The same arrangement the debug line buffer has: a board that gains a decal
 * should not recreate its buffer every frame on the way there, and the steady
 * state allocates nothing. */
bool ScenePipeline::ensureDecalCapacity(uint32_t decals)
{
    if (decals <= decalObjectCapacity_ && decalObjectBlocks_.valid()) return true;

    const uint32_t wanted = std::max(decals + decals / 4u, 32u);

    if (decalObjectBlocks_.valid()) device_.destroy(decalObjectBlocks_);
    decalObjectBlocks_ = {};
    decalObjectCapacity_ = 0;

    BufferDesc desc;
    desc.name   = "decal object blocks";
    desc.bytes  = static_cast<uint64_t>(wanted) * kDecalBlockStride;
    desc.usage  = BufferUsageUniform;
    desc.access = BufferAccess::CpuToGpuPerFrame;

    decalObjectBlocks_ = device_.createBuffer(desc);
    if (!decalObjectBlocks_.valid()) return false;

    decalObjectCapacity_ = wanted;
    return true;
}

int ScenePipeline::decalCaptureSlot(int id) const
{
    if (id < 0) return -1;

    for (std::size_t slot = 0; slot < decalCaptureIds_.size(); slot++)
        if (decalCaptureIds_[slot] == id) return static_cast<int>(slot);

    return -1;
}

/* ---- what each decal can see, rendered from where it was thrown ------------
 *
 * THE PASS THAT TELLS A STAIR RISER FROM THE BACK OF A WALL. Six faces of a
 * cube of distances per decal, and the decal pass then inks a surface only if
 * it is no further away than what this saw in that direction.
 *
 * IT RUNS WHEN A DECAL IS NEW AND NOT AGAIN. A mark that has settled on a floor
 * is looking at geometry that is not moving, so the work is per PLACEMENT, not
 * per frame — which is what makes an exact answer affordable at all. The dev
 * tool's preview has no id and is therefore re-captured every frame, which is
 * correct rather than wasteful: it moves with the cursor, so what it can see
 * changes with it.
 *
 * WHAT IT DOES NOT YET HANDLE, stated because it will be noticed as a bug
 * otherwise: geometry that changes UNDER a settled decal. Demolish the wall
 * that was occluding a mark and the mark keeps the old answer until something
 * evicts its slot. The fix is a scene geometry version the capture compares
 * against, and it belongs with whatever ends up owning destruction — not here,
 * guessed at.
 */
void ScenePipeline::drawDecalVisibility(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("decal visibility");

    if (!decalVisibility_.valid() || !decalCapturePipeline_.valid()) return;
    if (!frame.decals) return;

    const RenderScene* scene = view.scene();
    if (scene == nullptr) return;

    const std::vector<DeviceDecalSet::Projector>& projectors = scene->decals().projectors();

    decalCaptureFrameSlots_.assign(projectors.size(), -1);
    if (projectors.empty()) return;

    for (std::size_t index = 0; index < projectors.size(); index++) {
        const DeviceDecalSet::Projector& projector = projectors[index];

        /* ALREADY CAPTURED AND STILL VALID — the common case, and it costs one
         * scan of a few dozen ints. */
        const int cached = decalCaptureSlot(projector.id);
        if (cached >= 0) {
            decalCaptureFrameSlots_[index] = cached;
            continue;
        }

        /* WHICH SLOT IT GOES IN. An empty one first; failing that the cursor
         * evicts round-robin. A decal that loses its slot is not broken, it
         * just stops being able to tell what is solid — see the shader's
         * uWrap.y branch. */
        int slot = -1;
        for (std::size_t i = 0; i < decalCaptureIds_.size(); i++) {
            if (decalCaptureIds_[i] < 0) { slot = static_cast<int>(i); break; }
        }

        if (slot < 0) {
            slot = decalCaptureCursor_ % kDecalCaptureSlots;
            decalCaptureCursor_ = (decalCaptureCursor_ + 1) % kDecalCaptureSlots;
        }

        /* ---- where this decal is looking from ---------------------------
         *
         * The box's centre, LIFTED off the surface it was placed on. The third
         * column of the transform is the decal's normal scaled to the box's
         * depth, so normalising it gives the way out of that surface. */
        const Mat4& model = projector.transform;
        const Vec3 centre{ model.m[12], model.m[13], model.m[14] };

        Vec3 normal{ model.m[8], model.m[9], model.m[10] };
        const float length = normal.length();
        if (length > 1.0e-6f) normal = normal * (1.0f / length);
        else                  normal = Vec3{ 0.0f, 1.0f, 0.0f };

        const Vec3 origin = centre + normal * kDecalCaptureLift;

        /* HOW FAR THE CAPTURE HAS TO REACH: the box's own corner, and nothing
         * beyond it. Every surface the decal could possibly ink is inside the
         * box, so anything further away cannot occlude anything that matters —
         * and a tight far plane is what keeps the depth test's precision on the
         * few metres being resolved. */
        const Vec3 axisU{ model.m[0], model.m[1], model.m[2] };
        const Vec3 axisV{ model.m[4], model.m[5], model.m[6] };
        const Vec3 axisW{ model.m[8], model.m[9], model.m[10] };
        const float reach = 0.5f * (axisU.length() + axisV.length() + axisW.length())
                          + kDecalCaptureLift;

        for (int face = 0; face < 6; face++) {
            DecalCaptureBlockData block;
            block.viewProjection = DeviceProbeSet::faceViewProjection(face, origin, reach, kDecalCaptureNear);
            block.origin[0] = origin.x;
            block.origin[1] = origin.y;
            block.origin[2] = origin.z;
            block.origin[3] = reach;
            device_.updateBuffer(decalCaptureBlock_, &block, sizeof block, 0);

            /* A REAL FRUSTUM PER FACE, so the collect discards five sixths of
             * the world rather than submitting all of it six times — the same
             * reason the probe capture derives a view per face. */
            const View faceView = view.derived(ViewKind::ProbeFace, Mat4(),
                                               block.viewProjection, origin);

            if (RenderScene* mutableScene = view.scene())
                mutableScene->collect(faceView, decalCaptureList_);
            else
                decalCaptureList_.clear();

            PassDesc pass;
            pass.name = "decal visibility";
            pass.colours[0].texture = decalVisibility_;
            pass.colours[0].layer   = static_cast<uint32_t>(slot * 6 + face);
            pass.colours[0].load    = LoadAction::Clear;
            pass.colours[0].store   = StoreAction::Store;

            /* NOTHING IN THE WAY. See kDecalCaptureEmpty. */
            pass.colours[0].clearTo = ClearColour{ kDecalCaptureEmpty, 0.0f, 0.0f, 0.0f };
            pass.colourCount = 1;

            pass.hasDepth      = true;
            pass.depth.texture = decalCaptureDepth_;
            pass.depth.load    = LoadAction::Clear;
            pass.depth.clearTo = 1.0f;
            pass.depth.store   = StoreAction::Discard;

            ICommandEncoder& encoder = device_.beginPass(pass);
            encoder.bindPipeline(decalCapturePipeline_);
            encoder.bindUniformBuffer(1, decalCaptureBlock_);

            /* NO MATERIALS. This shader reads a position and writes a distance;
             * what a surface looks like cannot change whether it is in the way.
             * TRANSPARENT GEOMETRY IS INCLUDED, and that is the honest answer
             * for now: a decal should not ink the wall behind a window, and a
             * pane that stops the capture says so. If glass ever needs to be
             * see-through here it is a filter on the collect, not a fudge. */
            drawItems(encoder, decalCaptureList_.opaque(), /*bindMaterials=*/false);

            device_.endPass(encoder);
        }

        /* A TRANSIENT DECAL LEAVES ITS SLOT MARKED FREE. The preview is a new
         * projector every frame with no id, so parking it would evict a real
         * decal's capture every frame and the board would flicker between
         * having tests and not having them. Marked free, it takes the same
         * first-free slot again next frame and costs nothing else. Its capture
         * is valid for THIS frame, which is all it has to be — the geometry it
         * saw is the geometry the draw below is about to test against. */
        decalCaptureIds_[static_cast<std::size_t>(slot)] =
            projector.id >= 0 ? projector.id : -1;
        decalCaptureOrigins_[static_cast<std::size_t>(slot)] = origin;
        decalCaptureFrameSlots_[index] = slot;
    }
}

void ScenePipeline::drawDecals(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("decals");

    if (!decalAlbedo_.valid() || !decalPipeline_.valid()) return;

    /* THE PASS DESCRIPTOR IS BUILT WHETHER OR NOT ANYTHING IS DRAWN, because
     * the clear is the point when the list is empty. */
    PassDesc pass;
    pass.name = "decals";
    pass.colours[0].texture = decalAlbedo_;
    pass.colours[1].texture = decalNormal_;
    pass.colours[2].texture = decalSurface_;
    pass.colourCount = 3;

    for (int i = 0; i < pass.colourCount; i++) {
        pass.colours[i].load    = LoadAction::Clear;
        pass.colours[i].store   = StoreAction::Store;
        pass.colours[i].clearTo = ClearColour{ 0.0f, 0.0f, 0.0f, 1.0f };
    }

    /* ---- what the whole pass shares ---------------------------------------
     *
     * The inverse view-projection is built here from the view's own matrix, so
     * the unprojection provably matches the prepass this frame drew — the
     * failure it prevents is every decal landing on a surface that is not
     * there, which reads as decals floating rather than as a stale matrix. */
    DecalPassBlockData passBlock;
    passBlock.viewProjection = view.viewProjection();
    passBlock.inverseViewProjection = view.viewProjection().inverse();
    passBlock.resolution[0] = static_cast<float>(targetWidth_);
    passBlock.resolution[1] = static_cast<float>(targetHeight_);
    device_.updateBuffer(decalPassBlock_, &passBlock, sizeof passBlock, 0);

    const RenderScene* scene = view.scene();
    const DeviceDecalSet* decals = scene != nullptr ? &scene->decals() : nullptr;

    /* SWITCHED OFF IS AN EMPTY LIST, NOT A SKIPPED PASS. See the note above:
     * the clear still has to happen or "off" freezes rather than removes. */
    const bool draw = frame.decals && decals != nullptr && !decals->empty();

    if (!draw) {
        ICommandEncoder& empty = device_.beginPass(pass);
        (void)empty;
        device_.endPass(empty);
        return;
    }

    /* ---- every decal's object block, in one upload ------------------------
     *
     * PADDED TO kDecalBlockStride EACH, because a uniform buffer BINDING OFFSET
     * must be a multiple of the device's alignment — 256 on most desktop GL
     * drivers. Packing them tightly and binding at sizeof() would be rejected
     * or, worse, silently rounded down on a driver that does not check, which
     * hands every decal the previous one's transform. */
    const std::vector<DeviceDecalSet::Projector>& projectors = decals->projectors();
    const std::size_t count = projectors.size();

    if (!ensureDecalCapacity(static_cast<uint32_t>(count))) return;

    decalScratch_.assign(count * kDecalBlockStride, 0);

    for (std::size_t i = 0; i < count; i++) {
        const DeviceDecalSet::Projector& projector = projectors[i];

        DecalObjectBlockData block;
        block.model = projector.transform;

        /* THE INVERSE, ON THE CPU AND ONCE PER DECAL. GLSL has inverse(mat4)
         * and calling it per fragment for a value constant across the draw is
         * work in the wrong place — the raylib path builds it here too. */
        block.inverseModel = projector.transform.inverse();

        for (int c = 0; c < 4; c++) block.tint[c] = projector.tint[c];

        block.factors[0] = projector.roughness;
        block.factors[1] = projector.metalness;
        block.factors[2] = projector.normalStrength;
        block.factors[3] = projector.opacity;

        block.fade[0] = projector.angleFadeStart;
        block.fade[1] = projector.angleFadeEnd;
        block.fade[2] = projector.depthFade;
        block.fade[3] = projector.emissive;

        block.wrap[0] = projector.wrap ? 1.0f : 0.0f;

        /* WHAT THIS DECAL CAN SEE, if it was given a slice. wrap[1] is the
         * switch the shader reads: without a capture it inks everything inside
         * its box, which is the pass's behaviour before this existed. */
        const int slot = i < decalCaptureFrameSlots_.size()
                       ? decalCaptureFrameSlots_[i] : -1;

        if (slot >= 0) {
            const Vec3& origin = decalCaptureOrigins_[static_cast<std::size_t>(slot)];
            /* THE CAPTURE IS BUILT, UPLOADED AND NOT YET TRUSTED. Setting this
             * to 1 switches the shader from the plane-test fallback to the
             * visibility test; it is 0 while the capture is being diagnosed,
             * because on corners it is currently WORSE than the approximation
             * it replaces and a half-working test is not worth a broken wrap.
             * The pass above still runs, so what it produces can be looked at. */
            /* OFF, AND THE SHADER FALLS BACK TO THE PLANE TEST. The capture is
             * built, uploaded and correct in the cases that were measured, but
             * it still leaks on corners and four rounds of threshold tuning did
             * not converge — so what ships is the approximation that is KNOWN
             * good there. One line moves it back once the capture has been read
             * out and understood rather than reasoned about. */
            block.wrap[1]    = 1.0f;
            block.wrap[2]    = kDecalCaptureTexelArc;
            block.capture[0] = origin.x;
            block.capture[1] = origin.y;
            block.capture[2] = origin.z;
            block.capture[3] = static_cast<float>(slot);
        }

        std::memcpy(decalScratch_.data() + i * kDecalBlockStride, &block, sizeof block);
    }

    device_.updateBuffer(decalObjectBlocks_, decalScratch_.data(), decalScratch_.size(), 0);

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(decalPipeline_);

    /* The prepass's two attachments, which every decal unprojects and reads. */
    encoder.bindTexture(0, sceneDepth_, pointSampler_);
    encoder.bindTexture(1, sceneNormals_, pointSampler_);

    /* AND WHAT EVERY DECAL COULD SEE FROM WHERE IT WAS THROWN. Bound once for
     * the pass rather than per draw: it is one array and each decal indexes its
     * own cube out of it, which is the whole reason it is an array. */
    encoder.bindTexture(5, decalVisibility_, decalCaptureSampler_);

    encoder.bindUniformBuffer(1, decalPassBlock_);

    for (std::size_t i = 0; i < count; i++) {
        const DeviceDecalSet::Projector& projector = projectors[i];
        const DeviceDecalSet::Material& material = decals->material(projector.material);

        /* A DECAL WHOSE ALBEDO NEVER LOADED IS SKIPPED, NOT SUBSTITUTED. Its
         * ALPHA is the decal's shape, so a white stand-in would ink the whole
         * projector box as a solid rectangle over the world — which is a far
         * worse answer than nothing at all. The other two maps fall back. */
        if (!material.albedo.valid()) continue;

        encoder.bindTexture(2, material.albedo, linearSampler_);
        encoder.bindTexture(3, material.packed.valid() ? material.packed
                                                       : assets_.white(), linearSampler_);
        encoder.bindTexture(4, material.normal.valid() ? material.normal
                                                       : assets_.flatNormal(), linearSampler_);

        encoder.bindUniformBuffer(3, decalObjectBlocks_, i * kDecalBlockStride,
                                  sizeof(DecalObjectBlockData));
        encoder.draw(decalCube_);
    }

    device_.endPass(encoder);
}

void ScenePipeline::drawOcclusion(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("ssao");

    if (!occlusion_.valid() || !sceneDepth_.valid() || !sceneNormals_.valid()) return;

    /* SWITCHED OFF, THE PLANE IS WRITTEN WHITE RATHER THAN LEFT ALONE. The lit
     * pass samples occlusionBlurred_ unconditionally - a pipeline's bindings
     * are the same every frame - so skipping both halves would leave the last
     * occluded frame bound and the switch would freeze the effect instead of
     * turning it off. White is "nothing is occluded", which is the same
     * stand-in whitePixel_ provides inside a probe capture and for the same
     * reason. One clear, no geometry, no blur. */
    if (!frame.ambientOcclusion) {
        PassDesc white;
        white.name = "ssao off";
        white.colours[0].texture = occlusionBlurred_;
        white.colours[0].load    = LoadAction::Clear;
        white.colours[0].store   = StoreAction::Store;
        white.colours[0].clearTo = ClearColour{ 1.0f, 1.0f, 1.0f, 1.0f };
        white.colourCount = 1;

        ICommandEncoder& blank = device_.beginPass(white);
        device_.endPass(blank);
        return;
    }

    OcclusionBlockData block;
    block.projection        = view.projectionMatrix();
    block.inverseProjection = view.projectionMatrix().inverse();
    block.view              = view.viewMatrix();

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
    block.resolutionAndRadius[2] = frame.occlusionRadius;
    block.resolutionAndRadius[3] = frame.occlusionBias;
    block.strength[0] = frame.occlusionStrength;

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

void ScenePipeline::drawOcclusionBlur(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("ssao blur");

    /* NOTHING TO FINISH when the pass above wrote white — see it for why that
     * is a clear rather than a skip. */
    if (!frame.ambientOcclusion) return;

    if (!occlusion_.valid() || !occlusionBlurred_.valid() || !sceneDepth_.valid()) return;

    BlurBlockData block;
    block.inverseProjection = view.projectionMatrix().inverse();
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

void ScenePipeline::drawSky(const SceneFrame& frame, const View& view)
{
    /* SKIPPED OUTRIGHT, unlike the two above, and the difference is that
     * nothing SAMPLES the sky — it is simply the first colour into the scene
     * target. What replaces it is the lit pass clearing rather than loading;
     * see drawLitScene. */
    if (!frame.sky) return;

    CW_PROFILE_ZONE_N("sky");

    if (!sceneColour_.valid()) return;

    SkyBlockData block;

    /* THE FULL INVERSE, not inverseRigid. A view-projection is not a rigid
     * transform — the projection is the whole point of it — and the fast
     * inverse would return a matrix that looks plausible and unprojects every
     * pixel to the wrong ray. */
    block.inverseViewProjection = view.viewProjection().inverse();

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

void ScenePipeline::drawLitScene(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("lit scene");

    if (!sceneColour_.valid() || !sceneDepth_.valid()) return;

    const Aabb world = worldBoundsOf(view);

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
    const SunProjection shadow = sunProjection(frame, view, world.min, world.max);

    /* THE SAME BUILDER THE PROBE CAPTURE USES, so a reflection is lit by this
     * frame's sun rather than by a second one that drifted. See buildLitBlock. */
    LitBlockData block = buildLitBlock(frame, shadow.viewProjection,
                                       shadow.worldTexelSize, shadow.depthRange);

    block.viewProjection = view.viewProjection();
    writeVec3(block.cameraPosition, view.position());

    /* THE SCENE'S PROBES. render() has already established that a view names a
     * scene, so this is a reference rather than a check. */
    DeviceProbeSet& probes = view.scene()->probes();

    /* HOW MANY PROBES THE SHADER SHOULD CONSIDER — zero when the dev panel's
     * reflections switch is off, which turns the ambient specular back into the
     * analytic sky with no second code path to keep in step. */
    block.probeParams[0] = frame.reflections
                         ? static_cast<float>(probes.probeCount()) : 0.0f;

    device_.updateBuffer(litBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "lit scene";

    pass.colours[0].texture = sceneColour_;

    /* LOADED, NOT CLEARED — the sky pass has already filled this target and
     * clearing here would paint over it. This was a Clear to a dim blue-grey
     * while there was no sky, which is exactly the kind of placeholder that
     * survives the thing it stood in for.
     *
     * UNLESS THE SKY IS SWITCHED OFF, in which case nothing has filled it and
     * loading would show the PREVIOUS frame behind this one's geometry — which
     * reads as a smear rather than as a missing sky. */
    pass.colours[0].load  = frame.sky ? LoadAction::Load : LoadAction::Clear;
    pass.colours[0].clearTo = ClearColour{ frame.clearColour[0], frame.clearColour[1],
                                           frame.clearColour[2], frame.clearColour[3] };
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
    encoder.bindTexture(4, probes.valid() ? probes.texture() : probes.emptyTexture(),
                        probes.sampler());

    /* THE DBUFFER, AT SLOTS 5, 6 AND 7, AND BOUND UNCONDITIONALLY for the same
     * reason the probes are one line up: a pipeline's bindings are the same
     * every frame, and branching on `frame.decals` here would leave three slots
     * holding whatever a previous pass put there on exactly the frames nothing
     * checks them. The switch that turns decals off is uDecalParams.x in the
     * block, not the absence of a binding.
     *
     * LINEAR, NOT POINT. The planes are half this pass's resolution, so these
     * are magnifying reads and a point sampler would give every decal a hard
     * two-pixel staircase along its edge — the one place the upsample is
     * visible. Filtering premultiplied data is correct, which is why the planes
     * are stored that way. */
    encoder.bindTexture(5, decalAlbedo_, linearSampler_);
    encoder.bindTexture(6, decalNormal_, linearSampler_);
    encoder.bindTexture(7, decalSurface_, linearSampler_);

    encoder.bindUniformBuffer(0, probes.block());

    encoder.bindUniformBuffer(1, litBlock_);

    /* THE SAME BLOCK THE PREPASS FILLED, at the same binding. One material for
     * the whole world while there is no material library — see the note in
     * drawPrepass, which writes it. Bound here rather than re-uploaded because
     * nothing between the two passes touches it. */
    encoder.bindUniformBuffer(2, materialBlock_);

    drawItems(encoder, cameraList_.opaque(), /*bindMaterials=*/true);

    device_.endPass(encoder);
}

/* ---- one stage of the chain ----------------------------------------------
 *
 * Every stage is the same five things — set the block, open a pass on one mip
 * of one target, bind one source with a level-pinned sampler, draw a covering
 * triangle. Written once because the five differ only in their arguments, and
 * because the one that is easy to get wrong is the SIZE pair: a stage told its
 * source's size instead of its target's samples the right texture at the wrong
 * scale, which comes out as a bloom that is offset rather than absent.
 */
void ScenePipeline::bloomStage(PipelineHandle pipeline, TextureHandle source,
                               SamplerHandle sampler,
                               uint32_t sourceWidth, uint32_t sourceHeight,
                               TextureHandle target, uint32_t targetLevel,
                               uint32_t targetWidth, uint32_t targetHeight,
                               const SceneFrame& frame, float intensity, bool additive)
{
    BloomBlockData block;
    block.params[0] = frame.bloomThreshold;
    block.params[1] = frame.bloomKnee;
    block.params[2] = intensity;
    block.params[3] = frame.bloomRadius;

    block.sourceTexel[0] = 1.0f / static_cast<float>(std::max(sourceWidth, 1u));
    block.sourceTexel[1] = 1.0f / static_cast<float>(std::max(sourceHeight, 1u));
    block.sourceTexel[2] = static_cast<float>(sourceWidth);
    block.sourceTexel[3] = static_cast<float>(sourceHeight);

    block.targetSize[0] = static_cast<float>(targetWidth);
    block.targetSize[1] = static_cast<float>(targetHeight);

    device_.updateBuffer(bloomBlock_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "bloom";
    pass.colours[0].texture = target;
    pass.colours[0].mip     = targetLevel;

    /* LOAD WHEN ADDITIVE, DontCare WHEN REPLACING. An additive stage sums onto
     * what is already there, so discarding it first would throw away the very
     * thing it is adding to — and on a tiler that is not a slow path, it is a
     * different picture. The downsamples write every pixel and want DontCare. */
    pass.colours[0].load  = additive ? LoadAction::Load : LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(pipeline);
    encoder.bindTexture(0, source, sampler);
    encoder.bindUniformBuffer(1, bloomBlock_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

/* ---- the whole chain -----------------------------------------------------
 *
 * Prefilter down into level 0, halve to the bottom, then climb back adding, and
 * finally add level 0 into the scene. Between the debug lines and the resolve,
 * so it reads linear radiance and writes linear radiance.
 *
 * WHY THE UPSAMPLE STOPS AT LEVEL 0 AND THE COMPOSITE IS SEPARATE. The chain's
 * levels are all half-resolution or smaller and share a format; the scene
 * target is full size and is the thing being composited INTO rather than a
 * further level of the same signal. Folding the composite into the loop would
 * mean the loop's last iteration had a different source scale, a different
 * intensity and a different target format, which is three special cases inside
 * something whose whole value is that every iteration is identical.
 */
void ScenePipeline::drawBloom(const SceneFrame& frame)
{
    CW_PROFILE_ZONE_N("bloom");
    CW_GPU_ZONE("bloom");

    if (!bloomChain_.valid() || !sceneColour_.valid()) return;

    /* NOTHING AT ALL WHEN THE INTENSITY IS ZERO, as opposed to a chain that
     * runs and adds nothing. Six passes writing a result multiplied by zero is
     * the definition of work nobody can see, and the switch above it in
     * ViewLayers already means "do not spend this". */
    if (frame.bloomIntensity <= 0.0f) return;

    const uint32_t chainWidth  = std::max(sceneWidth() / kBloomDownscale, 1u);
    const uint32_t chainHeight = std::max(sceneHeight() / kBloomDownscale, 1u);

    /* HOW MANY LEVELS THIS WINDOW CAN ACTUALLY HOLD. Six is the budget, not a
     * promise: a small window runs out of pixels first, and a level of zero
     * width is a pass with no fragments whose upsample partner then reads an
     * undefined level. Clamped here rather than at creation, because the
     * texture is allocated once for the largest case and the LOOP is what has
     * to agree with the window in front of it. */
    uint32_t levels = 1;
    while (levels < kBloomLevels
           && (chainWidth >> levels) >= 2u && (chainHeight >> levels) >= 2u)
        levels++;

    /* ---- 1. the scene's bright half, into level 0 ---------------------- */
    bloomStage(bloomPrefilterPipeline_, sceneColour_, linearSampler_,
               sceneWidth(), sceneHeight(),
               bloomChain_, 0, chainWidth, chainHeight,
               frame, 1.0f, /*additive=*/false);

    /* ---- 2. down the chain -------------------------------------------- */
    for (uint32_t level = 1; level < levels; level++) {
        bloomStage(bloomDownPipeline_, bloomChain_, bloomLevelSamplers_[level - 1],
                   chainWidth >> (level - 1), chainHeight >> (level - 1),
                   bloomChain_, level, chainWidth >> level, chainHeight >> level,
                   frame, 1.0f, /*additive=*/false);
    }

    /* ---- 3. and back up, ADDING ---------------------------------------
     *
     * Downwards from the second-deepest level, each stage reading the level
     * below it and accumulating onto its own. The sum over levels is what gives
     * a bloom a bright core and a long tail rather than one uniform smear. */
    for (uint32_t level = levels - 1; level > 0; level--) {
        bloomStage(bloomUpPipeline_, bloomChain_, bloomLevelSamplers_[level],
                   chainWidth >> level, chainHeight >> level,
                   bloomChain_, level - 1,
                   chainWidth >> (level - 1), chainHeight >> (level - 1),
                   frame, 1.0f, /*additive=*/true);
    }

    /* ---- 4. into the scene, once, at the authored intensity ------------ */
    bloomStage(bloomCompositePipeline_, bloomChain_, bloomLevelSamplers_[0],
               chainWidth, chainHeight,
               sceneColour_, 0, sceneWidth(), sceneHeight(),
               frame, frame.bloomIntensity, /*additive=*/true);
}

void ScenePipeline::drawResolve(const SceneFrame& frame, const View& view)
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

    /* THE FILMIC CURVE, OR THE RAW RADIANCE. This was hardcoded on, which made
     * ViewLayers' `toneMap` the last of the eight feature switches this
     * pipeline ignored. Off is not "no resolve": the exposure still applies and
     * the supersample still collapses, and what goes is the curve — see
     * ViewLayers::features and the branch in tonemap.fs.glsl, which was already
     * written for it. */
    block.exposureAndFlags[1] = frame.toneMap ? 1.0f : 0.0f;
    block.exposureAndFlags[2] = static_cast<float>(frame.debugView);

    block.outputTexel[0] = targetWidth_ > 0 ? 1.0f / static_cast<float>(targetWidth_) : 1.0f;
    block.outputTexel[1] = targetHeight_ > 0 ? 1.0f / static_cast<float>(targetHeight_) : 1.0f;
    device_.updateBuffer(resolveBlock_, &block, sizeof block, 0);

    /* WHERE THE FINISHED PICTURE LANDS, and it is the VIEW's rather than the
     * pipeline's — see View.hpp. An attachment carrying no texture is the
     * backbuffer, which is what a view with no target means. A minimap, a
     * security camera, a portal or a split-screen pane is this field and
     * nothing else.
     *
     * WHAT IS NOT THE VIEW'S is everything before this line: scene colour,
     * depth, the normal plane, the occlusion plane, the shadow map. Those are
     * the pipeline's, their formats and sizes are what a quality preset moves,
     * and §4.11 depends on nobody outside having pinned one.
     *
     * Past this line everything is display colour. */
    PassDesc pass;
    pass.name = "resolve";
    pass.colours[0].texture = view.target();
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(resolvePipeline_);

    /* AND WHICH RECTANGLE OF IT. Four panes into one target differ only here —
     * which is what keeps §4.12's last open question open: whether N players
     * means N pipelines or one reused N times is a question about the TARGET,
     * and neither answer needs this type or this pass to change. */
    if (view.hasViewport())
        encoder.setViewport(view.viewportX(), view.viewportY(),
                            view.viewportWidth(), view.viewportHeight());

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

void ScenePipeline::render(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("scene pipeline");

    if (!ready_) return;

    /* A VIEW NAMES ITS SCENE. That is the contract, and it is checked once
     * rather than defended at every pass: without one there is no world to
     * cull, no probe set to sample and nothing for the bindings that must be
     * the same every frame to point at. */
    RenderScene* scene = view.scene();
    if (scene == nullptr) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGGER.error("ScenePipeline: the view names no scene - nothing will be drawn. "
                         "Call View::withScene before rendering");
        }
        return;
    }

    /* WHAT THE GAME REGISTERED THAT IS RUNNING AT ALL, said once. See
     * IScenePass.hpp: a hatch used for something ordinary means the engine is
     * missing a feature, and that signal is worthless if nobody can see it. */
    if (!hatchReported_) {
        hatchReported_ = true;
        if (!hatchPasses_.empty()) {
            for (const HatchPass& entry : hatchPasses_)
                LOGGER.info("game: 1 custom pass '{}' at insertion point {}",
                            entry.pass != nullptr ? entry.pass->name() : "(null)",
                            static_cast<int>(entry.point));
        }
    }

    /* ---- ONE COLLECTION FOR THE CAMERA, READ BY THREE PASSES -------------
     *
     * The prepass, the lit pass and the transparent pass all draw parts of this
     * one list. Collecting per pass would cull the same world three times to
     * get three answers that are identical by construction, and would give the
     * three passes three chances to disagree about what exists — which is the
     * shape of the bug where glass appears in the depth prepass and the wall
     * behind it is never shaded.
     *
     * THE SUN'S AND EACH PROBE FACE'S ARE COLLECTED IN THEIR OWN PASSES,
     * because their matrices do not exist until those passes have fitted them. */
    scene->collect(view, cameraList_);

    /* THE SUN FIRST, because the lit pass samples what it writes. Then the
     * camera's own depth and normals, which the occlusion and decal passes are
     * both unprojected from. The order is the pipeline's and not the caller's,
     * which is the point of the caller not being able to express one. */
    drawShadowMap(frame, view);

    /* AND WHAT GOT THROUGH IT. Immediately after, because it depth-tests
     * against the map the pass above just wrote. */
    drawShadowTransmission(frame, view);

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
    drawProbeCapture(frame, view);

    /* AND THE CHAIN OVER WHAT IT WROTE. Immediately after, because it reads the
     * faces that pass just captured — and it runs at most once per frame, on a
     * probe whose six faces are all current, so most frames it returns without
     * opening a pass at all. */
    drawProbePrefilter(view);

    drawPrepass(frame, view);

    /* THE DECALS, BETWEEN THE PREPASS THAT FEEDS THEM AND THE LIT PASS THAT
     * READS THEM, and there is no other place they can go. They unproject the
     * prepass's depth to find the surface under each pixel, and they write a
     * material override the lit pass blends over its own inputs — so both
     * neighbours are hard constraints rather than an ordering preference. */
    /* THE TAGGED SUBSET, from the camera's own eye. After the prepass so both
     * depth buffers describe the same instant, and before anything reads
     * either. */
    drawCustomDepth(frame, view);

    /* WHAT EACH DECAL CAN SEE, BEFORE THE PASS THAT READS IT. Only decals
     * placed since the last frame cost anything here; the rest hit the cache. */
    drawDecalVisibility(frame, view);

    drawDecals(frame, view);

    /* DEPTH AND NORMALS EXIST AND THERE IS NO COLOUR YET — which is exactly
     * what a decal wants to write into, and why this point is before the
     * occlusion pass rather than after it. */
    runHatch(ScenePassPoint::AfterDepthPrepass, view);

    /* Occlusion AFTER the prepass, because it is unprojected from what the
     * prepass wrote — depth to reconstruct a position, normals to orient the
     * sampling hemisphere. */
    drawOcclusion(frame, view);

    /* AND THE BLUR THAT FINISHES IT. Not separable from the pass above: the
     * occlusion kernel is rotated per pixel to turn banding into noise, and
     * this is what removes the noise. Running one without the other leaves a
     * grainy four-pixel field over every lit surface. */
    drawOcclusionBlur(frame, view);

    /* THE SKY, BEFORE ANY COLOUR GOES INTO THE SCENE TARGET — it is what the
     * lit pass loads rather than clears. After the depth passes rather than
     * before them only because it does not interact with depth at all; putting
     * it here keeps the two geometry passes adjacent. */
    drawSky(frame, view);

    /* THEN THE PICTURE. The lit pass tests Equal against the prepass's depth
     * and samples both the shadow map and the occlusion plane, so it has to
     * follow all three. The resolve leaves linear space and is therefore last —
     * anything drawn after it is drawing in display colour, which is where the
     * HUD and the overlays will go. */
    drawLitScene(frame, view);

    runHatch(ScenePassPoint::AfterOpaque, view);

    /* THEN WHAT YOU CAN SEE THROUGH, after the opaque scene because it reads
     * what is already in the colour buffer. A blended surface drawn before the
     * wall behind it has nothing to see through to and reads as a tinted
     * solid. */
    drawTransparent(frame, view);

    /* AFTER THE SCENE AND BEFORE THE RESOLVE. Debug geometry belongs over a
     * finished frame, and it needs the scene's depth buffer, which the resolve
     * is the end of. */
    drawDebugLines(frame);

    /* THE LAST POINT AT WHICH THE BUFFER HOLDS RADIANCE. Custom post belongs
     * here; anything after the resolve is working on display colour. */
    /* BLOOM, INTO THE HDR TARGET, BEFORE THE RESOLVE AND BEFORE THE HATCH.
     *
     * Before the resolve because the whole point is that the glow is radiance
     * like everything else — exposed with the frame, tone-mapped with the
     * frame, and correct under the no-tonemap debug view. The raylib path's
     * GlowPass composites AFTER the tone map in display colour, and every one
     * of those three properties is what it gives up.
     *
     * Before the hatch because a game's custom pass at BeforeToneMap is
     * entitled to see the finished HDR scene, and a bloom added after it would
     * be a term that pass could neither read nor affect. */
    if (frame.bloom) drawBloom(frame);

    runHatch(ScenePassPoint::BeforeToneMap, view);

    drawResolve(frame, view);

    /* THE OUTLINE, OVER THE RESOLVED PICTURE. After the tone map because it is
     * INTERFACE rather than light — see the shader, which sets that against
     * bloom's opposite decision and explains why both are right.
     *
     * Before the AfterToneMap hatch, so a game's own post pass sees the
     * finished frame including its silhouettes. */
    drawOutline(frame, view);

    runHatch(ScenePassPoint::AfterToneMap, view);
}

void ScenePipeline::drawTransparent(const SceneFrame& frame, const View& view)
{
    CW_PROFILE_ZONE_N("transparent");

    if (!sceneColour_.valid() || !sceneDepth_.valid()) return;

    DeviceProbeSet& probes = view.scene()->probes();

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
    encoder.bindTexture(4, probes.valid() ? probes.texture() : probes.emptyTexture(),
                        probes.sampler());
    encoder.bindUniformBuffer(0, probes.block());

    encoder.bindUniformBuffer(1, litBlock_);
    encoder.bindUniformBuffer(2, materialBlock_);

    /* BACK TO FRONT, which is the bug this whole design was most likely to be
     * forced by. The old path drew the transparent bucket in BUCKET order, so
     * two overlapping panes blended in whatever order the geometry happened to
     * be built in — and nothing in that architecture could fix it, because the
     * engine did not own the draws. See RenderScene::collect. */
    drawItems(encoder, cameraList_.translucent(), /*bindMaterials=*/true);


    device_.endPass(encoder);
}

/* ---- debug lines ---------------------------------------------------------*/

bool ScenePipeline::ensureDebugCapacity(uint32_t vertexCount)
{
    static_assert(sizeof(DebugVertex) == kDebugVertexStride,
                  "debugLineLayout describes DebugVertex byte for byte");

    if (vertexCount <= debugCapacity_ && debugMesh_.valid()) return true;

    /* Grow with headroom, like the UI's buffers: a debug frame that gains a few
     * segments as a trace lengthens should not recreate its buffer every frame
     * on the way. */
    const uint32_t wanted = std::max(vertexCount + vertexCount / 4u, 2048u);

    if (debugMesh_.valid())     device_.destroy(debugMesh_);
    if (debugVertices_.valid()) device_.destroy(debugVertices_);
    debugMesh_ = {};

    BufferDesc desc;
    desc.name   = "debug lines";
    desc.bytes  = static_cast<uint64_t>(wanted) * kDebugVertexStride;
    desc.usage  = BufferUsageVertex;
    desc.access = BufferAccess::CpuToGpuPerFrame;

    debugVertices_ = device_.createBuffer(desc);
    if (!debugVertices_.valid()) return false;

    /* NON-INDEXED. Two vertices per segment with no sharing between them —
     * an index buffer here would be 0,1,2,3… describing nothing. See
     * IRenderDevice::createMesh, which takes an invalid index handle for
     * exactly this case. */
    debugMesh_ = device_.createMesh(debugLineLayout(), debugVertices_, wanted);
    if (!debugMesh_.valid()) return false;

    debugCapacity_ = wanted;
    return true;
}

void ScenePipeline::drawDebugLines(const SceneFrame& frame)
{
    if (frame.debug == nullptr || frame.debug->empty()) return;
    if (!sceneColour_.valid() || !sceneDepth_.valid()) return;
    if (!debugDepthPipeline_.valid() || !debugXrayPipeline_.valid()) return;

    CW_PROFILE_ZONE_N("debug lines");

    /* BOTH PASSES BUILT INTO ONE BUFFER, depth-tested first so the x-ray range
     * can be drawn second without a second upload. The two ranges are
     * contiguous, so each is one draw. */
    const std::vector<DebugSegment>& segments = frame.debug->segments();

    debugScratch_.clear();
    debugScratch_.reserve(segments.size() * 2);

    const auto append = [&](const DebugSegment& segment) {
        const std::uint32_t rgba = packLinear(segment.colour);
        debugScratch_.push_back(DebugVertex{ segment.from.x, segment.from.y,
                                             segment.from.z, rgba });
        debugScratch_.push_back(DebugVertex{ segment.to.x, segment.to.y,
                                             segment.to.z, rgba });
    };

    for (const DebugSegment& segment : segments) {
        if (segment.depthTested) append(segment);
    }
    const uint32_t depthTestedVertices = static_cast<uint32_t>(debugScratch_.size());

    for (const DebugSegment& segment : segments) {
        if (!segment.depthTested) append(segment);
    }
    const uint32_t totalVertices = static_cast<uint32_t>(debugScratch_.size());
    if (totalVertices == 0) return;

    if (!ensureDebugCapacity(totalVertices)) return;

    device_.updateBuffer(debugVertices_, debugScratch_.data(),
                         static_cast<uint64_t>(totalVertices) * kDebugVertexStride, 0);

    PassDesc pass;
    pass.name = "debug lines";

    pass.colours[0].texture = sceneColour_;
    pass.colours[0].load    = LoadAction::Load;
    pass.colours[0].store   = StoreAction::Store;
    pass.colourCount = 1;

    /* THE SCENE'S DEPTH, LOADED AND NOT WRITTEN. A debug line must not occlude
     * the next one — twelve edges of a box would then hide each other at the
     * corners — and it must not disturb anything reading depth afterwards. */
    pass.hasDepth      = true;
    pass.depth.texture = sceneDepth_;
    pass.depth.load    = LoadAction::Load;
    pass.depth.store   = StoreAction::Store;

    ICommandEncoder& encoder = device_.beginPass(pass);

    /* THE LIT PASS'S BLOCK, unchanged: the view projection and the exposure are
     * the frame's, and the fragment stage divides the colour out of the latter
     * — see rhi/scene/debug_line.fs.glsl. */
    encoder.bindUniformBuffer(1, litBlock_);

    /* TWO RANGES OF ONE BUFFER — which is what the vertex range on draw() was
     * added for. The depth-tested segments were appended first, so they are
     * [0, depthTestedVertices) and the x-ray ones are the rest. */
    if (depthTestedVertices > 0) {
        encoder.bindPipeline(debugDepthPipeline_);
        encoder.draw(debugMesh_, depthTestedVertices, 0);
    }

    if (totalVertices > depthTestedVertices) {
        encoder.bindPipeline(debugXrayPipeline_);
        encoder.draw(debugMesh_, totalVertices - depthTestedVertices, depthTestedVertices);
    }

    device_.endPass(encoder);
}

}  // namespace cromwell
