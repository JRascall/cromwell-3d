/* OpenGlRenderDevice.cpp — the GL 4.3 backend.
 *
 * ORDERING NOTE: GL.hpp pulls in glad and must precede anything that could
 * bring in a system GL header. See the note at the top of gpu/GL.hpp.
 */
#include "cromwell/gpu/GL.hpp"

#include "cromwell/rhi/pc/opengl/OpenGlRenderDevice.hpp"

#include "cromwell/diag/DepthDump.hpp"
#include "cromwell/diag/Logger.hpp"

#include <cstdlib>
#include <cstring>

namespace cromwell::rhi {
namespace {

/* ---- handle packing -------------------------------------------------------
 *
 * 24 bits of index, 8 of generation, and zero is never a valid handle. The
 * index is stored PLUS ONE so that a zero handle cannot resolve to slot zero —
 * see Handles.hpp, which promises that a default-constructed handle is null on
 * every backend. */
constexpr uint32_t kIndexBits = 24;
constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;

/* raylib's bundled glad declares the entry points but not every extension's
 * tokens, so the two anisotropy constants are spelled out rather than assumed.
 * Same values on every implementation — they are registry-assigned. */
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

/* ASKED OF THE DRIVER, not of glad's loader flags.
 *
 * glad exposes GLAD_GL_<extension> booleans only for the extensions it was
 * generated with, and raylib's copy was generated for raylib's needs rather
 * than this engine's. Querying the string list is a few dozen comparisons once
 * at startup and it cannot go stale when the loader is regenerated. */
bool hasExtension(const char* wanted)
{
    int count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);

    for (int i = 0; i < count; i++) {
        const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        if (name != nullptr && std::strcmp(name, wanted) == 0) return true;
    }
    return false;
}

uint32_t packHandle(uint32_t index, uint8_t generation)
{
    return ((index + 1u) & kIndexMask) | (static_cast<uint32_t>(generation) << kIndexBits);
}

bool unpackHandle(uint32_t id, uint32_t& index, uint8_t& generation)
{
    if (id == 0) return false;
    const uint32_t stored = id & kIndexMask;
    if (stored == 0) return false;

    index      = stored - 1u;
    generation = static_cast<uint8_t>(id >> kIndexBits);
    return true;
}

/* ---- format translation ---------------------------------------------------
 *
 * THE CLOSED SET FROM Formats.hpp, and nothing else. A format the engine does
 * not declare cannot be requested, so the default case here is a bug in this
 * file rather than a caller's problem — it logs and refuses rather than
 * guessing, because a texture created with the wrong internal format renders
 * plausibly wrong rather than failing. */
struct GlFormat {
    uint32_t internalFormat = 0;
    uint32_t layout = 0;   /* GL_RGBA, GL_RED, GL_DEPTH_COMPONENT, ... */
    uint32_t type = 0;
    bool     valid = false;
};

GlFormat translate(TextureFormat format)
{
    switch (format) {
        case TextureFormat::R8:        return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, true };
        case TextureFormat::RG8:       return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE, true };
        case TextureFormat::RGBA8:     return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true };
        case TextureFormat::RGBA8Srgb: return { GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, true };
        case TextureFormat::RGBA16F:   return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, true };
        case TextureFormat::RGBA32F:   return { GL_RGBA32F, GL_RGBA, GL_FLOAT, true };
        case TextureFormat::R16F:      return { GL_R16F, GL_RED, GL_HALF_FLOAT, true };
        case TextureFormat::R32F:      return { GL_R32F, GL_RED, GL_FLOAT, true };
        case TextureFormat::RG16F:     return { GL_RG16F, GL_RG, GL_HALF_FLOAT, true };
        case TextureFormat::R11G11B10F:
            return { GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, true };

        case TextureFormat::D16:
            return { GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, true };
        case TextureFormat::D24S8:
            return { GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, true };
        case TextureFormat::D32F:
            return { GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, true };
        case TextureFormat::D32FS8:
            return { GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL,
                     GL_FLOAT_32_UNSIGNED_INT_24_8_REV, true };

        case TextureFormat::Unknown:
        default:
            return {};
    }
}

uint32_t translate(CompareFunc compare)
{
    switch (compare) {
        case CompareFunc::Never:        return GL_NEVER;
        case CompareFunc::Less:         return GL_LESS;
        case CompareFunc::Equal:        return GL_EQUAL;
        case CompareFunc::LessEqual:    return GL_LEQUAL;
        case CompareFunc::Greater:      return GL_GREATER;
        case CompareFunc::NotEqual:     return GL_NOTEQUAL;
        case CompareFunc::GreaterEqual: return GL_GEQUAL;
        case CompareFunc::Always:
        default:                        return GL_ALWAYS;
    }
}

uint32_t translate(BlendFactor factor)
{
    switch (factor) {
        case BlendFactor::Zero:              return GL_ZERO;
        case BlendFactor::One:               return GL_ONE;
        case BlendFactor::SrcColour:         return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColour: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha:          return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:  return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstColour:         return GL_DST_COLOR;
        case BlendFactor::OneMinusDstColour: return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::DstAlpha:          return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:
        default:                             return GL_ONE_MINUS_DST_ALPHA;
    }
}

uint32_t translate(BlendOp op)
{
    switch (op) {
        case BlendOp::Add:             return GL_FUNC_ADD;
        case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min:             return GL_MIN;
        case BlendOp::Max:
        default:                       return GL_MAX;
    }
}

uint32_t translate(PrimitiveType primitive)
{
    switch (primitive) {
        case PrimitiveType::Triangles:     return GL_TRIANGLES;
        case PrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveType::Lines:         return GL_LINES;
        case PrimitiveType::LineStrip:     return GL_LINE_STRIP;
        case PrimitiveType::Points:
        default:                           return GL_POINTS;
    }
}

uint32_t translate(WrapMode wrap)
{
    switch (wrap) {
        case WrapMode::Repeat:        return GL_REPEAT;
        case WrapMode::ClampToEdge:   return GL_CLAMP_TO_EDGE;
        case WrapMode::ClampToBorder: return GL_CLAMP_TO_BORDER;
        case WrapMode::MirrorRepeat:
        default:                      return GL_MIRRORED_REPEAT;
    }
}

/* Minification takes the mip filter into account; magnification never does,
 * because there is no such thing as magnifying between mip levels. Conflating
 * them is how a texture ends up with GL_LINEAR_MIPMAP_LINEAR magnification,
 * which GL rejects and which silently disables filtering on some drivers. */
uint32_t minifyFilter(FilterMode minify, FilterMode mip, uint32_t mipCount)
{
    if (mipCount <= 1) return minify == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;

    if (minify == FilterMode::Nearest)
        return mip == FilterMode::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;

    return mip == FilterMode::Nearest ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
}

/* Layout attribute -> (component count, GL type, normalised). */
void translate(VertexFormat format, int& components, uint32_t& type, bool& normalised)
{
    normalised = false;
    switch (format) {
        case VertexFormat::Float1: components = 1; type = GL_FLOAT; return;
        case VertexFormat::Float2: components = 2; type = GL_FLOAT; return;
        case VertexFormat::Float3: components = 3; type = GL_FLOAT; return;
        case VertexFormat::Float4: components = 4; type = GL_FLOAT; return;
        case VertexFormat::UByte4: components = 4; type = GL_UNSIGNED_BYTE; return;
        case VertexFormat::UByte4Normalised:
            components = 4; type = GL_UNSIGNED_BYTE; normalised = true; return;
        case VertexFormat::Short2: components = 2; type = GL_SHORT; return;
        case VertexFormat::Short2Normalised:
            components = 2; type = GL_SHORT; normalised = true; return;
        case VertexFormat::Short4: components = 4; type = GL_SHORT; return;
        case VertexFormat::Short4Normalised:
        default:
            components = 4; type = GL_SHORT; normalised = true; return;
    }
}

