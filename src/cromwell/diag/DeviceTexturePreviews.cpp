#include "cromwell/diag/DeviceTexturePreviews.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

namespace cromwell {

using namespace cromwell::rhi;

namespace {

/* THE SAME COVERING TRIANGLE EVERY SCREEN PASS USES, and a fourth copy of it.
 *
 * ScenePipeline declares this inline for the occlusion pass, its blur and the
 * resolve, with a note saying a file per pass would be copies to keep in step —
 * which is the right call inside one file and stops being one here, because
 * this is a different translation unit and the copies can now genuinely drift.
 *
 * IT IS STILL NOT WORTH A HEADER YET. Three lines with no state, no includes
 * and no way to be subtly wrong: a wrong covering triangle draws nothing at
 * all, immediately, on the first frame. Promote it when a fifth caller appears
 * or when the offline toolchain (§4.9) gives shaders a place to live that is
 * not a string literal — whichever comes first. */
const char* const kFullscreenVertex = R"(#version 450 core
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

/* std140: two vec4s, and ONE buffer feeds both shaders — so both declare the
 * block identically even though the cube one reads only the first. A std140
 * block that disagrees between two programs about its own members is silently
 * wrong offsets, which CONVENTIONS.md names as the failure worth avoiding by
 * construction rather than by care. See either shader's PreviewBlock. */
struct PreviewBlockData {
    float preview[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float depthRange[4] = { 0.1f, 0.0f, 1.0f, 0.0f };
};

}  // namespace

DeviceTexturePreviews::DeviceTexturePreviews(IRenderDevice& device) : device_(device) {}

DeviceTexturePreviews::~DeviceTexturePreviews() { release(); }

bool DeviceTexturePreviews::initialise()
{
    if (ready_) return true;

    const std::string source = ShaderLibrary::preprocess("rhi/dev/preview.fs.glsl");
    const std::string cubeSource = ShaderLibrary::preprocess("rhi/dev/preview_cube.fs.glsl");

    if (source.empty() || cubeSource.empty()) {
        LOGGER.error("previews: rhi/dev/preview shaders not found - "
                     "the texture panel will show nothing");
        return false;
    }

    shader_ = device_.createShader("preview", kFullscreenVertex, source.c_str());
    if (!shader_.valid()) return false;

    cubeShader_ = device_.createShader("preview cube", kFullscreenVertex, cubeSource.c_str());
    if (!cubeShader_.valid()) return false;

    /* BOTH PIPELINES ARE THE SAME STATE and differ only in their shader: no
     * vertex layout (the triangle reads no attributes), no depth (a screen pass
     * writes every pixel it covers, and testing against an attachment this pass
     * does not have discards everything on some drivers), no blend, no cull. */
    PipelineDesc desc;
    desc.name             = "preview";
    desc.shader           = shader_;
    desc.colourFormats[0] = TextureFormat::RGBA8;
    desc.colourCount      = 1;
    desc.depth.test       = false;
    desc.depth.write      = false;
    desc.raster.cull      = CullMode::None;

    pipeline_ = device_.createPipeline(desc);
    if (!pipeline_.valid()) return false;

    desc.name   = "preview cube";
    desc.shader = cubeShader_;
    cubePipeline_ = device_.createPipeline(desc);
    if (!cubePipeline_.valid()) return false;

    /* LINEAR AND CLAMPED. A preview is scaled to whatever height the panel is
     * set to, and a point sampler there produces the blocky thumbnails that
     * make a perfectly good buffer look broken.
     *
     * PINNED TO LEVEL ZERO. Every source here has exactly one level, and
     * createSampler asks for a mipmapped minification filter regardless — a
     * texture whose chain does not reach what the filter wants is INCOMPLETE in
     * GL and samples as zero, which here is a black preview of a buffer that is
     * fine. Same two floats the ImGui backend needed; see rhi/MIGRATION.md
     * §4.10. */
    SamplerDesc sampler;
    sampler.minify  = FilterMode::Linear;
    sampler.magnify = FilterMode::Linear;
    sampler.mip     = FilterMode::Nearest;
    sampler.wrapU   = WrapMode::ClampToEdge;
    sampler.wrapV   = WrapMode::ClampToEdge;
    sampler.wrapW   = WrapMode::ClampToEdge;
    sampler.minLod  = 0.0f;
    sampler.maxLod  = 0.0f;

    sampler_ = device_.createSampler(sampler);
    if (!sampler_.valid()) return false;

    BufferDesc block;
    block.name   = "preview block";
    block.bytes  = sizeof(PreviewBlockData);
    block.usage  = BufferUsageUniform;
    block.access = BufferAccess::CpuToGpuPerFrame;

    block_ = device_.createBuffer(block);
    if (!block_.valid()) return false;

    ready_ = true;

    /* SAID ONCE, on the frame the panel is first opened rather than at startup,
     * because that is when this is brought up. It is worth a line: the two
     * shaders are the only ones in the tree that nothing draws with until a
     * human presses a key, so a compile failure in them would otherwise be
     * discovered by a blank texture tab rather than by a log. */
    LOGGER.info("previews: device texture previews ready");
    return true;
}

DeviceTexturePreviews& DeviceTexturePreviews::withDepthRange(float nearPlane,
                                                             float farPlane, float span)
{
    nearPlane_ = nearPlane;
    farPlane_  = farPlane;
    depthSpan_ = span;
    return *this;
}

void DeviceTexturePreviews::release()
{
    for (Slot& slot : slots_) {
        if (slot.texture.valid()) device_.destroy(slot.texture);
        slot = Slot{};
    }

    if (block_.valid())        device_.destroy(block_);
    if (sampler_.valid())      device_.destroy(sampler_);
    if (cubePipeline_.valid()) device_.destroy(cubePipeline_);
    if (pipeline_.valid())     device_.destroy(pipeline_);
    if (cubeShader_.valid())   device_.destroy(cubeShader_);
    if (shader_.valid())       device_.destroy(shader_);

    block_ = {};
    sampler_ = {};
    cubePipeline_ = {};
    pipeline_ = {};
    cubeShader_ = {};
    shader_ = {};
    ready_ = false;
}

bool DeviceTexturePreviews::ensureTarget(int slot, uint32_t width, uint32_t height)
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    if (width == 0 || height == 0) return false;

