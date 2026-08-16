/* Descriptors.hpp — what to make, said once, up front.
 *
 * SINGLE RESPONSIBILITY: describe every GPU object the engine can ask a device
 * to create, as plain data with no API in it.
 *
 * ==================== WHY EVERYTHING IS DESCRIBED UP FRONT =================
 *
 * Because three of the four target backends demand it. Metal, Vulkan and the
 * console APIs all validate a pipeline when it is built and refuse to let its
 * state be poked afterwards; only GL allows the state machine that raylib's API
 * (and therefore this engine's passes, today) is written against.
 *
 * That asymmetry decides the design. An interface with loose setters can be
 * implemented on an explicit backend only by recording every poke and building
 * a pipeline lazily at draw time — which is slow, is a cache nobody asked for,
 * and moves every validation failure from "at load, with a name" to "at draw,
 * as a black screen". Describing up front is the shape that costs the GL
 * backend nothing and saves the other three from emulating a machine that no
 * longer exists.
 *
 * ============================ AGGREGATES, ALL OF THEM ======================
 *
 * These are one-shot data carriers in exactly the sense CLAUDE.md sets out:
 * filled by one caller, read by one callee, dead once the resource exists. No
 * invariant spans their fields — a descriptor is not wrong until the device
 * validates it — so public fields and designated initialisers are the clearest
 * thing here, and the project's private-members rule does not apply. Same
 * decision, same reasoning, as FrameView and PassContext.
 */
#pragma once

#include "cromwell/rhi/Formats.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>

namespace cromwell::rhi {

/* Linear RGBA, 0..1 nominal but unclamped — a clear value for an HDR target is
 * legitimately above one. THE ENGINE'S OWN COLOUR TYPE and not a backend's:
 * four floats need no API to mean what they mean. */
struct ClearColour {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/* ---- resources -----------------------------------------------------------*/

struct TextureDesc {
    const char*   name   = nullptr;   /* for the backend's debug labels */
    uint32_t      width  = 0;
    uint32_t      height = 0;

    /* 1 for a plain texture, 6 for a cubemap, more for an array. A cube array —
     * which the reflection probes want — is `6 * probeCount`. */
    uint32_t      layers = 1;
    uint32_t      mipLevels = 1;

    TextureFormat format = TextureFormat::RGBA8;
    uint32_t      usage  = TextureUsageSampled;

    /* Cubemaps are a distinct kind on every backend, not merely six layers. */
    bool          cube   = false;
};

struct SamplerDesc {
    FilterMode minify  = FilterMode::Linear;
    FilterMode magnify = FilterMode::Linear;
    FilterMode mip     = FilterMode::Linear;
    WrapMode   wrapU   = WrapMode::Repeat;
    WrapMode   wrapV   = WrapMode::Repeat;
    WrapMode   wrapW   = WrapMode::Repeat;

    float      maxAnisotropy = 1.0f;

    /* WHICH MIP LEVELS THIS SAMPLER MAY READ. The defaults span everything any
     * texture can have, so a sampler that says nothing behaves as it always did.
     *
     * THIS EXISTS FOR ONE JOB AND IT IS NOT AN OPTIMISATION. Generating a
     * prefiltered chain means rendering INTO mip N while sampling mip N-1 of the
     * SAME texture, and a texture that is simultaneously an attachment and a
     * live sampler read is undefined — unless the sampler provably cannot reach
     * the level being written. Every backend expresses that as a LOD clamp, and
     * without it the only alternative is a second full-size scratch array to
     * ping-pong through, which is megabytes to avoid two floats. */
    float      minLod = 0.0f;
    float      maxLod = 1000.0f;

    /* A SHADOW SAMPLER, which is a different object on every backend and not a
     * flag on a read. Set the function to anything but Never and the sampler
     * compares rather than fetches, so the hardware's bilinear unit does the
     * percentage-closer filtering for free instead of the shader doing four
     * taps by hand. */
    CompareFunc compare = CompareFunc::Never;
};

struct BufferDesc {
    const char*  name  = nullptr;
    uint64_t     bytes = 0;
    uint32_t     usage = 0;
    BufferAccess access = BufferAccess::GpuOnly;
};

/* ---- vertex layout -------------------------------------------------------
 *
 * WHAT A VERTEX LOOKS LIKE, declared so a pipeline can be validated against the
 * shader it was built for. Slots are numbered rather than named: names are a GL
 * and D3D reflection idea, and matching by index is what the explicit backends
 * do. The shader's `layout(location = N)` is the contract. */
enum class VertexFormat : uint8_t {
    Float1, Float2, Float3, Float4,
    UByte4, UByte4Normalised,
    Short2, Short2Normalised,
    Short4, Short4Normalised,
};

struct VertexAttribute {
    uint32_t     location = 0;
    uint32_t     offset   = 0;
    VertexFormat format   = VertexFormat::Float3;
};

struct VertexLayout {
    static constexpr int kMaxAttributes = 8;