uint32_t bufferUsageHint(BufferAccess access)
{
    switch (access) {
        case BufferAccess::CpuToGpuPerFrame: return GL_DYNAMIC_DRAW;
        case BufferAccess::CpuToGpuOnce:     return GL_STATIC_DRAW;
        case BufferAccess::GpuOnly:
        default:                             return GL_STATIC_DRAW;
    }
}

bool compileStage(uint32_t stage, const char* source, const char* name, uint32_t& out)
{
    const uint32_t shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        /* THE LOG, NOT JUST "it failed". A shader that will not compile is the
         * most routine failure in a renderer and the driver already knows
         * exactly why; discarding that and printing "compile failed" turns a
         * one-line fix into a bisect. */
        char log[2048] = {};
        int  length = 0;
        glGetShaderInfoLog(shader, static_cast<int>(sizeof log) - 1, &length, log);
        LOGGER.error("shader '{}' failed to compile: {}", name != nullptr ? name : "?", log);

        glDeleteShader(shader);
        return false;
    }

    out = shader;
    return true;
}

}  // namespace

/* ========================================================================== */
/*  The encoder                                                               */
/* ========================================================================== */

/* GL EXECUTES IMMEDIATELY, so this records nothing — every call goes straight
 * to the driver. That is not a shortcut around the interface, it is what the
 * interface allows: ICommandEncoder is shaped so that a Vulkan or console
 * backend CAN record into a command buffer, not so that every backend must.
 *
 * The shape still earns its keep here, because it is what makes the pass's
 * extent explicit — beginPass and endPass bracket the work, so the load and
 * store actions have somewhere to be applied. */
class OpenGlRenderDevice::Encoder final : public ICommandEncoder {
public:
    explicit Encoder(OpenGlRenderDevice& device) : device_(device) {}

    void bindPipeline(PipelineHandle handle) override;
    void bindUniformBuffer(uint32_t slot, BufferHandle buffer,
                           uint64_t offset, uint64_t bytes) override;
    void bindStorageBuffer(uint32_t slot, BufferHandle buffer) override;
    void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
    void bindStorageTexture(uint32_t slot, TextureHandle texture, uint32_t mip) override;
    void pushConstants(const void* data, uint32_t bytes) override;
    void setViewport(float x, float y, float width, float height) override;
    void setScissor(float x, float y, float width, float height) override;
    void setStencilReference(uint32_t value) override;
    void draw(MeshHandle mesh, uint32_t instances) override;
    void drawIndexed(MeshHandle mesh, uint32_t indexCount,
                     uint32_t firstIndex, uint32_t instances) override;
    void drawFullscreen() override;
    void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
    void pushDebugGroup(const char* name) override;
    void popDebugGroup() override;

    void beginCompute() { compute_ = true; }
    void beginRender()  { compute_ = false; }

private:
    OpenGlRenderDevice& device_;
    PrimitiveType   primitive_ = PrimitiveType::Triangles;
    uint32_t        program_ = 0;
    bool            compute_ = false;
    int             debugDepth_ = 0;
};

void OpenGlRenderDevice::Encoder::bindPipeline(PipelineHandle handle)
{
    const Pipeline* pipeline = device_.resolve(handle);
    if (pipeline == nullptr) {
        LOGGER.error("bindPipeline: stale or invalid pipeline handle");
        return;
    }

    const Shader* shader = device_.resolve(pipeline->shader);
    if (shader == nullptr) {
        LOGGER.error("bindPipeline: the pipeline's shader has been destroyed");
        return;
    }

    glUseProgram(shader->program);
    program_   = shader->program;
    primitive_ = pipeline->raster.primitive;

    /* Depth. */
    if (pipeline->depth.test) glEnable(GL_DEPTH_TEST);
    else                      glDisable(GL_DEPTH_TEST);
    glDepthMask(pipeline->depth.write ? GL_TRUE : GL_FALSE);
    glDepthFunc(translate(pipeline->depth.compare));

    if (pipeline->depth.biasConstant != 0.0f || pipeline->depth.biasSlope != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pipeline->depth.biasSlope, pipeline->depth.biasConstant);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    /* Blend. */
    if (pipeline->blend.enabled) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(translate(pipeline->blend.sourceColour),
                            translate(pipeline->blend.destColour),
                            translate(pipeline->blend.sourceAlpha),
                            translate(pipeline->blend.destAlpha));
        glBlendEquationSeparate(translate(pipeline->blend.colourOp),
                                translate(pipeline->blend.alphaOp));
    } else {
        glDisable(GL_BLEND);
    }

    /* Raster. */
    switch (pipeline->raster.cull) {
        case CullMode::None:  glDisable(GL_CULL_FACE); break;
        case CullMode::Front: glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); break;
        case CullMode::Back:  glEnable(GL_CULL_FACE); glCullFace(GL_BACK); break;
    }
    glFrontFace(pipeline->raster.winding == Winding::CounterClockwise ? GL_CCW : GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, pipeline->raster.wireframe ? GL_LINE : GL_FILL);
}

void OpenGlRenderDevice::Encoder::bindUniformBuffer(uint32_t slot, BufferHandle handle,
                                                uint64_t offset, uint64_t bytes)
{
    const Buffer* buffer = device_.resolve(handle);
    if (buffer == nullptr) return;

    if (bytes == 0) glBindBufferBase(GL_UNIFORM_BUFFER, slot, buffer->name);
    else            glBindBufferRange(GL_UNIFORM_BUFFER, slot, buffer->name,
                                      static_cast<GLintptr>(offset),
                                      static_cast<GLsizeiptr>(bytes));
}

void OpenGlRenderDevice::Encoder::bindStorageBuffer(uint32_t slot, BufferHandle handle)
{
    const Buffer* buffer = device_.resolve(handle);
    if (buffer == nullptr) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, buffer->name);
}

void OpenGlRenderDevice::Encoder::bindTexture(uint32_t slot, TextureHandle textureHandle,
                                          SamplerHandle samplerHandle)
{
    const Texture* texture = device_.resolve(textureHandle);
    if (texture == nullptr) return;

    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(texture->target, texture->name);

    /* A SAMPLER OBJECT OVERRIDES the texture's own parameters for this unit,
     * which is exactly why the interface separates them: the same shadow map is
     * read with a comparison sampler by the lit pass and a plain one by the
     * debug preview, and without sampler objects that means mutating the
     * texture between passes. */
    const Sampler* sampler = device_.resolve(samplerHandle);
    glBindSampler(slot, sampler != nullptr ? sampler->name : 0);
}

void OpenGlRenderDevice::Encoder::bindStorageTexture(uint32_t slot, TextureHandle handle, uint32_t mip)
{
    const Texture* texture = device_.resolve(handle);
    if (texture == nullptr) return;

    const GlFormat format = translate(texture->format);
    if (!format.valid) return;

    glBindImageTexture(slot, texture->name, static_cast<int>(mip), GL_FALSE, 0,
                       GL_READ_WRITE, format.internalFormat);
}

