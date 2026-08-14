/* IRenderDevice.hpp — the graphics API, as cromwell is willing to know it.
 *
 * SINGLE RESPONSIBILITY: create GPU resources, and hand out encoders that
 * record work against them. Every backend implements this and nothing above it
 * names a graphics API.
 *
 * ============================ WHAT THIS REPLACES ==========================
 *
 * A previous policy in cromwell/math/RaylibInterop.hpp said the opposite: that
 * resource handles were raylib's and should stay raylib's "in interfaces and
 * all", on the argument that swapping backends means reimplementing the passes
 * anyway, so the typedefs save nothing.
 *
 * THE ARGUMENT WAS SOUND AND ANSWERED THE WRONG QUESTION. It is true that the
 * passes are the cost. That is the reason FOR this interface, not against it:
 * with the passes written against a device, two backends COEXIST and are chosen
 * at startup. With them written against rlgl, adding a second backend means
 * rewriting the first one in place, and there is no point in the process where
 * the tree builds and runs on both. The first arrangement is a port; the second
 * is a rolling outage.
 *
 * It also assumed one target. cromwell ships to Windows and Linux (GL 4.3),
 * macOS — where GL is deprecated, capped at 4.1, and has none of the compute
 * cromwell/gpu/compute already uses — and consoles, whose APIs are explicit and
 * whose headers cannot be committed here at all. "Reimplement the passes" is
 * not a one-off cost on that list, it is a cost per platform, forever, and it
 * is paid in the one place a bug is most expensive to find.
 *
 * ========================= WHY IT IS EXPLICIT =============================
 *
 * There is no begin/end state machine here, no "bind this then draw", no loose
 * depth or blend setters. State is baked into a PipelineHandle up front and
 * bound as a unit; attachments and their load/store actions are declared before
 * a pass starts.
 *
 * That shape is free on GL and mandatory on the other three. The inverse — an
 * immediate-mode interface — is free on GL and has to be EMULATED on the other
 * three, by recording pokes and building pipelines lazily at draw time. That
 * emulation is slow, and worse, it moves every validation failure from load
 * time with a resource name to draw time with a black screen.
 *
 * ============================ THREADING ===================================
 *
 * The device is not thread-safe and does not pretend to be. Resource creation
 * happens on the thread that created the device. Encoders are the unit that
 * could later be recorded in parallel — that is why they are separate objects
 * rather than methods on the device — but nothing does that yet and no backend
 * is required to support it until something does.
 */
#pragma once

#include "cromwell/rhi/Descriptors.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell::rhi {

class ICommandEncoder;

/* WHAT THIS MACHINE CAN ACTUALLY DO, asked once at startup rather than assumed.
 *
 * The engine has genuine reasons to branch on these: compute is used by
 * cromwell/gpu/compute and does not exist on macOS's GL, and a project that
 * silently produced a black screen rather than saying "this build needs
 * compute" would be a support ticket nobody can answer remotely. Ask, then
 * either degrade the feature or refuse to start with a sentence a player can
 * read. */
struct DeviceCapabilities {
    bool compute            = false;
    bool geometryShaders    = false;
    bool cubeArrays         = false;   /* the reflection probe set wants these */
    bool anisotropicFilter  = false;
    bool depthClamp         = false;

    uint32_t maxTextureSize     = 0;
    uint32_t maxColourTargets   = 1;
    uint32_t maxAnisotropy      = 1;
    uint32_t uniformBufferAlign = 256;

    /* For the log line and the bug report — "GL 4.3 / NVIDIA RTX 4070". */
    const char* backendName = "";
    const char* deviceName  = "";
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual const DeviceCapabilities& capabilities() const = 0;

    /* ---- resources ------------------------------------------------------
     *
     * ALL RETURN AN INVALID HANDLE ON FAILURE rather than throwing or
     * aborting. A texture that could not be made is an ordinary outcome — the
     * format is unsupported, the memory is gone — and the caller usually has
     * something sensible to do about it, which is what every pass in this
     * engine already does when its shader fails to load. The backend logs the
     * reason; the handle carries the fact. */
    virtual TextureHandle  createTexture(const TextureDesc& desc) = 0;
    virtual SamplerHandle  createSampler(const SamplerDesc& desc) = 0;
    virtual BufferHandle   createBuffer(const BufferDesc& desc) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;

    /* Source is the engine's own shader dialect, translated by the backend.
     * TAKING SOURCE RATHER THAN A FILE PATH keeps the device out of the asset
     * system: what a shader is called and where it lives is a project
     * question, and the console backends want precompiled bytecode fed in the
     * same way anyway. */
    virtual ShaderHandle createShader(const char* name,
                                      const char* vertexSource,
                                      const char* fragmentSource) = 0;
    virtual ShaderHandle createComputeShader(const char* name, const char* source) = 0;

