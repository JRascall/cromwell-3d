/* RhiTests.cpp — the backend interface, checked by implementing it.
 *
 * WHAT IS WORTH PINNING DOWN HERE, and it is not much about behaviour: this
 * file's job is almost entirely done at COMPILE time, and the runtime checks
 * below are the small remainder.
 *
 * ================= THE TWO THINGS THIS FILE ACTUALLY PROVES ================
 *
 * 1. THE INTERFACE IS IMPLEMENTABLE. NullDevice and NullEncoder override every
 *    pure virtual in IRenderDevice and ICommandEncoder. A method added to
 *    either without a thought for what a backend would do with it stops this
 *    file compiling, on the machine of whoever added it — which is the whole
 *    point of writing an interface a console team will have to satisfy under
 *    NDA, months from now, with no way to ask.
 *
 * 2. THE HEADERS ARE FREE OF ANY GRAPHICS API. This target links cromwell_base,
 *    which does not link raylib and never sees its headers. So `#include
 *    "cromwell/rhi/IRenderDevice.hpp"` compiling here IS the check that nothing
 *    in the RHI has quietly acquired a raylib, GL or platform dependency. That
 *    is the rule cromwell/rhi/IRenderDevice.hpp argues for at length, and this
 *    is the thing that enforces it rather than trusting it — the same
 *    arrangement CMakeLists uses for the engine's one architectural rule.
 *
 * If either of those breaks, it breaks here, at build time, in every
 * configuration — not on the one platform whose backend was being edited.
 *
 * ================== WHAT IS DELIBERATELY NOT TESTED HERE ==================
 *
 * Anything that needs a GPU. There is no device to make, no pixels to compare,
 * and a test that mocked one would be checking the mock. Correctness of a
 * backend's translation — that LoadAction::Clear really clears — belongs in a
 * per-backend test on hardware, and the honest place for it is a golden-image
 * suite, not this file.
 */
#include "cromwell/rhi/Descriptors.hpp"
#include "cromwell/rhi/Formats.hpp"
#include "cromwell/rhi/Handles.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include <cstdint>
#include <cstdio>
#include <type_traits>

using namespace cromwell::rhi;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* ---- compile-time: the handle contract ---------------------------------
 *
 * These are the properties every backend is entitled to rely on, so they are
 * asserted rather than tested — a violation should not reach a test runner. */

static_assert(sizeof(TextureHandle) == sizeof(uint32_t),
              "a handle is one integer: it is passed by value everywhere and "
              "stored in per-draw structs, so it must not grow");

static_assert(!TextureHandle{}.valid(),
              "a default-constructed handle is null on every backend - this is "
              "what makes an unfilled field in a descriptor safe");

static_assert(TextureHandle{ 1 }.valid(), "one is a live handle, not null");

static_assert(isDepthFormat(TextureFormat::D32F), "");
static_assert(isDepthFormat(TextureFormat::D24S8), "");
static_assert(!isDepthFormat(TextureFormat::RGBA16F), "");

/* THE TAGS MAKE THE TYPES DISTINCT, which is the property that turns a
 * transposed pair of arguments from a black screen into a compile error. If
 * this ever passes, every handle has collapsed to the same type and the
 * protection is gone. */
static_assert(!std::is_same_v<TextureHandle, BufferHandle>,
              "handles of different kinds must not be interchangeable");
static_assert(!std::is_same_v<ShaderHandle, PipelineHandle>,
              "a shader is not a pipeline: the whole point of PipelineHandle is "
              "that it carries the baked fixed-function state too");

/* ---- a backend, doing nothing ------------------------------------------
 *
 * Records what it was asked for so the ordering checks below have something to
 * look at, and otherwise implements the interface as cheaply as possible. */

class NullEncoder final : public ICommandEncoder {
public:
    void bindPipeline(PipelineHandle pipeline) override { lastPipeline = pipeline; }
    void bindUniformBuffer(uint32_t, BufferHandle, uint64_t, uint64_t) override {}
    void bindStorageBuffer(uint32_t, BufferHandle) override {}
    void bindTexture(uint32_t slot, TextureHandle, SamplerHandle) override { lastSlot = slot; }
    void bindStorageTexture(uint32_t, TextureHandle, uint32_t) override {}
    void pushConstants(const void*, uint32_t bytes) override { pushedBytes = bytes; }
    void setViewport(float, float, float, float) override {}
    void setScissor(float, float, float, float) override {}
    void setStencilReference(uint32_t value) override { stencil = value; }
    void draw(MeshHandle, uint32_t) override { draws++; }
    void drawIndexed(MeshHandle, uint32_t, uint32_t, uint32_t) override { draws++; }
    void drawFullscreen() override { draws++; }
    void dispatch(uint32_t, uint32_t, uint32_t) override { dispatches++; }
    void pushDebugGroup(const char*) override { groupDepth++; }
    void popDebugGroup() override { groupDepth--; }

