#include "cromwell/lighting/DeviceProbeSet.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {
namespace {

using namespace cromwell::rhi;

/* THE SIX FACES, IN GL's ORDER AND GL's ORIENTATIONS — identical to
 * ReflectionProbeSet's table and for the identical reason: a cubemap's faces
 * are defined in a LEFT-handed space, so four of the six up vectors point
 * DOWN. A face rendered with the intuitive up vector comes out mirrored, which
 * is far harder to spot than one that is obviously broken — the geometry is all
 * there, in the right places, reflected the wrong way round.
 *
 * Copied rather than shared because the raylib set speaks raylib's Vector3 and
 * this speaks the engine's Vec3, and they are deleted together at parity. If
 * one is ever edited, both are wrong until the other is. */
struct CubeFace {
    Vec3 forward;
    Vec3 up;
};

constexpr CubeFace kFaces[6] = {
    { {  1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* +X */
    { { -1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* -X */
    { {  0.0f,  1.0f,  0.0f }, { 0.0f,  0.0f,  1.0f } },   /* +Y */
    { {  0.0f, -1.0f,  0.0f }, { 0.0f,  0.0f, -1.0f } },   /* -Y */
    { {  0.0f,  0.0f,  1.0f }, { 0.0f, -1.0f,  0.0f } },   /* +Z */
    { {  0.0f,  0.0f, -1.0f }, { 0.0f, -1.0f,  0.0f } },   /* -Z */
};

/* NINETY DEGREES, AND IT IS NOT A TUNING KNOB. Six square faces meeting at a
 * point is exactly a cube; anything else leaves gaps or overlaps between the
 * faces, and the seams land wherever the reflection is most obviously wrong. */
constexpr float kFaceFovY = 1.5707963f;   /* pi/2 */

/* NEAR PLANE AT 5 CENTIMETRES OF TILE. The capture point sits in the middle of
 * an open cell, so nothing legitimate is closer than half a cell — but a probe
 * in a cupboard-sized room would clip its own walls at a larger value, and
 * depth precision is not the constraint here: the far plane is the board's
 * diagonal, not a kilometre. */
constexpr float kFaceNear = 0.05f;

/* THE VOLUME BLOCK, std140 — five vec4 arrays of kMaxProbes.
 *
 * ARRAYS OF vec4 RATHER THAN A STRUCT ARRAY, deliberately. std140 pads every
 * array element to sixteen bytes, so a `struct { vec3 a; float b; ... }[16]`
 * would have padding the C++ side must reproduce exactly and no diagnostic when
 * it does not. Five parallel arrays of a type that is already sixteen bytes has
 * no padding to get wrong.
 *
 * THE FOURTH COMPONENTS CARRY REAL DATA, which is why the arrays are vec4 and
 * not vec3 in the first place: transition rides in capture.w and priority in
 * parallaxMin.w. That is free — a vec3 array has vec4 stride regardless — and
 * it is what keeps the block at 1280 bytes instead of 1792.
 *
 * THE GLSL HALF IS rhi/probes.glsl, and the two are one contract written twice.
 * Add to the END of both, together. */
struct ProbeBlockData {
    float capture[DeviceProbeSet::kMaxProbes][4];       /* xyz point,  w transition */
    float parallaxMin[DeviceProbeSet::kMaxProbes][4];   /* xyz min,    w priority   */
    float parallaxMax[DeviceProbeSet::kMaxProbes][4];
    float influenceMin[DeviceProbeSet::kMaxProbes][4];
    float influenceMax[DeviceProbeSet::kMaxProbes][4];
};
static_assert(sizeof(ProbeBlockData) == DeviceProbeSet::kMaxProbes * 16 * 5,
              "std140: five vec4 arrays, 16-byte stride per element");

void writeVec3(float (&destination)[4], Vec3 value)
{
    destination[0] = value.x;
    destination[1] = value.y;
    destination[2] = value.z;
}

}  // namespace

Mat4 DeviceProbeSet::faceViewProjection(int face, Vec3 eye, float farPlane)
{
    if (face < 0 || face > 5) return Mat4{};

    const CubeFace& current = kFaces[face];
    const Mat4 view = Mat4::lookAt(eye, eye + current.forward, current.up);
    const Mat4 projection = Mat4::perspective(kFaceFovY, 1.0f, kFaceNear, farPlane);
    return projection * view;
}

DeviceProbeSet::DeviceProbeSet(rhi::IRenderDevice& device) : device_(device) {}

DeviceProbeSet::~DeviceProbeSet() { release(); }

void DeviceProbeSet::release()
{
    for (rhi::SamplerHandle handle : levelSamplers_)
        if (handle.valid()) device_.destroy(handle);
    levelSamplers_.clear();

    if (block_.valid())   device_.destroy(block_);
    if (sampler_.valid()) device_.destroy(sampler_);
    if (depth_.valid())   device_.destroy(depth_);
    if (empty_.valid())   device_.destroy(empty_);
    if (array_.valid())   device_.destroy(array_);

    block_   = {};
    sampler_ = {};
    depth_   = {};
    empty_   = {};
    array_   = {};
}

bool DeviceProbeSet::create()
{
    release();

    /* ASKED, NOT ASSUMED. A cube array is core in GL 4.0 and universal on the
     * console targets, but macOS's 4.1 GL and any future software backend are
     * exactly the cases DeviceCapabilities exists to answer. Falling back to the
     * analytic sky is a flatter frame, not a broken one. */
    if (!device_.capabilities().cubeArrays) {
        LOGGER.warn("probes: this device has no cubemap arrays - surfaces keep the analytic sky");
        return false;
    }

    TextureDesc array;
    array.name      = "reflection probes";
    array.width     = kFaceSize;
    array.height    = kFaceSize;

    /* SIX LAYERS PER PROBE, and this is the descriptor field most likely to be
     * got wrong: a cube array's layer count is `6 * probes`, not `probes`. The
     * backend picks GL_TEXTURE_CUBE_MAP_ARRAY over GL_TEXTURE_CUBE_MAP off
     * exactly this number. */
    array.layers    = kMaxProbes * 6;
    array.cube      = true;

    /* RGBA16F, and both halves matter. The COLOUR has to be HDR because the
     * capture is the lit pass — real sun radiance, several times one — and
     * clipping it to 8 bits would make every reflection of a sunlit wall the
     * same white. The ALPHA is not coverage: it is "did this direction see
     * world", which is what lets the shader blend back to the analytic sky
     * where the capture saw open air, without the sky ever being rendered into
     * the cubemap. */
    array.format    = TextureFormat::RGBA16F;
    array.usage     = TextureUsageSampled | TextureUsageRenderTarget;

    /* THE PREFILTERED CHAIN. Level L is the probe convolved for roughness
     * L/(kMipLevels-1), built by ScenePipeline's prefilter pass once all six of
     * a probe's faces are current.
     *
     * NOT glGenerateMipmap's CHAIN, and the difference is the whole point. A box
     * filter is one call and would "work" — it is also not a specular lobe, so
     * the roughness-to-level mapping would mean nothing physical and every rough
     * reflection would be wrong in a way that is hard to attribute later. See
     * rhi/probe_prefilter.fs.glsl. */
    array.mipLevels = kMipLevels;

    /* The old note, kept because it explains what the fade in the shader was
     * for and why deleting it is the payoff rather than a tidy-up:
     *
     * ONE LEVEL — NO PREFILTERED MIP CHAIN, and its absence was a decision.
     *
     * A real IBL prefilters roughness into the mip chain, so a rough surface
     * samples a blurred level rather than a mirror. Generating box-filtered
     * mips is NOT that: a box filter is not a GGX convolution, and handing a
     * 0.8-roughness wall a 4x4 average of its room would be a different wrong
     * answer with more memory behind it.
     *
     * So the shader does what the raylib path does instead — slides the probe
     * back to the analytic sky as roughness rises, on the grounds that a fully
     * rough reflection converges on the irradiance and the two-lobe sky already
     * approximates one. A prefilter pass is the upgrade, and it wants its own
     * compute dispatch rather than generateMips(). */

    array_ = device_.createTexture(array);
    if (!array_.valid()) {
        LOGGER.warn("probes: the cubemap array could not be created - "
                    "surfaces keep the analytic sky");
        return false;
    }

    /* THE STAND-IN THE CAPTURE PASS BINDS. Twelve layers rather than six,
     * because six would make the backend choose GL_TEXTURE_CUBE_MAP and a
     * samplerCubeArray reading a plain cubemap is undefined — which is the
     * failure this texture exists to prevent, arriving from the other side. */
    TextureDesc empty = array;
    empty.name   = "reflection probes (empty)";
    empty.width  = 1;
    empty.height = 1;
    empty.layers = 12;
    empty_ = device_.createTexture(empty);
    if (!empty_.valid()) return false;

    /* ONE FACE'S DEPTH, reused by all ninety-six. Never sampled — the capture
     * has no prepass and nothing reconstructs a position from it — so it is
     * cleared per pass and discarded at the end of one. */
    TextureDesc depth;
    depth.name   = "probe capture depth";
    depth.width  = kFaceSize;
    depth.height = kFaceSize;
    depth.format = TextureFormat::D32F;
    depth.usage  = TextureUsageDepthTarget;
    depth_ = device_.createTexture(depth);
    if (!depth_.valid()) return false;

    /* BILINEAR AND CLAMPED. Bilinear because a 128-pixel face stretched over a
     * window is otherwise visibly blocky, and clamped because a cube face's
     * edge must not wrap round to the opposite side of the same face — which is
     * a bright seam along four edges of every reflection.
     *
     * WHAT THIS DOES NOT DO is make the seams BETWEEN faces disappear. That is
     * GL_TEXTURE_CUBE_MAP_SEAMLESS, which is a context-wide enable rather than
     * a sampler state, and turning it on here would change the raylib path's
     * probes underneath the A/B comparison the two renderers exist for. It
     * belongs in the device's creation, after parity. */
    SamplerDesc sampler;
    sampler.minify  = FilterMode::Linear;
    sampler.magnify = FilterMode::Linear;
    sampler.mip     = FilterMode::Linear;
    sampler.wrapU   = WrapMode::ClampToEdge;
    sampler.wrapV   = WrapMode::ClampToEdge;
    sampler.wrapW   = WrapMode::ClampToEdge;
    sampler_ = device_.createSampler(sampler);
    if (!sampler_.valid()) return false;

    BufferDesc block;
    block.name   = "probe block";
    block.bytes  = sizeof(ProbeBlockData);
    block.usage  = BufferUsageUniform;

    /* NOT PER FRAME. The volumes change when the world does — a wall comes
     * down and two rooms become one — and not otherwise, so re-sending 1280
     * bytes every frame to say the same thing is exactly what the frequency
     * table in CONVENTIONS.md exists to stop. */
    block.access = BufferAccess::CpuToGpuOnce;
    block_ = device_.createBuffer(block);
    if (!block_.valid()) return false;

    /* UPLOADED EMPTY, NOT LEFT UNDEFINED. There are no probes yet, so nothing
     * reads past index zero — but a uniform block a driver has never been given
     * contents for is a block full of whatever the allocation held, and the
     * frames between here and the first placement would sample garbage boxes if
     * anything ever did read it. */
    blockDirty_ = true;
    uploadBlock();

    /* ONE SAMPLER PER SOURCE LEVEL, each clamped to exactly that level so the
     * prefilter cannot reach the level it is writing — see levelSampler. */
    levelSamplers_.assign(static_cast<std::size_t>(kMipLevels), rhi::SamplerHandle{});
    for (int level = 0; level < kMipLevels; level++) {
        SamplerDesc clamped = sampler;
        clamped.minLod = static_cast<float>(level);
        clamped.maxLod = static_cast<float>(level);
        levelSamplers_[static_cast<std::size_t>(level)] = device_.createSampler(clamped);
        if (!levelSamplers_[static_cast<std::size_t>(level)].valid()) return false;
    }

    faceMask_.assign(static_cast<std::size_t>(kMaxProbes), 0u);

    LOGGER.info("probes: {0}x{0} RGBA16F cubemap array, {1} layers, {3} mips ({2} KB)",
                kFaceSize, kMaxProbes * 6,
                kMaxProbes * 6 * kFaceSize * kFaceSize * 8 / 1024 * 4 / 3, kMipLevels);
    return true;
}

void DeviceProbeSet::clear()
{
    volumes_.clear();
    cursor_ = 0;
    staleFaces_ = 0;
    blockDirty_ = true;
    std::fill(faceMask_.begin(), faceMask_.end(), uint8_t{ 0 });
}

bool DeviceProbeSet::addProbe(const Volume& volume)
{
    if (!valid()) return false;

    /* The shader carries volume arrays of kMaxProbes and the array texture has
     * exactly that many layers, so a probe past the ceiling has nowhere to
     * live. Dropping it rather than asserting is deliberate: the caller decides
     * which rooms matter, and it can only make that call if a full set fails
     * softly. */
    if (probeCount() >= kMaxProbes) return false;

    volumes_.push_back(volume);

    /* Every existing layer now describes a probe list that has changed shape,
     * and the new one has never been captured at all. */
    cursor_ = 0;
    blockDirty_ = true;
    markAllStale();
    return true;
}

DeviceProbeSet& DeviceProbeSet::withCaptureFar(float distance)
{
    /* A far plane at or behind the near one is a degenerate projection — every
     * face would come out empty, which reads as "the probes are broken" rather
     * than as a bad argument. */
    if (distance > kFaceNear) captureFar_ = distance;
    return *this;
}

void DeviceProbeSet::uploadBlock()
{
    if (!blockDirty_ || !block_.valid()) return;
    blockDirty_ = false;

    /* ZEROED IN FULL, not just up to probeCount(). The shader loops to
     * uProbeCount and never reads past it, so the tail is unread today — and
     * the day something does read it, a zero box is a volume that contains
     * nothing rather than one that contains everything. */
    ProbeBlockData block{};

    for (std::size_t i = 0; i < volumes_.size(); i++) {
        const Volume& volume = volumes_[i];

        writeVec3(block.capture[i], volume.capture);
        block.capture[i][3] = volume.transition;

        writeVec3(block.parallaxMin[i], volume.parallaxMin);
        block.parallaxMin[i][3] = volume.priority;

        writeVec3(block.parallaxMax[i], volume.parallaxMax);

        /* NON-ZERO MEANS "CORRECT AGAINST THE BOX". Zero is an environment at
         * infinity, which is what the outdoor volume is — see Volume::parallax
         * for why a board-sized box is the wrong thing to aim at. */
        block.parallaxMax[i][3] = volume.parallax ? 1.0f : 0.0f;

        writeVec3(block.influenceMin[i], volume.influenceMin);
        writeVec3(block.influenceMax[i], volume.influenceMax);
    }

    device_.updateBuffer(block_, &block, sizeof block, 0);
}

rhi::SamplerHandle DeviceProbeSet::levelSampler(int sourceLevel) const
{
    if (sourceLevel < 0 || sourceLevel >= static_cast<int>(levelSamplers_.size()))
        return sampler_;
    return levelSamplers_[static_cast<std::size_t>(sourceLevel)];
}

int DeviceProbeSet::probeReadyToPrefilter() const
{
    /* ALL SIX BITS SET AND NOT YET PREFILTERED. The mask is per probe and the
     * six faces arrive on six different frames, so this is false for most of a
     * sweep and true exactly once per probe per rebuild. */
    constexpr uint8_t kAllFaces = 0x3Fu;

    for (int probe = 0; probe < probeCount(); probe++) {
        const std::size_t at = static_cast<std::size_t>(probe);
        if (at >= faceMask_.size()) break;
        if (faceMask_[at] == kAllFaces) return probe;
    }
    return -1;
}

void DeviceProbeSet::markPrefiltered(int probe)
{
    if (probe < 0 || static_cast<std::size_t>(probe) >= faceMask_.size()) return;

    /* THE MASK GOES BACK TO EMPTY, and that is what makes this fire once per
     * SWEEP rather than once per frame.
     *
     * The bits accumulate as the round robin visits a probe's faces, so all six
     * set means "every face has been recaptured since the last chain was built".
     * Clearing them here is what restores that meaning. Leaving them set — which
     * is what a separate "already prefiltered" flag did — made the condition
     * permanently true after the first sweep, so a single face landing
     * re-convolved the entire probe: thirty passes every frame, for ever,
     * measured at 243 rebuilds across 250 frames. */
    faceMask_[static_cast<std::size_t>(probe)] = 0u;
}

bool DeviceProbeSet::nextFace(Face& out)
{
    if (!valid() || volumes_.empty()) return false;

    /* HERE RATHER THAN IN addProbe, because a caller places every room in a
     * loop and uploading per addProbe would send the block sixteen times to
     * describe one placement. This is the last moment before anything reads
     * it. */
    uploadBlock();

    const int pairCount = probeCount() * 6;

    const int pair = cursor_ % pairCount;
    cursor_ = (cursor_ + 1) % pairCount;

    out.probe = pair / 6;
    out.index = pair % 6;
    out.slice = pair;
    out.eye   = volumes_[static_cast<std::size_t>(out.probe)].capture;

    /* THE SAME CALL THE SELF-TEST'S ORIENTATION STAGE MAKES. Not inlined here,
     * so a mirrored face table fails that stage rather than shipping. */
    out.viewProjection = faceViewProjection(out.index, out.eye, captureFar_);

    /* THIS FACE IS NOW CURRENT AND THE CHAIN BUILT FROM IT IS NOT. Recorded
     * before the caller has drawn it, for the same reason the stale count is
     * decremented here: nextFace hands out work the caller is committed to, and
     * a second call the caller had to remember to make is one that eventually
     * gets forgotten on one path. */
    /* SHIFTED BY AN UNSIGNED VALUE THAT IS RANGE-CHECKED. `out.index` is
     * `pair % 6` and the cursor only ever advances modulo a positive count, so
     * it cannot be negative — but that is an invariant three lines of arithmetic
     * away, the analyser cannot see it, and a shift by a negative amount is
     * undefined behaviour rather than merely a wrong answer. Stating the bound
     * costs one comparison on a path that runs a handful of times a frame. */
    const unsigned int face = static_cast<unsigned int>(out.index);
    if (!faceMask_.empty() && face < 6u)
        faceMask_[static_cast<std::size_t>(out.probe)] |= static_cast<uint8_t>(1u << face);

    /* DECREMENTED HERE, not by the caller. nextFace hands out work and the
     * caller is committed to drawing it, so the two cannot get out of step —
     * and a counter the caller had to remember to decrement is a counter that
     * eventually never reaches zero and makes the set capture at four times the
     * rate forever. */
    if (staleFaces_ > 0) staleFaces_--;

    return true;
}

}  // namespace cromwell