    /* INDEXED OR NOT — `indices` may be an invalid handle, and a great deal of
     * real geometry is non-indexed. This engine's static world is built as
     * triangle soup by the box emitter, so requiring an index buffer would have
     * meant generating 0,1,2,3… for every mesh: a second buffer the size of the
     * first, uploaded and bound, describing nothing.
     *
     * `vertexCount` is always required. `indexCount` is read only when there is
     * an index buffer, which is why the two are separate parameters rather than
     * one "count" whose meaning depends on another argument. */
    virtual MeshHandle createMesh(const VertexLayout& layout,
                                  BufferHandle vertices,
                                  uint32_t vertexCount,
                                  BufferHandle indices = {},
                                  uint32_t indexCount = 0) = 0;

    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(SamplerHandle handle) = 0;
    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(MeshHandle handle) = 0;

    /* ---- uploads --------------------------------------------------------*/

    virtual void updateBuffer(BufferHandle buffer, const void* data,
                              uint64_t bytes, uint64_t offset = 0) = 0;

    virtual void updateTexture(TextureHandle texture, const void* pixels,
                               uint32_t layer = 0, uint32_t mip = 0) = 0;

    virtual void generateMips(TextureHandle texture) = 0;

    /* A REGION OF THE BACKBUFFER, INTO A TEXTURE — what has actually been drawn
     * so far, copied to `destination`'s origin. False when the copy could not
     * be made.
     *
     * NOT readTexture. That one goes to the CPU and stalls the pipeline; this
     * stays on the device and does not. They read the same pixels and are
     * otherwise nothing alike, which is why they are not one call with a flag.
     *
     * ============== BETWEEN PASSES, AND THAT IS THE WHOLE DESIGN ============
     *
     * It is a device call rather than an encoder one, and calling it inside a
     * pass is an error. GL would not care — a copy from the bound framebuffer is
     * legal wherever you like — but Vulkan forbids an image copy inside a render
     * pass outright, and Metal needs a blit encoder, which is a different
     * encoder. An interface that allowed it here would be one that cannot be
     * implemented on three of four targets without secretly splitting the pass
     * behind the caller's back.
     *
     * So the caller splits it, visibly, and pays what it costs. On a tiler that
     * cost is real — a split stores and reloads the attachment — and a UI with
     * four frosted panels should be able to see in its own code that it asked
     * for four of them.
     *
     * `x` and `y` have their ORIGIN AT THE BOTTOM LEFT, matching setScissor and
     * every backend's copy rectangle. A UI measuring from the top flips on the
     * way in; doing that here instead would make this the one call in the
     * interface with its own convention. */
    virtual bool copyBackbufferToTexture(TextureHandle destination,
                                         uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height) = 0;

    /* ---- the frame ------------------------------------------------------
     *
     * beginPass HANDS BACK AN ENCODER, valid until the matching endPass. The
     * two are separate calls rather than an RAII scope in this interface
     * because a backend may want to hold state across the pair; the engine-side
     * wrapper that most call sites use puts a scope object over the top. */
    virtual ICommandEncoder& beginPass(const PassDesc& desc) = 0;
    virtual void             endPass(ICommandEncoder& encoder) = 0;

    /* Compute is outside a render pass on every backend. Returns null when
     * capabilities().compute is false, rather than a null-object that silently
     * does nothing — a compute dispatch that quietly no-ops is a wrong picture
     * with no error attached to it. */
    virtual ICommandEncoder* beginCompute(const char* name) = 0;

    /* Submit everything recorded and present. Blocks only as far as the
     * swapchain requires. */
    virtual void present() = 0;

    /* PIXELS BACK FROM THE GPU, as RGBA8, tightly packed, appended to `out`.
     * False when the texture cannot be read.
     *
     * THIS STALLS, AND THAT IS THE ENTIRE CHARACTER OF THE CALL. It blocks
     * until the GPU has finished everything queued, which on a console or a
     * tiler means flushing work that had not been submitted yet. It has no
     * place inside a frame and there is no way to make it cheap.
     *
     * It is on the interface anyway because two things genuinely need it and
     * neither is optional: a screenshot, and the conformance self-test — which
     * is the only way to prove that a pass wrote what it was told to rather
     * than merely running without error. A backend that could not be asked
     * this could not be verified at all. */
    /* `layer` names the array slice or cube face, and defaults to the only one
     * a plain 2D texture has. It is not optional generality: a cubemap array is
     * the one resource in this engine whose CONTENTS cannot be inferred from
     * the frame — a capture that silently wrote nothing looks identical to one
     * that wrote sky, because the sampler blends to the analytic sky exactly
     * where the capture found nothing. Without this parameter the only way to
     * ask "did the probe pass write anything at all" is to look at a reflection
     * and guess. */
    virtual bool readTexture(TextureHandle texture, uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t>& out, uint32_t layer = 0) = 0;