void OpenGlRenderDevice::Encoder::pushConstants(const void* data, uint32_t bytes)
{
    if (program_ == 0 || data == nullptr || bytes == 0) return;
    if (bytes > kMaxPushConstantBytes) {
        LOGGER.error("pushConstants: {} bytes exceeds the {} the interface guarantees",
                     bytes, kMaxPushConstantBytes);
        return;
    }

    /* GL HAS NO PUSH CONSTANTS, so they arrive as a reserved uniform block's
     * worth of vec4s at location 0. Vulkan and Metal have the real thing; this
     * is the emulation, and it is cheap because the payload is capped at 128
     * bytes precisely so that it can be. */
    const int vec4Count = static_cast<int>((bytes + 15u) / 16u);
    float padded[kMaxPushConstantBytes / sizeof(float)] = {};
    std::memcpy(padded, data, bytes);
    glUniform4fv(0, vec4Count, padded);
}

void OpenGlRenderDevice::Encoder::setViewport(float x, float y, float width, float height)
{
    glViewport(static_cast<int>(x), static_cast<int>(y),
               static_cast<int>(width), static_cast<int>(height));
}

void OpenGlRenderDevice::Encoder::setScissor(float x, float y, float width, float height)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<int>(x), static_cast<int>(y),
              static_cast<int>(width), static_cast<int>(height));
}

void OpenGlRenderDevice::Encoder::setStencilReference(uint32_t value)
{
    glStencilFunc(GL_ALWAYS, static_cast<int>(value), 0xFF);
}

void OpenGlRenderDevice::Encoder::draw(MeshHandle handle, uint32_t instances)
{
    const Mesh* mesh = device_.resolve(handle);
    if (mesh == nullptr || instances == 0) return;

    /* WHOLE-MESH DRAW, indexed or not. `draw` is the call every pass makes;
     * whether the geometry carries indices was decided at creation and is not
     * something a call site should have to remember. Getting this wrong is a
     * mesh that renders nothing at all — glDrawElements with no element buffer
     * bound reads from address zero and the driver quietly discards it. */
    if (!mesh->indexed) {
        glBindVertexArray(mesh->vao);
        glDrawArraysInstanced(translate(primitive_), 0,
                              static_cast<int>(mesh->vertexCount),
                              static_cast<int>(instances));
        glBindVertexArray(0);
        return;
    }

    drawIndexed(handle, mesh->indexCount, 0, instances);
}

void OpenGlRenderDevice::Encoder::drawIndexed(MeshHandle handle, uint32_t indexCount,
                                          uint32_t firstIndex, uint32_t instances)
{
    const Mesh* mesh = device_.resolve(handle);
    if (mesh == nullptr || indexCount == 0 || instances == 0) return;

    glBindVertexArray(mesh->vao);

    const auto offset = static_cast<uintptr_t>(firstIndex) * sizeof(uint32_t);
    glDrawElementsInstanced(translate(primitive_), static_cast<int>(indexCount),
                            GL_UNSIGNED_INT, reinterpret_cast<const void*>(offset),
                            static_cast<int>(instances));
    glBindVertexArray(0);
}