    PipelineHandle lastPipeline;
    uint32_t lastSlot = 0;
    uint32_t pushedBytes = 0;
    uint32_t stencil = 0;
    int draws = 0;
    int dispatches = 0;
    int groupDepth = 0;
};

class NullDevice final : public IRenderDevice {
public:
    NullDevice()
    {
        caps_.backendName = "null";
        caps_.deviceName  = "test";
        caps_.compute     = false;   /* deliberately: see the check below */
    }

    const DeviceCapabilities& capabilities() const override { return caps_; }

    TextureHandle  createTexture(const TextureDesc&) override  { return { next_++ }; }
    SamplerHandle  createSampler(const SamplerDesc&) override  { return { next_++ }; }
    BufferHandle   createBuffer(const BufferDesc&) override    { return { next_++ }; }
    PipelineHandle createPipeline(const PipelineDesc&) override { return { next_++ }; }

    ShaderHandle createShader(const char*, const char*, const char*) override
    {
        return { next_++ };
    }
    ShaderHandle createComputeShader(const char*, const char*) override { return { next_++ }; }

    MeshHandle createMesh(const VertexLayout&, BufferHandle, uint32_t,
                          BufferHandle, uint32_t) override
    {
        return { next_++ };
    }

    void destroy(TextureHandle) override  { destroyed_++; }
    void destroy(SamplerHandle) override  { destroyed_++; }
    void destroy(BufferHandle) override   { destroyed_++; }
    void destroy(PipelineHandle) override { destroyed_++; }
    void destroy(ShaderHandle) override   { destroyed_++; }
    void destroy(MeshHandle) override     { destroyed_++; }

    void updateBuffer(BufferHandle, const void*, uint64_t, uint64_t) override {}
    void updateTexture(TextureHandle, const void*, uint32_t, uint32_t) override {}
    void generateMips(TextureHandle) override {}

    ICommandEncoder& beginPass(const PassDesc& desc) override
    {
        passes++;
        lastPassName = desc.name;
        return encoder;
    }
    void endPass(ICommandEncoder&) override { closed++; }

    /* NO COMPUTE ON THIS DEVICE, and it says so by handing back null rather
     * than an encoder that ignores dispatches. macOS's GL is exactly this case
     * and a silent no-op there is a wrong picture with no error attached. */
    ICommandEncoder* beginCompute(const char*) override
    {
        return caps_.compute ? &encoder : nullptr;
    }

    void present() override { presents++; }

    /* A backend that cannot be read back cannot be verified — see
     * IRenderDevice::readTexture. This one refuses, which is the honest answer
     * for a device with no pixels. */
    bool readTexture(TextureHandle, uint32_t, uint32_t, uint32_t, uint32_t,
                     std::vector<uint8_t>&) override
    {
        return false;
    }

    /* REMEMBERED, not ignored — so the round trip is what a real backend does.
     * The interface's contract is that the platform TELLS the device its screen
     * size and the device hands back what it was told; a stub that returned a
     * constant regardless would pass while a backend that dropped the setter
     * failed in the frame. */
    void setBackbufferSize(uint32_t width, uint32_t height) override
    {
        backbufferWidth = width;
        backbufferHeight = height;
    }

    void backbufferSize(uint32_t& width, uint32_t& height) const override
    {
        width  = backbufferWidth;
        height = backbufferHeight;
    }

    uint32_t backbufferWidth = 1920;
    uint32_t backbufferHeight = 1080;

    NullEncoder encoder;
    int passes = 0;
    int closed = 0;
    int presents = 0;
    const char* lastPassName = nullptr;

    int destroyedCount() const { return destroyed_; }

private:
    DeviceCapabilities caps_;
    uint32_t next_ = 1;   /* zero stays reserved for "no resource" */
    int destroyed_ = 0;
};

/* ---- the checks ---------------------------------------------------------*/

void handlesAreNeverZero()
{
    NullDevice device;

    /* A BACKEND MUST NOT ISSUE ZERO, whatever its underlying API numbers from.
     * GL names objects from zero and a backend forwarding those raw would hand
     * out a handle that reads as null at every `if (handle)` in the engine —
     * the resource exists, and every guard says it does not. */
    const TextureHandle texture = device.createTexture(TextureDesc{ .name = "scene" });
    const BufferHandle  buffer  = device.createBuffer(BufferDesc{ .name = "camera" });

    CHECK(texture.valid(), "a created texture has a live handle");
    CHECK(buffer.valid(), "a created buffer has a live handle");
    CHECK(texture.id != buffer.id, "distinct resources get distinct ids");
}