    /* ---- the backbuffer's size ------------------------------------------
     *
     * TOLD, NOT DISCOVERED. The device does not own the window and cannot ask
     * it anything — that is the whole point of the platform layer — so the size
     * arrives from whoever does, once at startup and again on every resize.
     * IPlatform::beginFrame is where that happens.
     *
     * WHY THIS IS A SETTER AND NOT A QUERY INTO GL. The GL backend used to
     * answer backbufferSize() with glGetIntegerv(GL_VIEWPORT), which is not the
     * backbuffer's size — it is whatever the LAST PASS set. Every screen-target
     * pass therefore inherited the previous pass's viewport, and since the pass
     * before the resolve renders into a supersampled scene target, the screen
     * was being drawn with a viewport twice its width and height.
     *
     * The tone map survived that by accident: it addresses by gl_FragCoord and
     * divides by the backbuffer size, so an oversized viewport merely overhangs
     * and the arithmetic cancels. Anything with real vertex positions does not
     * cancel — the UI came out at double scale with three quarters of it off
     * screen, which is how this was found.
     *
     * A backend with no such notion (an offscreen device, a console rendering
     * into a swap chain it is handed) can ignore the setter and answer from
     * whatever it does have. */
    virtual void setBackbufferSize(uint32_t width, uint32_t height) = 0;

    /* The backbuffer's current size in pixels, which is not always the window's
     * — a high-DPI display and a resize in flight both separate them. */
    virtual void backbufferSize(uint32_t& width, uint32_t& height) const = 0;
};

/* ONE PASS'S WORTH OF RECORDED WORK.
 *
 * A SEPARATE OBJECT FROM THE DEVICE, and that is the piece of foresight worth
 * paying for now: on Vulkan and the console APIs this is a command buffer, and
 * command buffers are the unit that can be recorded on several threads at once.
 * Nothing here does that today. Making it a method on the device instead would
 * mean the interface has to change on the day something wants to. */
class ICommandEncoder {
public:
    virtual ~ICommandEncoder() = default;

    /* Everything fixed-function, as one already-validated object. */
    virtual void bindPipeline(PipelineHandle pipeline) = 0;

    /* SLOT-NUMBERED, NOT NAME-LOOKED-UP. A uniform found by string at draw time
     * is a GL and D3D reflection habit that the explicit backends do not have;
     * the shader's declared binding index is the contract, and looking one up
     * per draw is a hash lookup inside the hottest loop the renderer has. */
    virtual void bindUniformBuffer(uint32_t slot, BufferHandle buffer,
                                   uint64_t offset = 0, uint64_t bytes = 0) = 0;
    virtual void bindStorageBuffer(uint32_t slot, BufferHandle buffer) = 0;
    virtual void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) = 0;
    virtual void bindStorageTexture(uint32_t slot, TextureHandle texture, uint32_t mip = 0) = 0;

    /* SMALL, IMMEDIATE CONSTANTS — a transform, an object id, a debug flag.
     * Push constants on Vulkan, set-bytes on Metal, a reserved uniform block on
     * GL. Capped small deliberately: every backend guarantees at least 128
     * bytes and none guarantees much more, so anything larger belongs in a
     * uniform buffer where the caller can see it is paying for one. */
    static constexpr uint32_t kMaxPushConstantBytes = 128;
    virtual void pushConstants(const void* data, uint32_t bytes) = 0;

    virtual void setViewport(float x, float y, float width, float height) = 0;
    virtual void setScissor(float x, float y, float width, float height) = 0;

    /* Live because it genuinely varies per draw within one pipeline — the
     * custom-depth pass writes a different id per object and nothing else about
     * its state changes. Anything that varies per PASS belongs in the pipeline
     * instead. */
    virtual void setStencilReference(uint32_t value) = 0;

    /* `vertexCount` 0 means the whole mesh, which is what almost every pass
     * wants and what this took exclusively until debug lines arrived.
     *
     * THE RANGE IS HERE BECAUSE drawIndexed ALREADY HAD ONE. A caller with two
     * runs of vertices in one buffer — the debug pass has exactly that, its
     * depth-tested segments and then its x-ray ones — could otherwise draw them
     * only by keeping two buffers, or by inventing an index buffer that says
     * 0,1,2,3… and describes nothing. Every backend takes a first and a count
     * on a non-indexed draw; there was no reason for this one not to. */
    virtual void draw(MeshHandle mesh, uint32_t vertexCount = 0,
                      uint32_t firstVertex = 0, uint32_t instances = 1) = 0;
    virtual void drawIndexed(MeshHandle mesh, uint32_t indexCount,
                             uint32_t firstIndex = 0, uint32_t instances = 1) = 0;

    /* No mesh, no vertex buffer — the vertex shader synthesises its positions
     * from gl_VertexIndex. Every full-screen pass in this engine is one of
     * these, and going through a quad mesh for them is a buffer bind and three
     * wasted vertices per pass for nothing. */
    virtual void drawFullscreen() = 0;

    /* Compute encoders only; a no-op with a logged complaint on a render one. */
    virtual void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

    /* Named regions for RenderDoc, Nsight, PIX and the console tools. The GPU
     * profiler zones in cromwell/gpu/GpuProfiler.hpp ride on these, which is
     * why they are on the interface rather than being a backend's private
     * business — a capture with no markers is the thing that makes a frame
     * unreadable, and CLAUDE.md is emphatic about unlabelled work. */
    virtual void pushDebugGroup(const char* name) = 0;
    virtual void popDebugGroup() = 0;
};

}  // namespace cromwell::rhi
