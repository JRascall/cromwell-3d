/* OpenGlRenderDevice.hpp — IRenderDevice on OpenGL 4.3.
 *
 * SINGLE RESPONSIBILITY: own every GPU object the engine has asked for, and
 * execute the recorded passes against a GL context.
 *
 * ============== WHY THIS IS "opengl" AND NOT "raylib" =====================
 *
 * rhi/pc/opengl/ — the same two axes the platform layer uses, platform first
 * and library second, and here the library is OpenGL rather than raylib.
 *
 * That is not a technicality. Almost nothing in this file is raylib's: the
 * window and its context are (platform/pc/raylib/'s job), but every call below
 * is GL through the glad loader raylib happens to have already resolved.
 * Replacing raylib with a hand-rolled window layer would leave this file
 * untouched; replacing OpenGL with Vulkan would replace all of it. The folder
 * names the thing that would actually change.
 *
 * A Vulkan backend is rhi/pc/vulkan/ beside this one. A console backend is
 * rhi/ps5/ with its own. Both implement the same interface, and which is
 * compiled is a CMakeLists decision — see the two-axis note there.
 *
 * ==================== HANDLES CARRY A GENERATION ==========================
 *
 * A handle is an index and a generation packed into its 32 bits. The generation
 * is bumped every time a slot is reused, so a handle kept past the destroy()
 * that freed it resolves to nothing rather than to WHATEVER TOOK ITS PLACE.
 *
 * Without that, the failure is a texture that suddenly samples another
 * texture's contents — no GL error, no crash, and a picture that is wrong in a
 * way that looks like a shader bug. It is the single most confusing class of
 * error a handle-based API can produce, and eight bits of generation is a
 * complete fix for it.
 *
 * ==================== STATE IS APPLIED, NOT TRACKED ========================
 *
 * bindPipeline applies the whole state block — program, depth, blend, raster —
 * without comparing against what is already set. That is deliberate for now:
 * redundant-state elimination is a real optimisation, and it is also the most
 * common source of "the frame is wrong once every few hundred frames" bugs when
 * the tracked shadow state drifts from the driver's.
 *
 * Passes are counted in the dozens here, not the thousands, so the win would be
 * unmeasurable and the risk is not. If a profile ever says otherwise, the place
 * to add it is here, behind a measurement — see the order-of-attack rule in
 * CLAUDE.md.
 */
#pragma once

#include "cromwell/rhi/IRenderDevice.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cromwell::rhi {

class OpenGlRenderDevice final : public IRenderDevice {
public:
    /* HOW TO FIND A GL FUNCTION THE LOADER DID NOT RESOLVE.
     *
     * The context is created by the windowing layer, and so is the only thing
     * that knows how to ask for an entry point — wglGetProcAddress on Windows,
     * glXGetProcAddress on X11, and whatever GLFW or SDL wraps those in. Taking
     * it as an argument is what keeps this file from having to name any of
     * them, which is the same bargain every other interface in the port makes.
     *
     * It is needed because the glad the window backend loaded stops at GL 4.3,
     * and one function this device cannot work without is 4.5 — see
     * `clipControl` in the .cpp. */
    using ProcAddress = void (*)();
    using ProcLoader  = ProcAddress (*)(const char* name);

    /* NEEDS A LIVE GL CONTEXT. Returns null when the context is missing or does
     * not meet the feature level, having logged which. */
    static std::unique_ptr<OpenGlRenderDevice> create(ProcLoader loadProc, bool debugContext);

    ~OpenGlRenderDevice() override;

    OpenGlRenderDevice(const OpenGlRenderDevice&) = delete;
    OpenGlRenderDevice& operator=(const OpenGlRenderDevice&) = delete;

    const DeviceCapabilities& capabilities() const override { return capabilities_; }

    TextureHandle  createTexture(const TextureDesc& desc) override;
    SamplerHandle  createSampler(const SamplerDesc& desc) override;
    BufferHandle   createBuffer(const BufferDesc& desc) override;
    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    ShaderHandle   createShader(const char* name, const char* vertexSource,
                                const char* fragmentSource) override;
    ShaderHandle   createComputeShader(const char* name, const char* source) override;
    MeshHandle     createMesh(const VertexLayout& layout, BufferHandle vertices,
                              uint32_t vertexCount, BufferHandle indices,
                              uint32_t indexCount) override;