void OpenGlRenderDevice::Encoder::drawFullscreen()
{
    /* NO VERTEX BUFFER. The vertex shader synthesises a covering triangle from
     * gl_VertexID, which is one draw call and three vertices — going through a
     * quad mesh would mean a buffer bind, four vertices and six indices to
     * cover the same pixels, per full-screen pass, of which this renderer has
     * several.
     *
     * A core profile still refuses to draw with no vertex array bound at all,
     * hence the empty one. */
    glBindVertexArray(device_.emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void OpenGlRenderDevice::Encoder::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    if (!compute_) {
        LOGGER.error("dispatch: this is a render encoder - compute runs outside a render pass");
        return;
    }
    glDispatchCompute(groupsX, groupsY, groupsZ);

    /* THE BARRIER IS NOT OPTIONAL AND IT IS EASY TO OMIT. Without it the next
     * read of what this wrote returns whatever was there before — no GL error,
     * no warning, and a result that is stale by exactly one frame, which reads
     * as lag rather than as a missing barrier. ComputeSelfTest.hpp documents
     * the same trap. */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT
                    | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void OpenGlRenderDevice::Encoder::pushDebugGroup(const char* name)
{
    if (name == nullptr) return;


    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
    debugDepth_++;
}

void OpenGlRenderDevice::Encoder::popDebugGroup()
{
    if (debugDepth_ <= 0) return;


    glPopDebugGroup();
    debugDepth_--;
}

/* ========================================================================== */
/*  The device                                                                */
/* ========================================================================== */

namespace {

/* ================= THE ONE CALL THE ENGINE'S MATHS DEPENDS ON =============
 *
 * glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) tells GL that clip-space depth
 * runs 0..1 rather than -1..1. Mat4.hpp chose 0..1 because Metal, Vulkan, D3D
 * and every console API use it, and said in its header that the GL backend
 * would say this one sentence at startup. It never did, and this is that.
 *
 * ===================== WHAT IT LOOKS LIKE WHEN IT IS MISSING ===============
 *
 * Not a crash, not an error, and not obviously a depth problem. GL maps clip z
 * to window depth as (z + 1) / 2, so a projection already producing 0..1 lands
 * in 0.5..1.0 — the whole scene squashed into the back half of the buffer.
 *
 * The world still draws, because nothing falls outside [-1, 1] and so nothing
 * is clipped. What breaks is everything that COMPARES a depth against a number
 * it computed itself:
 *
 *   - the shadow test reads its 0..1 coordinate against a stored 0.5..1.0 and
 *     passes everywhere, so every surface is lit and NOTHING HAS SHADOWS;
 *   - SSAO unprojects the depth plane through an inverse projection that
 *     assumes 0..1, and reconstructs positions that are simply wrong.
 *
 * Both read as "that feature is not working yet" rather than as one missing
 * line at device creation, which is exactly why this comment is this long.
 *
 * ========================== WHY IT IS LOADED BY HAND =======================
 *
 * It is ARB_clip_control, core in GL 4.5. This device's floor is 4.3 and the
 * glad the window backend loaded stops at 4.3, so the symbol is not declared
 * and not resolved — hence the loader argument and the typedef.
 *
 * REQUIRED, NOT OPTIONAL. A machine without it cannot render this engine's
 * matrices correctly, and there is no honest fallback short of a second set of
 * projection matrices for one backend — which is precisely the "four backends
 * fix it up forever" outcome Mat4.hpp rejected. So device creation fails and
 * says why. In practice this means the GL backend needs 4.5, which every
 * desktop driver from 2014 onward provides; macOS is the exception and caps at
 * 4.1, so it cannot run this backend anyway (compute is 4.3).
 *
 * ============ AND WHY IT IS SET PER PASS RATHER THAN ONCE AT STARTUP =======
 *
 * Because two renderers currently share one GL context, and they disagree about
 * this. raylib builds its projections the GL way, so leaving 0..1 set globally
 * clips away everything in the near half of ITS view volume — the shipping
 * renderer would lose the front of every scene to fix the one under
 * construction, which is not a trade worth making at any point in a migration.
 *
 * So the convention is scoped to the device's own passes: set on beginPass,
 * restored on endPass. Two state calls per pass, a few dozen a frame, against a
 * driver call that costs approximately nothing — and in exchange neither
 * renderer can disturb the other no matter which draws, in what order, or how
 * many times.
 *
 * THIS COLLAPSES TO ONE CALL AT DEVICE CREATION the day the raylib path is
 * deleted, because then nothing else in the process draws. Written down so that
 * simplification is recognised as available rather than rediscovered as a
 * mystery. */
constexpr unsigned int kLowerLeft         = 0x8CA1;   /* GL_LOWER_LEFT          */
constexpr unsigned int kZeroToOne         = 0x935F;   /* GL_ZERO_TO_ONE         */
constexpr unsigned int kNegativeOneToOne  = 0x935E;   /* GL_NEGATIVE_ONE_TO_ONE */

/* GLAD_API_PTR, not APIENTRY. GL entry points are __stdcall on Windows and
 * plain on everything else, and glad already resolved which — reaching for
 * APIENTRY directly needs windows.h, which glad deliberately does not pull in.
 * Getting the convention wrong here corrupts the stack on one platform only. */
using ClipControlFn = void (GLAD_API_PTR*)(unsigned int origin, unsigned int depth);

OpenGlRenderDevice::ProcAddress resolveClipControl(OpenGlRenderDevice::ProcLoader loadProc)
{
    if (loadProc == nullptr) return nullptr;
    return loadProc("glClipControl");
}

/* THE ONE PLACE THE CAST HAPPENS, and the only file where GLAD_API_PTR is
 * visible to make it correct — see the member's note in the header.
 *
 * Converting between unrelated FUNCTION pointers is round-trip safe and is
 * exactly what a proc-address loader exists for; the same cast through a data
 * pointer would be the undefined one. */
void setClipDepth(OpenGlRenderDevice::ProcAddress clipControl, unsigned int depth)
{
    reinterpret_cast<ClipControlFn>(clipControl)(kLowerLeft, depth);
}

}  // namespace

std::unique_ptr<OpenGlRenderDevice> OpenGlRenderDevice::create(ProcLoader loadProc,
                                                              bool debugContext)
{
    /* A CONTEXT MUST ALREADY EXIST. The platform's window created it — see the
     * ordering argument in RaylibPlatform.hpp. Checking rather than assuming,
     * because every GL call below a missing context silently does nothing and
     * the first symptom is a black screen. */
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (version == nullptr) {
        LOGGER.error("OpenGlRenderDevice: no GL context is current - the window must be "
                     "created before the device");
        return nullptr;
    }

    std::unique_ptr<OpenGlRenderDevice> device(new OpenGlRenderDevice());

    int major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major < 4 || (major == 4 && minor < 3)) {
        LOGGER.error("OpenGlRenderDevice: GL {}.{} is below the 4.3 this engine targets", major, minor);
        return nullptr;
    }

    /* RESOLVED AT CREATION, APPLIED PER PASS — see the long note above for both
     * halves of that. Failing here rather than at the first pass because a
     * device that cannot honour the engine's depth convention is not a device
     * this engine can use, and finding that out at startup beats finding it out
     * as absent shadows. */
    device->clipControl_ = resolveClipControl(loadProc);
    if (device->clipControl_ == nullptr) {
        LOGGER.error("OpenGlRenderDevice: glClipControl is unavailable (GL {}.{}, needs 4.5 "
                     "or ARB_clip_control) - the engine's 0..1 clip depth cannot be honoured, "
                     "and shadows and SSAO would be silently wrong rather than absent",
                     major, minor);
        return nullptr;
    }

    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    device->backendName_ = "OpenGL " + std::to_string(major) + "." + std::to_string(minor);
    device->deviceName_  = renderer != nullptr ? renderer : "unknown";

    DeviceCapabilities& caps = device->capabilities_;
    caps.backendName = device->backendName_.c_str();
    caps.deviceName  = device->deviceName_.c_str();

    /* ASKED, NOT ASSUMED — see DeviceCapabilities. Every one of these is false
     * on at least one target the engine intends to ship to. */
    caps.compute           = true;   /* guaranteed by the 4.3 floor checked above */
    caps.geometryShaders   = true;
    caps.cubeArrays        = true;
    caps.anisotropicFilter = hasExtension("GL_EXT_texture_filter_anisotropic");
    caps.depthClamp        = true;

    /* FILTER ACROSS CUBE FACE BOUNDARIES rather than clamping at them. Without
     * it every reflection probe shows three seams meeting at each corner, which
     * on a smooth surface reads as a crack in the reflected world.
     *
     * HERE BECAUSE IT IS CONTEXT STATE, NOT SAMPLER STATE — there is nowhere
     * else to put it. `SamplerDesc` cannot express it and a per-pass enable
     * would be a global toggled by whichever pass ran last.
     *
     * IT WAS ALREADY ON, and that is precisely why it belongs here now:
     * `ReflectionProbeSet::create` enables it, that set is still constructed
     * alongside the device path, and so the device renderer has been getting
     * seamless filtering as a side effect of the renderer it exists to replace.
     * Deleting FrameRenderer at parity would have taken it away silently, and
     * the symptom — seams on every probe — points at the probe code rather than
     * at a deletion in an unrelated file. Setting it twice costs nothing. */
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    int value = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    caps.maxTextureSize = static_cast<uint32_t>(value);

    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &value);
    caps.maxColourTargets = static_cast<uint32_t>(value);

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &value);
    caps.uniformBufferAlign = static_cast<uint32_t>(value);

    if (caps.anisotropicFilter) {
        float maxAnisotropy = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
        caps.maxAnisotropy = static_cast<uint32_t>(maxAnisotropy);
    }

    if (debugContext) {
        /* SYNCHRONOUS, which is the only setting that makes a debug context
         * worth having: without it the callback fires at an unspecified later
         * point and the stack that caused the error is long gone. */
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    }

    glGenVertexArrays(1, &device->emptyVao_);
    device->encoder_ = std::make_unique<Encoder>(*device);

    LOGGER.info("OpenGlRenderDevice: {} on {} (max texture {}, {} colour targets)",
                device->backendName_, device->deviceName_,
                caps.maxTextureSize, caps.maxColourTargets);

    return device;
}

OpenGlRenderDevice::~OpenGlRenderDevice()
{
    /* EVERYTHING THIS OWNS, released while the context is still alive. The
     * platform closes the window after destroying the device precisely so that
     * this is true — see the teardown note in RaylibPlatform.hpp. */
    for (auto& slot : textures_.slots)  if (slot.live) glDeleteTextures(1, &slot.value.name);
    for (auto& slot : buffers_.slots)   if (slot.live) glDeleteBuffers(1, &slot.value.name);
    for (auto& slot : samplers_.slots)  if (slot.live) glDeleteSamplers(1, &slot.value.name);
    for (auto& slot : meshes_.slots)    if (slot.live) glDeleteVertexArrays(1, &slot.value.vao);
    for (auto& slot : shaders_.slots)   if (slot.live) glDeleteProgram(slot.value.program);

    for (auto& entry : framebuffers_) glDeleteFramebuffers(1, &entry.second);

    if (emptyVao_ != 0) glDeleteVertexArrays(1, &emptyVao_);
}

void OpenGlRenderDevice::setBackbufferSize(uint32_t width, uint32_t height)
{
    backbufferWidth_  = width;
    backbufferHeight_ = height;
}

void OpenGlRenderDevice::backbufferSize(uint32_t& width, uint32_t& height) const
{
    if (backbufferWidth_ != 0 && backbufferHeight_ != 0) {
        width  = backbufferWidth_;
        height = backbufferHeight_;
        return;
    }

    /* NOBODY HAS SAID YET — a device driven by something that does not own a
     * window, or the frames before the first beginFrame. The viewport is the
     * best guess available and is right at startup, when nothing has changed it
     * from whatever created the context.
     *
     * It is NOT right once passes have run, which is why this is a fallback and
     * not the implementation — see setBackbufferSize. */
    int viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    width  = static_cast<uint32_t>(viewport[2]);
    height = static_cast<uint32_t>(viewport[3]);
}