    Slot& entry = slots_[slot];
    if (entry.texture.valid() && entry.width == width && entry.height == height) return true;

    if (entry.texture.valid()) device_.destroy(entry.texture);
    entry = Slot{};

    /* RGBA8, NOT THE SOURCE'S FORMAT. Every mode above ends in a 0..1 display
     * value, and the point of the copy is that whatever a buffer's storage is,
     * what comes out of here is an ordinary picture a panel can sample without
     * knowing anything about where it came from. */
    TextureDesc desc;
    desc.name   = "preview";
    desc.width  = width;
    desc.height = height;
    desc.format = TextureFormat::RGBA8;
    desc.usage  = TextureUsageSampled | TextureUsageRenderTarget;

    entry.texture = device_.createTexture(desc);
    if (!entry.texture.valid()) {
        LOGGER.warn("previews: could not create a {}x{} preview target", width, height);
        return false;
    }

    entry.width  = width;
    entry.height = height;
    return true;
}

void DeviceTexturePreviews::blit(int slot, PipelineHandle pipeline, TextureHandle source,
                                 float mode, float parameter)
{
    const Slot& entry = slots_[slot];

    PreviewBlockData block;
    block.preview[0] = mode;
    block.preview[1] = parameter;
    block.preview[2] = static_cast<float>(entry.width);
    block.preview[3] = static_cast<float>(entry.height);

    block.depthRange[0] = nearPlane_;
    block.depthRange[1] = farPlane_;
    block.depthRange[2] = depthSpan_;
    device_.updateBuffer(block_, &block, sizeof block, 0);

    PassDesc pass;
    pass.name = "preview";
    pass.colours[0].texture = entry.texture;

    /* DontCare: the covering triangle writes every pixel of the target. */
    pass.colours[0].load  = LoadAction::DontCare;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(pipeline);
    encoder.bindTexture(0, source, sampler_);
    encoder.bindUniformBuffer(1, block_);
    encoder.drawFullscreen();
    device_.endPass(encoder);
}

TextureHandle DeviceTexturePreviews::render(int slot, TextureHandle source, Mode mode,
                                            uint32_t width, uint32_t height)
{
    /* AN INVALID SOURCE IS NOT AN ERROR AND IS NOT A BLANK PICTURE EITHER. A
     * pass that has not run yet, or a buffer a quality preset turned off, has no
     * texture — and the honest answer is "nothing here", which the caller says
     * in words. Blitting whatever was in the target from last frame would be a
     * stale picture of a buffer that does not exist. */
    if (!ready_ || !source.valid()) return {};
    if (!ensureTarget(slot, width, height)) return {};

    blit(slot, pipeline_, source, static_cast<float>(static_cast<int>(mode)), 0.0f);
    return slots_[slot].texture;
}

TextureHandle DeviceTexturePreviews::renderCube(int slot, TextureHandle cubeArray, int probe,
                                                uint32_t faceSize)
{
    if (!ready_ || !cubeArray.valid() || faceSize == 0) return {};
    if (probe < 0) return {};
    if (!ensureTarget(slot, faceSize * 6, faceSize)) return {};

    blit(slot, cubePipeline_, cubeArray, 0.0f, static_cast<float>(probe));
    return slots_[slot].texture;
}

}  // namespace cromwell