    void destroy(TextureHandle handle) override;
    void destroy(SamplerHandle handle) override;
    void destroy(BufferHandle handle) override;
    void destroy(PipelineHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(MeshHandle handle) override;

    void updateBuffer(BufferHandle buffer, const void* data,
                      uint64_t bytes, uint64_t offset) override;
    void updateTexture(TextureHandle texture, const void* pixels,
                       uint32_t layer, uint32_t mip) override;
    void generateMips(TextureHandle texture) override;
    bool copyBackbufferToTexture(TextureHandle destination, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) override;

    ICommandEncoder& beginPass(const PassDesc& desc) override;
    void             endPass(ICommandEncoder& encoder) override;
    ICommandEncoder* beginCompute(const char* name) override;

    void present() override;
    void setBackbufferSize(uint32_t width, uint32_t height) override;
    void backbufferSize(uint32_t& width, uint32_t& height) const override;

    /* Implemented with glReadPixels, which stalls until the GPU has caught up.
     * That is inherent rather than a shortcoming of this backend — see the note
     * on IRenderDevice::readTexture for why it is on the interface at all. */
    bool readTexture(TextureHandle texture, uint32_t x, uint32_t y,
                     uint32_t width, uint32_t height, std::vector<uint8_t>& out,
                     uint32_t layer = 0) override;

private:
    OpenGlRenderDevice() = default;

    /* ---- the pools ------------------------------------------------------*/

    struct Texture {
        uint32_t      name = 0;     /* the GL object */
        uint32_t      target = 0;   /* GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, ... */
        TextureFormat format = TextureFormat::Unknown;
        uint32_t      width = 0, height = 0, layers = 1, mips = 1;
    };

    struct Buffer {
        uint32_t name = 0;
        uint64_t bytes = 0;
        uint32_t usage = 0;
    };

    struct Shader {
        uint32_t program = 0;
        bool     compute = false;
    };

    struct Pipeline {
        ShaderHandle shader;
        DepthState   depth;
        BlendState   blend;
        RasterState  raster;
        VertexLayout layout;
    };

    struct Mesh {
        uint32_t     vao = 0;
        BufferHandle vertices;
        BufferHandle indices;
        uint32_t     vertexCount = 0;
        uint32_t     indexCount = 0;
        bool         indexed = false;
        VertexLayout layout;
    };

    struct Sampler {
        uint32_t name = 0;
    };

    /* A slot table with generations — see the header note on why. */
    template <typename T>
    struct Pool {
        struct Slot {
            T        value{};
            uint8_t  generation = 1;   /* never 0, so a handle is never null by accident */
            bool     live = false;
        };
        std::vector<Slot>     slots;
        std::vector<uint32_t> free;
    };

    Pool<Texture>  textures_;
    Pool<Buffer>   buffers_;
    Pool<Shader>   shaders_;
    Pool<Pipeline> pipelines_;
    Pool<Mesh>     meshes_;
    Pool<Sampler>  samplers_;

    /* ---- pool access ----------------------------------------------------
     *
     * Defined in the .cpp alongside the handle packing they depend on. The
     * resolve overloads return null for a handle that is stale, freed or never
     * valid — every caller checks, because "the resource is gone" is an
     * ordinary outcome of a hot reload or a resize, not an assertion. */
    /* One shape, six instantiations. Members rather than free functions because
     * Pool and the resource structs are private — which they should stay, since
     * nothing outside this class has any business holding a raw GL name. */
    template <typename T> static T*       resolveIn(Pool<T>& pool, uint32_t id);
    template <typename T> static uint32_t allocateIn(Pool<T>& pool, const T& value);
    template <typename T> static bool     releaseIn(Pool<T>& pool, uint32_t id);

    Texture*  resolve(TextureHandle handle);
    Buffer*   resolve(BufferHandle handle);
    Shader*   resolve(ShaderHandle handle);
    Pipeline* resolve(PipelineHandle handle);
    Mesh*     resolve(MeshHandle handle);
    Sampler*  resolve(SamplerHandle handle);

    /* The Encoder reaches these; it is the device's own inner class and exists
     * only to execute against it. */
    friend class Encoder;

    /* ---- passes ---------------------------------------------------------
     *
     * FRAMEBUFFERS ARE CACHED BY THEIR ATTACHMENT SET. Creating and destroying
     * an FBO per pass per frame is a driver-side allocation in the hot path,
     * and the same handful of attachment combinations recur every frame. Keyed
     * on the attachments themselves so a changed target gets a different FBO
     * rather than a stale one. */
    std::unordered_map<uint64_t, uint32_t> framebuffers_;

    uint32_t framebufferFor(const PassDesc& desc);

    class Encoder;
    std::unique_ptr<Encoder> encoder_;

    DeviceCapabilities capabilities_;
    std::string        backendName_;
    std::string        deviceName_;

    bool inPass_ = false;

    /* glClipControl, resolved at creation because glad's 4.3 loader does not
     * declare it, and called on every beginPass/endPass because raylib shares
     * this context and wants the opposite convention. Never null past create(),
     * which refuses to return a device without it — see the note beside
     * resolveClipControl in the .cpp, which is the one worth reading before
     * touching any of this.
     *
     * HELD AS THE UNTYPED ProcAddress and cast at the call site, NOT as a
     * typed pointer here. A GL entry point is __stdcall on 32-bit Windows and
     * cdecl everywhere else, and glad's GLAD_API_PTR is what resolves which —
     * a macro this header cannot see without pulling glad into every consumer.
     * Spelling the signature out without it would compile silently (x64 has one
     * convention, so the mismatch is invisible here) and corrupt the stack on a
     * 32-bit build. Untyped storage plus one cast in the .cpp, where the macro
     * is available, has no such trapdoor. */
    ProcAddress clipControl_ = nullptr;

    /* THE SCREEN'S SIZE, as the platform last reported it. Zero until told,
     * which is what backbufferSize falls back on. See the interface. */
    uint32_t backbufferWidth_ = 0;
    uint32_t backbufferHeight_ = 0;

    /* TEMPORARY DIAGNOSTIC: the shadow pass's depth attachment, latched in
     * beginPass and dumped in endPass when XC_DUMP_SHADOW is set. Remove with
     * the two blocks in the .cpp that use it. */
    TextureHandle pendingShadowDump_;

    /* A full-screen draw needs a bound vertex array even though it reads no
     * attributes — a core profile refuses to draw without one. One empty VAO,
     * made once. */
    uint32_t emptyVao_ = 0;
};

}  // namespace cromwell::rhi