    VertexAttribute attributes[kMaxAttributes]{};
    int             attributeCount = 0;
    uint32_t        stride = 0;

    /* Advance per instance rather than per vertex — the whole layout, because
     * mixing the two in one buffer is a complication no pass here needs. */
    bool            perInstance = false;
};

/* ---- pipeline ------------------------------------------------------------*/

struct DepthState {
    bool        test  = true;
    bool        write = true;
    CompareFunc compare = CompareFunc::Less;

    /* Depth bias, for shadow casters. In the API-neutral units every backend
     * exposes: a constant offset and a slope-scaled one. Stated here rather
     * than baked into the shadow shader because it is fixed-function on all
     * four targets and doing it in the shader gives up the hardware's. */
    float       biasConstant = 0.0f;
    float       biasSlope    = 0.0f;
};

/* ---- the stencil ---------------------------------------------------------
 *
 * OFF BY DEFAULT, AND OFF MEANS THE TEST ALWAYS PASSES AND NOTHING IS WRITTEN.
 * That is the identity, so a pipeline that says nothing about the stencil
 * behaves exactly as every pipeline in this engine did before this state
 * existed — which is what makes adding it a non-event for the fifteen passes
 * that do not want one.
 *
 * ONE FACE, NOT TWO. Every backend can set front and back separately, and the
 * one technique that genuinely needs it — two-sided stencil shadow volumes —
 * is not something this engine does or is planned to. A second face doubles
 * this struct and the translation table to serve a technique that would also
 * need depth-fail volumes and a shadow-volume extruder. When one arrives, it
 * arrives with its own entry here; guessing at it now is the speculative
 * generality §4.11 warns about.
 *
 * THE MASKS ARE SEPARATE AND BOTH MATTER. `readMask` is ANDed into both sides
 * of the comparison, which is how several effects share one 8-bit buffer by
 * owning different bits. `writeMask` decides which bits an op may change — and
 * it also masks the CLEAR, which is the trap this state is most likely to
 * spring; see the GL backend, where the clear resets it deliberately.
 */
struct StencilState {
    bool        enabled = false;

    /* Always, with the default masks and ops, is a test that changes nothing. */
    CompareFunc compare = CompareFunc::Always;

    uint32_t    readMask  = 0xFFu;
    uint32_t    writeMask = 0xFFu;

    /* WHAT TO DO AT EACH OUTCOME. The names are the outcome, not the action, so
     * a reader does not have to remember the argument order that every graphics
     * API gets wrong: `depthFail` is "the stencil passed and the DEPTH test did
     * not", which is the one that reads backwards in GL's `glStencilOp(sfail,
     * dpfail, dppass)`. */
    StencilOp   onStencilFail = StencilOp::Keep;
    StencilOp   onDepthFail   = StencilOp::Keep;
    StencilOp   onPass        = StencilOp::Keep;

    /* THE REFERENCE LIVES ON THE ENCODER, NOT HERE — see
     * ICommandEncoder::setStencilReference. It is the one part of this that
     * genuinely varies per DRAW rather than per pass: a tagging pass writes a
     * different id per object and nothing else about its state changes. Putting
     * it in the pipeline would mean a pipeline object per id. */

    /* THE TWO SHAPES ALMOST EVERY CALLER WANTS, named so they are not rebuilt
     * by hand at each site. Anything else is spelled out in full, which is
     * appropriate: a stencil configuration that is not one of these is doing
     * something specific and should say so. */

    /* WRITE the reference wherever a fragment survives depth. The tagging half
     * of every stencil technique. */
    static StencilState write(uint32_t writeMask = 0xFFu)
    {
        StencilState state;
        state.enabled   = true;
        state.compare   = CompareFunc::Always;
        state.writeMask = writeMask;
        state.onPass    = StencilOp::Replace;
        return state;
    }

