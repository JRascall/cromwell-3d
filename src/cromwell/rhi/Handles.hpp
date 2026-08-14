/* Handles.hpp — what a GPU resource is called, everywhere except inside the
 * backend that made it.
 *
 * SINGLE RESPONSIBILITY: name GPU resources in a way that carries no API with
 * it, and make the compiler refuse to confuse one kind for another.
 *
 * ===================== WHY A HANDLE AND NOT A POINTER =====================
 *
 * cromwell has to run on Windows and Linux (GL 4.3 today), on macOS — where GL
 * is deprecated and capped at 4.1, which does not have the compute shaders
 * cromwell/gpu/compute already uses — and on consoles, whose APIs are explicit,
 * proprietary and under NDA. A pointer to a backend object would put that
 * backend's type in every signature that touches a texture, and on the console
 * targets the header declaring that type cannot be committed to this repository
 * at all.
 *
 * An integer with a type tag has none of those problems. It is copyable,
 * comparable, storable in a POD, meaningless to anyone but the device that
 * issued it, and — the part that matters for a build that has to compile
 * without a given backend present — it costs no include.
 *
 * ==================== WHY THEY ARE NOT INTERCHANGEABLE ====================
 *
 * Every handle is a distinct type over the same integer. A TextureHandle will
 * not silently pass where a BufferHandle is wanted, which is not a theoretical
 * nicety: on every backend these are all "an unsigned int" underneath, so a
 * transposed pair of arguments is accepted by the compiler, accepted by the
 * driver, and shows up as a black screen. The tag costs nothing at runtime —
 * the struct is one integer and passes in a register — and it converts that
 * whole class of bug into a compile error.
 *
 * This is the same reasoning math/Mask.hpp already applies to layer ids, and
 * it is applied here for the same reason.
 *
 * ========================== ZERO IS ALWAYS NULL ===========================
 *
 * A default-constructed handle is invalid, on every backend, forever. Backends
 * that number their objects from zero must add one on the way out and subtract
 * it on the way in. That is one instruction in the one place that knows, and it
 * buys `if (handle)` reading correctly everywhere else — including in a
 * default-initialised struct nobody remembered to fill.
 */
#pragma once

#include <cstdint>

namespace cromwell::rhi {

/* One integer, one tag, no API. `Tag` is only ever an incomplete type — it
 * exists to make two instantiations different, never to be constructed. */
template <typename Tag>
struct Handle {
    uint32_t id = 0;

    constexpr bool valid() const { return id != 0; }
    constexpr explicit operator bool() const { return valid(); }

    friend constexpr bool operator==(Handle a, Handle b) { return a.id == b.id; }
    friend constexpr bool operator!=(Handle a, Handle b) { return a.id != b.id; }
};

struct TextureTag;
struct BufferTag;
struct SamplerTag;
struct ShaderTag;
struct PipelineTag;
struct TargetTag;
struct MeshTag;

/* An image the GPU can sample or render into. Includes cubemaps and arrays —
 * which of those it is was decided by the descriptor that made it, and nothing
 * outside the backend needs to re-ask. */
using TextureHandle = Handle<TextureTag>;

/* A block of GPU memory: vertices, indices, uniforms, or a compute buffer. What
 * it may be used for was fixed at creation (BufferDesc::usage) because Vulkan
 * and the console APIs need to know before they allocate, and a backend that
 * could not be told would have to allocate for every possibility. */
using BufferHandle = Handle<BufferTag>;

/* How a texture is filtered and wrapped. SEPARATE FROM THE TEXTURE, because
 * every modern API separates them and because the same image is genuinely
 * sampled two ways — a shadow map read with a comparison sampler for the test
 * and with a plain one for the debug preview. */
using SamplerHandle = Handle<SamplerTag>;

/* A compiled program. One handle covers a whole vertex/fragment pair or a
 * compute kernel; the stages are a backend concern. */
using ShaderHandle = Handle<ShaderTag>;

/* SHADER PLUS ALL THE FIXED-FUNCTION STATE, baked. This is the type that makes
 * the interface explicit rather than GL-shaped: depth test, blend, cull and
 * vertex layout are decided once, validated once, and bound as a unit.
 *
 * GL lets those be poked one at a time and every other target does not — Metal,
 * Vulkan and the console APIs all want a pipeline object built up front. An
 * interface with loose setters would be a GL state machine those backends have
 * to emulate by tracking every poke and rebuilding a pipeline at draw time,
 * which is both slow and exactly the impedance mismatch this design exists to
 * avoid. */
using PipelineHandle = Handle<PipelineTag>;

/* Somewhere to render: a set of colour attachments and optionally a depth one.
 * The swapchain's backbuffer is a target like any other, and is the one a
 * default-constructed (invalid) handle means when a pass names no target. */
using TargetHandle = Handle<TargetTag>;

/* Geometry ready to draw — vertex and index buffers already associated with the
 * layout they were built for. A convenience over raw buffers, and the level
 * most of the engine actually draws at. */
using MeshHandle = Handle<MeshTag>;

}  // namespace cromwell::rhi