void descriptorsDefaultToSomethingSafe()
{
    /* THE DEFAULTS MATTER MORE THAN THEY LOOK. A descriptor is filled field by
     * field at a call site and the ones nobody sets are the ones that bite, so
     * the unset state has to be the conservative answer rather than the fast
     * one. */
    const TextureDesc texture;
    CHECK(texture.layers == 1, "a texture is one layer unless asked otherwise");
    CHECK(texture.mipLevels == 1, "no mips unless asked - they are not free");
    CHECK(!texture.cube, "a cubemap is a deliberate choice");
    CHECK(texture.usage == TextureUsageSampled, "sampled is the harmless default");

    const DepthState depth;
    CHECK(depth.test && depth.write, "depth on by default: a pass that wants it off says so");

    const BlendState blend;
    CHECK(!blend.enabled, "blending off by default - opaque is the common case");

    const RasterState raster;
    CHECK(raster.cull == CullMode::Back, "back faces culled unless a pass says otherwise");

    /* Premultiplied is the one several passes want and the one that is easy to
     * assemble wrongly by hand, which is why it is a named constructor. */
    const BlendState glass = BlendState::premultiplied();
    CHECK(glass.enabled, "premultiplied blending is enabled");
    CHECK(glass.sourceColour == BlendFactor::One,
          "premultiplied ADDS the source: the shader already scaled it by coverage");
    CHECK(glass.destColour == BlendFactor::OneMinusSrcAlpha, "and attenuates the destination");
}

void passDescriptorsCarryTheirActions()
{
    /* The load/store actions are the tile-based targets' whole reason for
     * caring about this type, so the defaults are checked explicitly: a pass
     * that forgets to state them should get the SAFE answer (keep the result),
     * not the fast one. */
    const ColourAttachment colour;
    CHECK(colour.load == LoadAction::Clear, "an unstated colour attachment clears");
    CHECK(colour.store == StoreAction::Store, "and keeps what it drew");

    const DepthAttachment depth;
    CHECK(depth.store == StoreAction::Store, "depth is kept unless a pass discards it");
    CHECK(depth.stencilStore == StoreAction::Discard,
          "stencil is NOT kept by default - most passes have no stencil to keep");

    const PassDesc pass;
    CHECK(pass.colourCount == 0 && !pass.hasDepth,
          "an empty pass targets the backbuffer, which is what no attachments means");
}

void encodingRunsThroughTheDevice()
{
    NullDevice device;

    PassDesc desc{ .name = "shadow map" };
    desc.hasDepth = true;
    desc.depth.load  = LoadAction::Clear;
    desc.depth.store = StoreAction::Store;

    ICommandEncoder& encoder = device.beginPass(desc);
    encoder.pushDebugGroup("casters");
    encoder.bindPipeline(PipelineHandle{ 42 });
    encoder.setStencilReference(7);
    encoder.drawFullscreen();
    encoder.popDebugGroup();
    device.endPass(encoder);

    CHECK(device.passes == 1 && device.closed == 1, "the pass opened and closed once each");
    CHECK(device.lastPassName != nullptr, "a pass carries its name for the profiler and captures");
    CHECK(device.encoder.draws == 1, "the draw reached the encoder");
    CHECK(device.encoder.lastPipeline == PipelineHandle{ 42 }, "the pipeline bound");
    CHECK(device.encoder.stencil == 7, "the stencil reference is per draw, not per pipeline");
    CHECK(device.encoder.groupDepth == 0, "debug groups are balanced");
}

void missingComputeIsVisible()
{
    NullDevice device;

    /* THE macOS CASE, and the reason this returns a pointer rather than a
     * reference. GL there is capped at 4.1 with no compute at all, so a caller
     * has to be able to find out — silently doing nothing would be a wrong
     * picture with no error attached to it. */
    CHECK(!device.capabilities().compute, "this device reports no compute");
    CHECK(device.beginCompute("occlusion") == nullptr,
          "a device without compute refuses the encoder rather than no-oping");
}

}  // namespace

int main()
{
    handlesAreNeverZero();
    descriptorsDefaultToSomethingSafe();
    passDescriptorsCarryTheirActions();
    encodingRunsThroughTheDevice();
    missingComputeIsVisible();

    if (g_failures == 0) {
        std::printf("rhi: all checks passed\n");
        return 0;
    }
    std::printf("rhi: %d check(s) failed\n", g_failures);
    return 1;
}