void OpenGlRenderDevice::present()
{
    /* NOTHING YET, and deliberately.
     *
     * raylib owns the window and therefore the buffer swap, and the renderer
     * still calls its EndDrawing directly. Swapping here as well would present
     * twice a frame. This becomes a real swap when either the renderer stops
     * going through raylib or a native window backend replaces it — whichever
     * happens first — and it exists now so the call site is already in the
     * right place. */
}

/* ---- pools ---------------------------------------------------------------
 *
 * One shape, six instantiations. The generation check is the whole point — see
 * the header note on why a stale handle must resolve to nothing rather than to
 * whatever took its slot. */
template <typename T>
T* OpenGlRenderDevice::resolveIn(Pool<T>& pool, uint32_t id)
{
    uint32_t index = 0;
    uint8_t  generation = 0;
    if (!unpackHandle(id, index, generation)) return nullptr;
    if (index >= pool.slots.size()) return nullptr;

    auto& slot = pool.slots[index];
    if (!slot.live || slot.generation != generation) return nullptr;

    return &slot.value;
}

template <typename T>
uint32_t OpenGlRenderDevice::allocateIn(Pool<T>& pool, const T& value)
{
    uint32_t index = 0;
    if (!pool.free.empty()) {
        index = pool.free.back();
        pool.free.pop_back();
    } else {
        index = static_cast<uint32_t>(pool.slots.size());
        pool.slots.emplace_back();
    }

    auto& slot = pool.slots[index];
    slot.value = value;
    slot.live  = true;
    return packHandle(index, slot.generation);
}

template <typename T>
bool OpenGlRenderDevice::releaseIn(Pool<T>& pool, uint32_t id)
{
    uint32_t index = 0;
    uint8_t  generation = 0;
    if (!unpackHandle(id, index, generation)) return false;
    if (index >= pool.slots.size()) return false;

    auto& slot = pool.slots[index];
    if (!slot.live || slot.generation != generation) return false;

    slot.live = false;

    /* BUMPED SO THE NEXT HOLDER IS DISTINGUISHABLE, and skipping zero because a
     * zero generation combined with index 0 would pack to a handle that reads
     * as plausible. Wrapping after 255 reuses a generation, which is a 1-in-256
     * chance of a stale handle resolving — vastly better than the certainty
     * without it, and the alternative is spending more of the 32 bits on
     * generation than on index. */
    slot.generation = slot.generation == 255 ? 1 : static_cast<uint8_t>(slot.generation + 1);
    pool.free.push_back(index);
    return true;
}

OpenGlRenderDevice::Texture*  OpenGlRenderDevice::resolve(TextureHandle h)  { return resolveIn(textures_, h.id); }
OpenGlRenderDevice::Buffer*   OpenGlRenderDevice::resolve(BufferHandle h)   { return resolveIn(buffers_, h.id); }
OpenGlRenderDevice::Shader*   OpenGlRenderDevice::resolve(ShaderHandle h)   { return resolveIn(shaders_, h.id); }
OpenGlRenderDevice::Pipeline* OpenGlRenderDevice::resolve(PipelineHandle h) { return resolveIn(pipelines_, h.id); }
OpenGlRenderDevice::Mesh*     OpenGlRenderDevice::resolve(MeshHandle h)     { return resolveIn(meshes_, h.id); }
OpenGlRenderDevice::Sampler*  OpenGlRenderDevice::resolve(SamplerHandle h)  { return resolveIn(samplers_, h.id); }

/* ---- resources -----------------------------------------------------------*/