    /* DRAW ONLY WHERE the buffer already holds the reference, changing nothing.
     * The masking half. */
    static StencilState testEqual(uint32_t readMask = 0xFFu)
    {
        StencilState state;
        state.enabled   = true;
        state.compare   = CompareFunc::Equal;
        state.readMask  = readMask;
        state.writeMask = 0u;
        return state;
    }
};

struct BlendState {
    bool        enabled = false;
    BlendFactor sourceColour = BlendFactor::SrcAlpha;
    BlendFactor destColour   = BlendFactor::OneMinusSrcAlpha;
    BlendOp     colourOp     = BlendOp::Add;
    BlendFactor sourceAlpha  = BlendFactor::One;
    BlendFactor destAlpha    = BlendFactor::OneMinusSrcAlpha;
    BlendOp     alphaOp      = BlendOp::Add;

    /* PREMULTIPLIED, which several passes here want and which is easy to get
     * subtly wrong by hand: the shader has already scaled colour by coverage,
     * so the blender must ADD what it is given rather than scale it again. */
    static BlendState premultiplied()
    {
        BlendState state;
        state.enabled      = true;
        state.sourceColour = BlendFactor::One;
        state.destColour   = BlendFactor::OneMinusSrcAlpha;
        state.sourceAlpha  = BlendFactor::One;
        state.destAlpha    = BlendFactor::OneMinusSrcAlpha;
        return state;
    }

    static BlendState alpha() { return BlendState{ true, BlendFactor::SrcAlpha,
                                                   BlendFactor::OneMinusSrcAlpha, BlendOp::Add,
                                                   BlendFactor::One,
                                                   BlendFactor::OneMinusSrcAlpha, BlendOp::Add }; }
    static BlendState additive() { BlendState s; s.enabled = true;
                                   s.sourceColour = BlendFactor::One;
                                   s.destColour = BlendFactor::One;
                                   s.sourceAlpha = BlendFactor::One;
                                   s.destAlpha = BlendFactor::One; return s; }
};

struct RasterState {
    CullMode      cull      = CullMode::Back;
    Winding       winding   = Winding::CounterClockwise;
    PrimitiveType primitive = PrimitiveType::Triangles;
    bool          wireframe = false;   /* ignored where the target has no such mode */
};

struct PipelineDesc {
    const char*  name   = nullptr;
    ShaderHandle shader;
    VertexLayout vertexLayout;

    DepthState   depth;
    StencilState stencil;
    BlendState   blend;
    RasterState  raster;

    /* THE ATTACHMENT FORMATS THIS PIPELINE WILL BE USED WITH. Required, and
     * required up front, because Metal and Vulkan compile the pipeline against
     * them — a pipeline built for an RGBA8 target cannot be bound inside a pass
     * writing RGBA16F, and finding that out at draw time is exactly the class
     * of failure this whole file exists to move to load time. */
    static constexpr int kMaxColourAttachments = 4;
    TextureFormat colourFormats[kMaxColourAttachments]{};
    int           colourCount  = 1;
    TextureFormat depthFormat  = TextureFormat::D32F;
};

/* ---- render pass ---------------------------------------------------------*/

struct ColourAttachment {
    TextureHandle texture;
    uint32_t      layer = 0;   /* which cube face or array slice */
    uint32_t      mip   = 0;

    LoadAction    load  = LoadAction::Clear;
    StoreAction   store = StoreAction::Store;
    ClearColour   clearTo;
};

struct DepthAttachment {
    TextureHandle texture;
    uint32_t      layer = 0;

    LoadAction    load  = LoadAction::Clear;
    StoreAction   store = StoreAction::Store;
    float         clearTo = 1.0f;

    /* Separate from the depth plane's, because a combined D24S8 still has two
     * of them and a pass routinely wants to keep one and drop the other. */
    LoadAction    stencilLoad  = LoadAction::DontCare;
    StoreAction   stencilStore = StoreAction::Discard;
    uint32_t      stencilClearTo = 0;
};

/* EVERYTHING A PASS TOUCHES, DECLARED BEFORE IT STARTS — the shape the tile
 * based targets need (see Formats.hpp on LoadAction) and the thing an
 * immediate-mode API never made anyone say.
 *
 * A pass with no attachments at all renders to the swapchain's backbuffer. */
struct PassDesc {
    const char* name = nullptr;   /* becomes the GPU profiler zone and debug marker */

    static constexpr int kMaxColourAttachments = PipelineDesc::kMaxColourAttachments;
    ColourAttachment colours[kMaxColourAttachments]{};
    int              colourCount = 0;

    DepthAttachment  depth;
    bool             hasDepth = false;
};

}  // namespace cromwell::rhi
