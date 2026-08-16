/* Formats.hpp — the fixed vocabulary every backend has to agree on.
 *
 * SINGLE RESPONSIBILITY: enumerate the pixel formats, comparison functions,
 * blend factors and attachment actions the engine is allowed to ask for, in
 * terms no single graphics API owns.
 *
 * ================== WHY THESE ARE ENUMS AND NOT PASS-THROUGHS ==============
 *
 * The tempting shortcut is to let a caller hand the backend whatever constant
 * its API uses. That compiles on one platform and on exactly one platform, and
 * the failure is silent rather than loud — a GL enum reaching a Metal backend
 * is a valid integer that means something else.
 *
 * So the set is CLOSED. Adding a format means adding it here and teaching every
 * backend to translate it, which is precisely the edit the compiler should
 * demand: a backend that cannot express a format the engine offers is a fact
 * worth discovering at build time on the machine that has that backend, not in
 * a bug report from a console submission.
 *
 * WHAT IS DELIBERATELY MISSING: everything exotic. This is the set the engine's
 * passes actually use, and it stays that size. A format nobody renders with is
 * a translation table entry in four backends earning nothing.
 */
#pragma once

#include <cstdint>

namespace cromwell::rhi {

/* ---- pixel formats -------------------------------------------------------
 *
 * NAMED BY CHANNELS, WIDTH AND INTERPRETATION, in that order, because that is
 * what a pass author needs to reason about. `RGBA16F` is the lit target's
 * format because the pipeline is linear and radiance does not fit in eight
 * bits; `RGBA8Srgb` is what a resolve writes because a display is not linear.
 * Getting that pair the wrong way round is the classic washed-out frame, and
 * having the transfer function IN THE NAME is what makes it visible at the
 * declaration. */
enum class TextureFormat : uint8_t {
    Unknown = 0,

    /* Colour. */
    R8,
    RG8,
    RGBA8,
    RGBA8Srgb,
    RGBA16F,
    RGBA32F,
    R16F,
    R32F,
    RG16F,

    /* Packed. R11G11B10F is the cheap HDR colour format — no alpha, which is
     * why it is not the lit target's default, but half the bandwidth when a
     * pass has no coverage to carry. */
    R11G11B10F,

    /* Depth and stencil. D32F is the default: a 24-bit depth buffer's precision
     * over a large world is the classic cause of z-fighting at distance, and
     * every current target supports the 32-bit float. */
    D16,
    D24S8,
    D32F,
    D32FS8,
};

bool constexpr isDepthFormat(TextureFormat format)
{
    return format == TextureFormat::D16 || format == TextureFormat::D24S8
        || format == TextureFormat::D32F || format == TextureFormat::D32FS8;
}

/* ---- attachment actions --------------------------------------------------
 *
 * WHAT HAPPENS TO AN ATTACHMENT AT THE START AND END OF A PASS, and this is the
 * single most important thing in this file for the console and macOS targets.
 *
 * Those are TILE-BASED renderers. A tiler keeps the attachment in fast on-chip
 * memory for the duration of a pass and only touches main memory at the edges,
 * so `load` and `store` are not bookkeeping — they are the difference between a
 * pass that costs its own work and one that also pays a full-resolution read
 * and write of every attachment it touches.
 *
 * DISCARD IS THE ONE PEOPLE FORGET. A depth buffer that is consumed inside the
 * pass and never read afterwards should be StoreAction::Discard, and on a tiler
 * that saves writing the whole thing out. An immediate-mode GL backend ignores
 * these entirely, which is why they must be stated by the pass rather than
 * inferred by the backend: the backend that could infer them is the one where
 * they do not matter. */
enum class LoadAction : uint8_t {
    /* Contents are undefined on entry. The cheapest, and correct whenever the
     * pass writes every pixel it cares about. */
    DontCare,

    /* Keep what is already there — for a pass that composites onto a previous
     * one's output. */
    Load,

    /* Fill with the pass's clear value. */
    Clear,
};

enum class StoreAction : uint8_t {
    /* Keep the result. */
    Store,

    /* Throw it away — nothing reads this attachment after the pass. */
    Discard,
};

/* ---- depth and blend -----------------------------------------------------*/

enum class CompareFunc : uint8_t {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always,
};

/* WHAT HAPPENS TO A STENCIL TEXEL, at each of the three outcomes a fragment can
 * reach. The set is every backend's, unchanged — GL, Vulkan, D3D and Metal all
 * offer exactly these eight and no more, which is why it can be a closed enum
 * rather than a negotiation.
 *
 * CLAMP VERSUS WRAP IS NOT A DETAIL. An increment that saturates at 255 counts
 * "how many, up to 255"; one that wraps counts modulo 256, so the 256th
 * overlapping fragment reads as none at all. Portal rendering and constructive
 * stencil shadows want the wrapping pair and everything else wants the clamping
 * one, and a caller that picks by name rather than by habit gets it right. */
enum class StencilOp : uint8_t {
    Keep,
    Zero,
    Replace,          /* with the reference value — the tagging case */
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap,
};

enum class BlendFactor : uint8_t {
    Zero, One,
    SrcColour, OneMinusSrcColour,
    SrcAlpha, OneMinusSrcAlpha,
    DstColour, OneMinusDstColour,
    DstAlpha, OneMinusDstAlpha,
};

enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

enum class CullMode : uint8_t { None, Front, Back };

/* Which way round a front face is wound. Stated rather than assumed because it
 * is the setting that silently inverts when geometry crosses a backend with a
 * different clip-space handedness, and the symptom — an interior that renders
 * as an exterior — reads as a modelling bug. */
enum class Winding : uint8_t { CounterClockwise, Clockwise };

enum class PrimitiveType : uint8_t { Triangles, TriangleStrip, Lines, LineStrip, Points };

/* ---- how a texture may be used -------------------------------------------
 *
 * FIXED AT CREATION, because Vulkan, Metal and the console APIs all place the
 * resource differently depending on the answer and none of them can be told
 * afterwards. A bitmask rather than an enum: a shadow map is rendered into AND
 * sampled, which is two of these and the common case. */
enum TextureUsage : uint32_t {
    TextureUsageSampled      = 1u << 0,
    TextureUsageRenderTarget = 1u << 1,
    TextureUsageDepthTarget  = 1u << 2,
    TextureUsageStorage      = 1u << 3,  /* read/write from a compute shader */
    TextureUsageCopySource   = 1u << 4,
    TextureUsageCopyDest     = 1u << 5,
};

enum BufferUsage : uint32_t {
    BufferUsageVertex  = 1u << 0,
    BufferUsageIndex   = 1u << 1,
    BufferUsageUniform = 1u << 2,
    BufferUsageStorage = 1u << 3,
    BufferUsageCopySource = 1u << 4,
    BufferUsageCopyDest   = 1u << 5,
};

/* How often the contents change, which is how a backend decides where to put
 * it. Static geometry belongs in device-local memory the CPU cannot reach;
 * a uniform buffer rewritten every frame belongs somewhere the CPU can. */
enum class BufferAccess : uint8_t { GpuOnly, CpuToGpuOnce, CpuToGpuPerFrame };

enum class FilterMode : uint8_t { Nearest, Linear };
enum class WrapMode   : uint8_t { Repeat, ClampToEdge, ClampToBorder, MirrorRepeat };

}  // namespace cromwell::rhi