TextureHandle OpenGlRenderDevice::createTexture(const TextureDesc& desc)
{
    const GlFormat format = translate(desc.format);
    if (!format.valid || desc.width == 0 || desc.height == 0) {
        LOGGER.error("createTexture '{}': unusable descriptor ({}x{}, format {})",
                     desc.name != nullptr ? desc.name : "?", desc.width, desc.height,
                     static_cast<int>(desc.format));
        return {};
    }

    Texture texture;
    texture.format = desc.format;
    texture.width  = desc.width;
    texture.height = desc.height;
    texture.layers = desc.layers;
    texture.mips   = desc.mipLevels > 0 ? desc.mipLevels : 1;

    /* Four distinct GL targets out of one descriptor. A cube array is the case
     * the reflection probes need and the one most likely to be got wrong,
     * because it is `6 * probes` layers rather than `probes`. */
    if (desc.cube) texture.target = desc.layers > 6 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
    else           texture.target = desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;

    glGenTextures(1, &texture.name);
    glBindTexture(texture.target, texture.name);

    /* IMMUTABLE STORAGE (glTexStorage, not glTexImage). The size and format are
     * fixed at creation, which is what every other backend does anyway, and it
     * lets the driver decide the layout once instead of re-validating on every
     * upload. It also makes a mismatched later upload an error rather than a
     * silent reallocation. */
    switch (texture.target) {
        case GL_TEXTURE_2D:
            glTexStorage2D(texture.target, static_cast<int>(texture.mips),
                           format.internalFormat, static_cast<int>(desc.width),
                           static_cast<int>(desc.height));
            break;
        case GL_TEXTURE_CUBE_MAP:
            glTexStorage2D(texture.target, static_cast<int>(texture.mips),
                           format.internalFormat, static_cast<int>(desc.width),
                           static_cast<int>(desc.height));
            break;
        default:
            glTexStorage3D(texture.target, static_cast<int>(texture.mips),
                           format.internalFormat, static_cast<int>(desc.width),
                           static_cast<int>(desc.height), static_cast<int>(desc.layers));
            break;
    }

    /* A SENSIBLE DEFAULT SAMPLER STATE, because a texture created and sampled
     * without one is otherwise black on most drivers: the GL default asks for
     * mipmaps that immutable storage has not generated yet. Callers that care
     * bind a sampler object, which overrides all of this. */
    glTexParameteri(texture.target, GL_TEXTURE_MIN_FILTER,
                    texture.mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(texture.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(texture.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(texture.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (desc.name != nullptr)
        glObjectLabel(GL_TEXTURE, texture.name, -1, desc.name);

    glBindTexture(texture.target, 0);
    return TextureHandle{ allocateIn(textures_, texture) };
}

SamplerHandle OpenGlRenderDevice::createSampler(const SamplerDesc& desc)
{
    Sampler sampler;
    glGenSamplers(1, &sampler.name);

    glSamplerParameteri(sampler.name, GL_TEXTURE_MIN_FILTER,
                        static_cast<int>(minifyFilter(desc.minify, desc.mip, 2)));
    glSamplerParameteri(sampler.name, GL_TEXTURE_MAG_FILTER,
                        desc.magnify == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR);

    glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_S, static_cast<int>(translate(desc.wrapU)));
    glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_T, static_cast<int>(translate(desc.wrapV)));
    glSamplerParameteri(sampler.name, GL_TEXTURE_WRAP_R, static_cast<int>(translate(desc.wrapW)));

    if (desc.maxAnisotropy > 1.0f && capabilities_.anisotropicFilter)
        glSamplerParameterf(sampler.name, GL_TEXTURE_MAX_ANISOTROPY_EXT, desc.maxAnisotropy);

    /* THE LOD CLAMP, which is what makes a prefilter pass legal — see
     * SamplerDesc. Set unconditionally rather than only when it differs from the
     * default: GL's own defaults are -1000..1000, ours are 0..1000, and leaving
     * the negative half in place would let a bias push a read below level zero. */
    glSamplerParameterf(sampler.name, GL_TEXTURE_MIN_LOD, desc.minLod);
    glSamplerParameterf(sampler.name, GL_TEXTURE_MAX_LOD, desc.maxLod);

    /* A COMPARISON SAMPLER IS A DIFFERENT OBJECT, not a flag on the read. With
     * this set the hardware's bilinear unit does the percentage-closer filter
     * for free; without it the shader does four taps by hand for the same
     * result at four times the cost. */
    if (desc.compare != CompareFunc::Never) {
        glSamplerParameteri(sampler.name, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(sampler.name, GL_TEXTURE_COMPARE_FUNC,
                            static_cast<int>(translate(desc.compare)));
    }

    return SamplerHandle{ allocateIn(samplers_, sampler) };
}

BufferHandle OpenGlRenderDevice::createBuffer(const BufferDesc& desc)
{
    if (desc.bytes == 0) return {};

    Buffer buffer;
    buffer.bytes = desc.bytes;
    buffer.usage = desc.usage;

    glGenBuffers(1, &buffer.name);

    /* GL_COPY_WRITE_BUFFER as the staging target: binding to it has no side
     * effect on any pipeline state, unlike GL_ARRAY_BUFFER, which would clobber
     * whatever a VAO is mid-setup with. */
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer.name);
    glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(desc.bytes), nullptr,
                 bufferUsageHint(desc.access));

    if (desc.name != nullptr)
        glObjectLabel(GL_BUFFER, buffer.name, -1, desc.name);

    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return BufferHandle{ allocateIn(buffers_, buffer) };
}

ShaderHandle OpenGlRenderDevice::createShader(const char* name, const char* vertexSource,
                                          const char* fragmentSource)
{
    if (vertexSource == nullptr || fragmentSource == nullptr) return {};

    uint32_t vertex = 0;
    uint32_t fragment = 0;
    if (!compileStage(GL_VERTEX_SHADER, vertexSource, name, vertex)) return {};
    if (!compileStage(GL_FRAGMENT_SHADER, fragmentSource, name, fragment)) {
        glDeleteShader(vertex);
        return {};
    }

    Shader shader;
    shader.program = glCreateProgram();
    glAttachShader(shader.program, vertex);
    glAttachShader(shader.program, fragment);
    glLinkProgram(shader.program);

    /* DETACHED AND DELETED WHETHER OR NOT THE LINK WORKED. The program holds
     * its own copy once linked, so leaving these attached keeps the compiled
     * stages alive for the life of the program for nothing. */
    glDetachShader(shader.program, vertex);
    glDetachShader(shader.program, fragment);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    int linked = 0;
    glGetProgramiv(shader.program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        char log[2048] = {};
        int  length = 0;
        glGetProgramInfoLog(shader.program, static_cast<int>(sizeof log) - 1, &length, log);
        LOGGER.error("shader '{}' failed to link: {}", name != nullptr ? name : "?", log);

        glDeleteProgram(shader.program);
        return {};
    }

    if (name != nullptr)
        glObjectLabel(GL_PROGRAM, shader.program, -1, name);

    return ShaderHandle{ allocateIn(shaders_, shader) };
}

ShaderHandle OpenGlRenderDevice::createComputeShader(const char* name, const char* source)
{
    if (source == nullptr) return {};
    if (!capabilities_.compute) {
        LOGGER.error("createComputeShader '{}': this device has no compute",
                     name != nullptr ? name : "?");
        return {};
    }

    uint32_t stage = 0;
    if (!compileStage(GL_COMPUTE_SHADER, source, name, stage)) return {};

    Shader shader;
    shader.compute = true;
    shader.program = glCreateProgram();
    glAttachShader(shader.program, stage);
    glLinkProgram(shader.program);
    glDetachShader(shader.program, stage);
    glDeleteShader(stage);

    int linked = 0;
    glGetProgramiv(shader.program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        char log[2048] = {};
        int  length = 0;
        glGetProgramInfoLog(shader.program, static_cast<int>(sizeof log) - 1, &length, log);
        LOGGER.error("compute shader '{}' failed to link: {}", name != nullptr ? name : "?", log);

        glDeleteProgram(shader.program);
        return {};
    }

    return ShaderHandle{ allocateIn(shaders_, shader) };
}

PipelineHandle OpenGlRenderDevice::createPipeline(const PipelineDesc& desc)
{
    if (resolve(desc.shader) == nullptr) {
        LOGGER.error("createPipeline '{}': its shader is invalid",
                     desc.name != nullptr ? desc.name : "?");
        return {};
    }

    /* NOTHING IS COMPILED HERE, because GL has no pipeline object — the state
     * is simply recorded and applied at bind. That asymmetry is the reason
     * PipelineDesc carries its attachment formats even though this backend
     * ignores them: Metal and Vulkan compile against them, and a descriptor
     * that only carried what GL needs would be missing the fields on the day
     * the second backend arrives. */
    Pipeline pipeline;
    pipeline.shader = desc.shader;
    pipeline.depth  = desc.depth;
    pipeline.blend  = desc.blend;
    pipeline.raster = desc.raster;
    pipeline.layout = desc.vertexLayout;

    return PipelineHandle{ allocateIn(pipelines_, pipeline) };
}

MeshHandle OpenGlRenderDevice::createMesh(const VertexLayout& layout, BufferHandle verticesHandle,
                                          uint32_t vertexCount, BufferHandle indicesHandle,
                                          uint32_t indexCount)
{
    const Buffer* vertices = resolve(verticesHandle);
    if (vertices == nullptr || vertexCount == 0) {
        LOGGER.error("createMesh: needs a vertex buffer and a non-zero vertex count");
        return {};
    }

    /* INDICES ARE OPTIONAL, and most of this engine's geometry has none: the
     * static world is triangle soup straight out of the box emitter. Demanding
     * an index buffer would mean generating 0,1,2,3… for every mesh — a second
     * buffer the size of the first, uploaded and bound, describing nothing. */
    const Buffer* indices = resolve(indicesHandle);
    const bool indexed = indices != nullptr && indexCount > 0;

    Mesh mesh;
    mesh.vertices    = verticesHandle;
    mesh.indices     = indexed ? indicesHandle : BufferHandle{};
    mesh.vertexCount = vertexCount;
    mesh.indexCount  = indexed ? indexCount : 0;
    mesh.indexed     = indexed;
    mesh.layout      = layout;

    /* THE VAO CAPTURES THE LAYOUT ONCE. A draw then costs one bind rather than
     * re-describing every attribute — which is where an immediate-mode
     * renderer spends a surprising amount of its CPU time. */
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glBindBuffer(GL_ARRAY_BUFFER, vertices->name);
    if (indexed) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->name);

    for (int i = 0; i < layout.attributeCount && i < VertexLayout::kMaxAttributes; i++) {
        const VertexAttribute& attribute = layout.attributes[i];

        int      components = 0;
        uint32_t type = 0;
        bool     normalised = false;
        translate(attribute.format, components, type, normalised);

        glEnableVertexAttribArray(attribute.location);
        glVertexAttribPointer(attribute.location, components, type,
                              normalised ? GL_TRUE : GL_FALSE,
                              static_cast<int>(layout.stride),
                              reinterpret_cast<const void*>(
                                  static_cast<uintptr_t>(attribute.offset)));

        /* PER INSTANCE ADVANCES ONCE PER INSTANCE, not per vertex. Set on the
         * whole layout rather than per attribute because mixing the two in one
         * buffer is a complication nothing here needs. */
        if (layout.perInstance) glVertexAttribDivisor(attribute.location, 1);
    }

    /* The element buffer binding is part of the VAO, so it must NOT be unbound
     * before the VAO is — doing so records "no indices" and every draw from
     * this mesh renders nothing. */
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return MeshHandle{ allocateIn(meshes_, mesh) };
}

/* ---- destruction ---------------------------------------------------------*/

void OpenGlRenderDevice::destroy(TextureHandle handle)
{
    if (Texture* texture = resolve(handle)) glDeleteTextures(1, &texture->name);
    releaseIn(textures_, handle.id);
}

void OpenGlRenderDevice::destroy(SamplerHandle handle)
{
    if (Sampler* sampler = resolve(handle)) glDeleteSamplers(1, &sampler->name);
    releaseIn(samplers_, handle.id);
}

void OpenGlRenderDevice::destroy(BufferHandle handle)
{
    if (Buffer* buffer = resolve(handle)) glDeleteBuffers(1, &buffer->name);
    releaseIn(buffers_, handle.id);
}

void OpenGlRenderDevice::destroy(PipelineHandle handle)
{
    /* Owns no GL object — it is recorded state and a handle to a shader it does
     * not own. Destroying a pipeline must not destroy the shader, because
     * several pipelines routinely share one. */
    releaseIn(pipelines_, handle.id);
}

void OpenGlRenderDevice::destroy(ShaderHandle handle)
{
    if (Shader* shader = resolve(handle)) glDeleteProgram(shader->program);
    releaseIn(shaders_, handle.id);
}

void OpenGlRenderDevice::destroy(MeshHandle handle)
{
    /* The VAO is the mesh's; the buffers are not. They were created separately
     * and may be shared between meshes, so they are the caller's to destroy. */
    if (Mesh* mesh = resolve(handle)) glDeleteVertexArrays(1, &mesh->vao);
    releaseIn(meshes_, handle.id);
}

/* ---- uploads -------------------------------------------------------------*/

void OpenGlRenderDevice::updateBuffer(BufferHandle handle, const void* data,
                                  uint64_t bytes, uint64_t offset)
{
    Buffer* buffer = resolve(handle);
    if (buffer == nullptr || data == nullptr || bytes == 0) return;

    if (offset + bytes > buffer->bytes) {
        LOGGER.error("updateBuffer: {} bytes at offset {} overruns a {}-byte buffer",
                     bytes, offset, buffer->bytes);
        return;
    }

    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer->name);
    glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(bytes), data);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

void OpenGlRenderDevice::updateTexture(TextureHandle handle, const void* pixels,
                                   uint32_t layer, uint32_t mip)
{
    Texture* texture = resolve(handle);
    if (texture == nullptr || pixels == nullptr) return;

    const GlFormat format = translate(texture->format);
    if (!format.valid) return;

    /* Mip dimensions halve and never go below one — a 1x1 mip of a 256x1
     * texture is 1x1, not 1x0, and asking GL to upload a zero-height image is
     * an error rather than a no-op. */
    const int width  = static_cast<int>(texture->width >> mip) > 0
                           ? static_cast<int>(texture->width >> mip) : 1;
    const int height = static_cast<int>(texture->height >> mip) > 0
                           ? static_cast<int>(texture->height >> mip) : 1;

    glBindTexture(texture->target, texture->name);

    /* TIGHTLY PACKED, which is what every caller here supplies and what
     * IImageDecoder promises. GL defaults to four-byte row alignment, so an
     * RGB or single-channel image with an odd width would be read with phantom
     * padding — the classic diagonally-skewed texture. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    switch (texture->target) {
        case GL_TEXTURE_2D:
            glTexSubImage2D(texture->target, static_cast<int>(mip), 0, 0, width, height,
                            format.layout, format.type, pixels);
            break;
        case GL_TEXTURE_CUBE_MAP:
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, static_cast<int>(mip),
                            0, 0, width, height, format.layout, format.type, pixels);
            break;
        default:
            glTexSubImage3D(texture->target, static_cast<int>(mip), 0, 0,
                            static_cast<int>(layer), width, height, 1,
                            format.layout, format.type, pixels);
            break;
    }

    glBindTexture(texture->target, 0);
}

void OpenGlRenderDevice::generateMips(TextureHandle handle)
{
    Texture* texture = resolve(handle);
    if (texture == nullptr || texture->mips <= 1) return;

    glBindTexture(texture->target, texture->name);
    glGenerateMipmap(texture->target);
    glBindTexture(texture->target, 0);
}

/* ---- passes --------------------------------------------------------------*/

uint32_t OpenGlRenderDevice::framebufferFor(const PassDesc& desc)
{
    /* THE BACKBUFFER IS FRAMEBUFFER ZERO, and there are two ways a pass says it
     * wants it: no attachments at all, or attachments carrying no texture.
     *
     * THE SECOND CASE IS NOT A MISTAKE TO TOLERATE, it is how a screen pass
     * states its load and store actions. `colourCount` is what the clear loop
     * in beginPass walks, so a pass that wants the backbuffer CLEARED has to
     * declare an attachment — with no texture, because the backbuffer is not
     * one. Treating that as an FBO to build produces
     * GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT and a pass that silently does
     * nothing, which is exactly what it did until this check existed. */
    bool anyTexture = desc.hasDepth && desc.depth.texture.valid();
    for (int i = 0; i < desc.colourCount && !anyTexture; i++)
        anyTexture = desc.colours[i].texture.valid();

    if (!anyTexture) return 0;

    /* Keyed on the attachment set so a changed target gets its own FBO rather
     * than silently reusing one built for different textures. Mixing the layer
     * and the mip in matters for the probe capture, which renders ninety-six
     * slices of ONE texture and would otherwise share a single cached
     * framebuffer between all of them.
     *
     * EVERY FIELD GETS ITS OWN MULTIPLY, and that is not stylistic. This packed
     * them as `id * 8 + layer`, which is only injective while a layer number
     * stays under eight — true when the only array in the engine was a six-face
     * cubemap, and false the moment the probe array arrived with ninety-six.
     * Past that, texture 5 slice 24 and texture 8 slice 0 hash identically, and
     * the consequence is a pass silently rendering into somebody else's target:
     * a probe face landing in the scene colour buffer, or the reverse. The
     * depth term happened to separate the cases that exist today, which is
     * exactly the kind of accident that stops holding when a pass is added. */
    uint64_t key = 0;
    for (int i = 0; i < desc.colourCount; i++) {
        key = key * 1000003u + desc.colours[i].texture.id;
        key = key * 1000003u + desc.colours[i].layer;
        key = key * 1000003u + desc.colours[i].mip;
    }
    if (desc.hasDepth) {
        key = key * 1000003u + desc.depth.texture.id;
        key = key * 1000003u + desc.depth.layer + 1u;
    }

    const auto found = framebuffers_.find(key);
    if (found != framebuffers_.end()) return found->second;

    uint32_t framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    uint32_t drawBuffers[PassDesc::kMaxColourAttachments] = {};
    for (int i = 0; i < desc.colourCount; i++) {
        const Texture* texture = resolve(desc.colours[i].texture);
        if (texture == nullptr) continue;

        if (texture->target == GL_TEXTURE_2D) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                   texture->name, static_cast<int>(desc.colours[i].mip));
        } else if (texture->target == GL_TEXTURE_CUBE_MAP) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + desc.colours[i].layer,
                                   texture->name, static_cast<int>(desc.colours[i].mip));
        } else {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, texture->name,
                                      static_cast<int>(desc.colours[i].mip),
                                      static_cast<int>(desc.colours[i].layer));
        }
        drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
    }

    if (desc.hasDepth) {
        if (const Texture* texture = resolve(desc.depth.texture)) {
            const bool stencil = texture->format == TextureFormat::D24S8
                              || texture->format == TextureFormat::D32FS8;
            const uint32_t attachment = stencil ? GL_DEPTH_STENCIL_ATTACHMENT
                                                : GL_DEPTH_ATTACHMENT;

            if (texture->target == GL_TEXTURE_2D)
                glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D,
                                       texture->name, 0);
            else
                glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, texture->name, 0,
                                          static_cast<int>(desc.depth.layer));
        }
    }

    /* DEPTH-ONLY PASSES MUST SAY SO. Without this GL still expects colour
     * attachment zero and the framebuffer is incomplete — which is exactly the
     * shadow map's case, and it fails with a status code rather than a hint. */
    if (desc.colourCount == 0) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    } else {
        glDrawBuffers(desc.colourCount, drawBuffers);
    }

    const uint32_t status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGGER.error("pass '{}': framebuffer incomplete (0x{:x})",
                     desc.name != nullptr ? desc.name : "?", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        return 0;
    }

    framebuffers_.emplace(key, framebuffer);
    return framebuffer;
}

ICommandEncoder& OpenGlRenderDevice::beginPass(const PassDesc& desc)
{
    if (inPass_)
        LOGGER.error("beginPass '{}': a pass is already open - passes do not nest",
                     desc.name != nullptr ? desc.name : "?");
    inPass_ = true;

    /* THE ENGINE'S CLIP DEPTH, for the duration of this pass only. Restored in
     * endPass, because raylib shares this context and disagrees — see the long
     * note beside resolveClipControl for what happens either way round. */
    setClipDepth(clipControl_, kZeroToOne);

    /* TEMPORARY DIAGNOSTIC: dump this pass's depth attachment the first time it
     * runs, when XC_DUMP_SHADOW names a file. Here rather than in ScenePipeline
     * because the GL object behind a TextureHandle is deliberately not visible
     * above this file. Remove with the comparison it was written for. */
    if (desc.hasDepth && desc.name != nullptr && std::strcmp(desc.name, "shadow map") == 0)
        pendingShadowDump_ = desc.depth.texture;

    glBindFramebuffer(GL_FRAMEBUFFER, framebufferFor(desc));

    if (desc.name != nullptr)
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, desc.name);

    /* THE VIEWPORT COMES FROM THE ATTACHMENTS, not from the window. A pass
     * rendering into a 384-pixel capture while the viewport still covers a
     * 1920-pixel screen draws a corner of the scene at the wrong scale — and it
     * does so without any error, which is why this is derived rather than left
     * to the caller. */
    uint32_t width = 0;
    uint32_t height = 0;
    if (desc.colourCount > 0) {
        if (const Texture* texture = resolve(desc.colours[0].texture)) {
            width  = texture->width;
            height = texture->height;
        }
    } else if (desc.hasDepth) {
        if (const Texture* texture = resolve(desc.depth.texture)) {
            width  = texture->width;
            height = texture->height;
        }
    }
    if (width == 0 || height == 0) backbufferSize(width, height);
    glViewport(0, 0, static_cast<int>(width), static_cast<int>(height));

    /* ---- load actions ---------------------------------------------------
     *
     * GL has no load/store, so Clear becomes a clear and everything else is
     * nothing. The actions still have to be STATED by the pass, because the
     * backends where they are free — the tile-based console and macOS targets —
     * are the ones that cannot infer them. See Formats.hpp.
     *
     * Scissor and depth-write state are forced off first: a clear is scissored
     * by an active scissor rectangle and silently skipped where depth writes
     * are masked, both of which are leftovers from whatever ran last. */
    glDisable(GL_SCISSOR_TEST);

    for (int i = 0; i < desc.colourCount; i++) {
        if (desc.colours[i].load != LoadAction::Clear) continue;

        const ClearColour& colour = desc.colours[i].clearTo;
        const float value[4] = { colour.r, colour.g, colour.b, colour.a };
        glClearBufferfv(GL_COLOR, i, value);
    }

    if (desc.hasDepth && desc.depth.load == LoadAction::Clear) {
        glDepthMask(GL_TRUE);
        const float depth = desc.depth.clearTo;

        if (desc.depth.stencilLoad == LoadAction::Clear) {
            glClearBufferfi(GL_DEPTH_STENCIL, 0, depth,
                            static_cast<int>(desc.depth.stencilClearTo));
        } else {
            glClearBufferfv(GL_DEPTH, 0, &depth);
        }
    }

    encoder_->beginRender();
    return *encoder_;
}

void OpenGlRenderDevice::endPass(ICommandEncoder& encoder)
{
    (void)encoder;

    if (!inPass_) {
        LOGGER.error("endPass: no pass is open");
        return;
    }
    inPass_ = false;

    /* GL'S OWN CONVENTION BACK, so anything else drawing into this context
     * behaves as it always did. Paired with the call in beginPass; the pair is
     * what makes this device's depth convention a property of its passes rather
     * than of the process. */
    setClipDepth(clipControl_, kNegativeOneToOne);

    /* TEMPORARY DIAGNOSTIC — see beginPass. After endPass because the pass has
     * to have finished writing before the readback means anything. */
    if (pendingShadowDump_.valid()) {
        const TextureHandle handle = pendingShadowDump_;
        pendingShadowDump_ = {};

        static bool dumped = false;
        const char* path = std::getenv("XC_DUMP_SHADOW");
        if (!dumped && path != nullptr) {
            dumped = true;
            if (const Texture* texture = resolve(handle)) {
                float minimum = 0.0f;
                float maximum = 0.0f;
                diag::dumpDepthTexture(texture->name, static_cast<int>(texture->width),
                                       path, minimum, maximum);
            }
        }
    }

    glPopDebugGroup();

    /* STORE ACTIONS ARE NOT IMPLEMENTED HERE, and that is a real gap rather
     * than a nothing.
     *
     * glInvalidateFramebuffer is the GL spelling of Discard, and on a desktop
     * immediate-mode renderer it saves approximately nothing — there is no tile
     * memory to avoid writing back. On the tile-based targets it is the whole
     * point of the field. Left out until there is a backend where it pays,
     * because implementing it here would be untestable and unmeasurable on this
     * hardware, and an optimisation nobody can measure is one nobody can tell
     * is broken. The DESCRIPTOR carries it, which is what matters — the call
     * sites are already stating their intent. */

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ICommandEncoder* OpenGlRenderDevice::beginCompute(const char* name)
{
    if (!capabilities_.compute) return nullptr;

    if (name != nullptr)
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);

    /* OUTSIDE A RENDER PASS on every backend — see IRenderDevice.hpp. Binding
     * framebuffer zero rather than leaving a target bound makes that literally
     * true here too, so a dispatch cannot accidentally interact with an
     * attachment. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    encoder_->beginCompute();
    return encoder_.get();
}

bool OpenGlRenderDevice::readTexture(TextureHandle handle, uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height, std::vector<uint8_t>& out,
                                 uint32_t layer)
{
    Texture* texture = resolve(handle);
    if (texture == nullptr || width == 0 || height == 0) return false;

    /* A STALL, AND ONLY THE SELF-TEST CALLS IT — see the header. glReadPixels
     * blocks until the GPU has caught up, which is exactly what makes it useful
     * for proving a pass wrote what it was told to and exactly what makes it
     * unacceptable in a frame. */
    PassDesc desc;
    desc.name = "readback";
    desc.colours[0].texture = handle;
    desc.colours[0].layer   = layer;
    desc.colours[0].load    = LoadAction::Load;
    desc.colourCount = 1;

    const uint32_t framebuffer = framebufferFor(desc);
    if (framebuffer == 0) return false;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    out.resize(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(width), static_cast<int>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, out.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

}  // namespace cromwell::rhi
