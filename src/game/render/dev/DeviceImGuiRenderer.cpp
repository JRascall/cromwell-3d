#include "game/render/dev/DeviceImGuiRenderer.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstring>

namespace game {
namespace {

using namespace cromwell;
using namespace cromwell::rhi;

/* ImDrawVert, BYTE FOR BYTE. Position, UV, packed colour — which happens to be
 * the same twenty bytes DeviceUiPainter's textured vertex uses, by coincidence
 * rather than by agreement: ImGui defines this layout and we describe it.
 *
 * THE static_assert IS THE WHOLE GUARD. ImGui can be configured to a different
 * vertex (imconfig.h allows it), and a layout that silently disagreed would
 * draw the panel as confetti — every attribute read from the wrong offset. */
static_assert(sizeof(ImDrawVert) == 20, "the layout below describes ImDrawVert");

VertexLayout imguiLayout()
{
    VertexLayout layout;
    layout.stride = sizeof(ImDrawVert);
    layout.attributeCount = 3;
    layout.attributes[0] = { 0, offsetof(ImDrawVert, pos), VertexFormat::Float2 };
    layout.attributes[1] = { 1, offsetof(ImDrawVert, uv),  VertexFormat::Float2 };

    /* NORMALISED BYTES, NOT RAW ONES. ImGui packs its colour as an ImU32 with
     * the red byte lowest, which is exactly what UByte4Normalised reads back as
     * 0..1 in r,g,b,a order. UByte4 would hand the shader 0..255 and every
     * widget would blow out to white. */
    layout.attributes[2] = { 2, offsetof(ImDrawVert, col), VertexFormat::UByte4Normalised };
    return layout;
}

}  // namespace

DeviceImGuiRenderer::DeviceImGuiRenderer(rhi::IRenderDevice& device) : device_(device) {}

DeviceImGuiRenderer::~DeviceImGuiRenderer() { release(); }

bool DeviceImGuiRenderer::initialise()
{
    const std::string vertexSource = ShaderLibrary::preprocess("rhi/ui/imgui.vs.glsl");
    const std::string fragmentSource = ShaderLibrary::preprocess("rhi/ui/imgui.fs.glsl");

    if (vertexSource.empty() || fragmentSource.empty()) {
        LOGGER.error("dev panel: rhi/ui/imgui shaders not found - no panel will be drawn");
        return false;
    }

    shader_ = device_.createShader("imgui", vertexSource.c_str(), fragmentSource.c_str());
    if (!shader_.valid()) return false;

    PipelineDesc desc;
    desc.name         = "imgui";
    desc.shader       = shader_;
    desc.vertexLayout = imguiLayout();

    /* NO DEPTH AT ALL. The panel is drawn over a finished, resolved frame and
     * has no business testing against the scene's depth — which by then is a
     * buffer describing a world at a different resolution. */
    desc.depth.test  = false;
    desc.depth.write = false;

    /* NO CULLING. ImGui emits both windings depending on how a shape was
     * tessellated, and a backend that culled would drop half of some widgets —
     * arcs and thick lines most visibly. Every ImGui backend disables it. */
    desc.raster.cull = CullMode::None;

    /* STRAIGHT ALPHA. ImGui premultiplies nothing; see the fragment shader. */
    desc.blend.enabled      = true;
    desc.blend.sourceColour = BlendFactor::SrcAlpha;
    desc.blend.destColour   = BlendFactor::OneMinusSrcAlpha;
    desc.blend.sourceAlpha  = BlendFactor::One;
    desc.blend.destAlpha    = BlendFactor::OneMinusSrcAlpha;

    pipeline_ = device_.createPipeline(desc);
    if (!pipeline_.valid()) return false;

    /* BILINEAR AND CLAMPED. ImGui draws its atlas at one texel per pixel in the
     * common case, where the filter does not matter — but an embedded image
     * preview is scaled to fit its panel, and a point sampler there produces
     * the blocky thumbnails that make a texture look broken when it is not.
     * Clamped because a widget sampling past its own sub-rectangle should take
     * the edge rather than wrap into a neighbouring glyph. */
    SamplerDesc sampler;
    sampler.minify  = FilterMode::Linear;
    sampler.magnify = FilterMode::Linear;
    sampler.wrapU   = WrapMode::ClampToEdge;
    sampler.wrapV   = WrapMode::ClampToEdge;

    /* PINNED TO LEVEL ZERO, AND THIS IS NOT A PREFERENCE.
     *
     * The atlas is uploaded with ONE level. rhi/MIGRATION.md 4.10 records that
     * createSampler hardcodes a mipmapped minification filter for every
     * sampler, and a texture whose mip chain does not reach what the filter
     * asks for is INCOMPLETE in GL - and an incomplete texture samples as
     * (0,0,0,0). This shader is `vColour * texture(...)`, so black-with-zero-
     * alpha is a panel that submits every vertex, issues every draw, and puts
     * no pixel on the screen. Nothing errors and nothing is missing from a
     * capture tool's list; the geometry is simply multiplied away.
     *
     * Clamping the level range is what makes the one level we have the only
     * one the sampler may reach. Same two floats the probe prefilter added for
     * the opposite reason - there, to stop a pass reading the level it was
     * writing. */
    sampler.mip    = FilterMode::Nearest;
    sampler.minLod = 0.0f;
    sampler.maxLod = 0.0f;
    sampler_ = device_.createSampler(sampler);
    if (!sampler_.valid()) return false;

    /* ---- AND TELL IMGUI WE MANAGE ITS TEXTURES ------------------------
     *
     * Without this flag ImGui takes the legacy path and expects a single font
     * atlas the backend uploaded once — which works until the first glyph that
     * was not in it, at which point the panel renders garbage with nothing to
     * point at. With it, every texture arrives as an explicit request. */
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "cromwell rhi";

    ready_ = true;
    LOGGER.info("dev panel: imgui backend on the device path");
    return true;
}

void DeviceImGuiRenderer::release()
{
    for (TextureHandle texture : textures_)
        if (texture.valid()) device_.destroy(texture);
    textures_.clear();

    if (mesh_.valid())         device_.destroy(mesh_);
    if (vertexBuffer_.valid()) device_.destroy(vertexBuffer_);
    if (indexBuffer_.valid())  device_.destroy(indexBuffer_);
    if (pipeline_.valid())     device_.destroy(pipeline_);
    if (shader_.valid())       device_.destroy(shader_);

    mesh_ = {};
    vertexBuffer_ = {};
    indexBuffer_ = {};
    pipeline_ = {};
    shader_ = {};
    vertexCapacity_ = 0;
    indexCapacity_ = 0;
    ready_ = false;
}

void DeviceImGuiRenderer::updateTexture(ImTextureData* texture)
{
    if (texture == nullptr) return;

    switch (texture->Status) {
        case ImTextureStatus_WantCreate: {
            TextureDesc desc;
            desc.name   = "imgui";
            desc.width  = static_cast<uint32_t>(texture->Width);
            desc.height = static_cast<uint32_t>(texture->Height);

            /* ALPHA8 IS A ONE-CHANNEL TEXTURE and the shader multiplies all
             * four channels, so a font atlas in that format would come through
             * with green, blue and alpha at whatever R8 expands to. ImGui only
             * asks for Alpha8 when told it may; the default is RGBA32 and the
             * case is handled rather than assumed away. */
            desc.format = texture->Format == ImTextureFormat_Alpha8
                        ? TextureFormat::R8 : TextureFormat::RGBA8;
            desc.usage  = TextureUsageSampled;

            const TextureHandle handle = device_.createTexture(desc);
            if (!handle.valid()) {
                LOGGER.error("dev panel: could not create a {}x{} imgui texture",
                             texture->Width, texture->Height);
                return;
            }

            /* LAYER AND MIP DEFAULT TO ZERO, AND THEY ARE NOT WIDTH AND
             * HEIGHT. `updateTexture(handle, pixels, layer, mip)` takes the
             * SLICE and the LEVEL - the size comes from the texture itself.
             * Passing the dimensions here compiles perfectly, because all four
             * are uint32_t, and uploads a 1x1 sub-image into mip level 128 of a
             * texture that has one level: no pixels are written, the atlas
             * samples black, and `vColour * texture(...)` multiplies the entire
             * panel away. Every vertex is submitted and every draw is issued.
             *
             * Same shape as the trap in MIGRATION.md 5 where a vertex range was
             * added to draw() and `draw(mesh, 1)` silently changed from one
             * INSTANCE to one VERTEX. When adjacent parameters share a type,
             * the compiler cannot help and the symptom is somewhere else
             * entirely. */
            device_.updateTexture(handle, texture->GetPixels());

            /* THE HANDLE'S ID IS THE ImTextureID. Not a pointer, and not an
             * index into a table of our own: the handle is already an opaque
             * integer that means nothing outside the device that issued it,
             * which is exactly what ImTextureID is for. It comes back
             * unchanged on every draw command and on WantDestroy. */
            texture->SetTexID(static_cast<ImTextureID>(handle.id));
            texture->Status = ImTextureStatus_OK;

            textures_.push_back(handle);
            break;
        }

        case ImTextureStatus_WantUpdates: {
            const TextureHandle handle{ static_cast<uint32_t>(texture->GetTexID()) };
            if (!handle.valid()) break;

            /* THE WHOLE TEXTURE, not the dirty rectangle ImGui offers. The RHI
             * uploads a complete image and has no sub-rectangle call, and an
             * atlas is a few hundred kilobytes updated when a glyph is added —
             * which is a startup-shaped event, not a per-frame one. A partial
             * upload path would be an interface addition serving one caller.
             * rlImGui does exactly the same against raylib's UpdateTexture. */
            device_.updateTexture(handle, texture->GetPixels());
            texture->Status = ImTextureStatus_OK;
            break;
        }

        case ImTextureStatus_WantDestroy: {
            const TextureHandle handle{ static_cast<uint32_t>(texture->GetTexID()) };
            if (handle.valid()) {
                device_.destroy(handle);
                textures_.erase(std::remove(textures_.begin(), textures_.end(), handle),
                                textures_.end());
            }
            texture->SetTexID(ImTextureID_Invalid);
            texture->Status = ImTextureStatus_Destroyed;
            break;
        }

        case ImTextureStatus_OK:
        case ImTextureStatus_Destroyed:
        default:
            break;
    }
}

bool DeviceImGuiRenderer::ensureCapacity(uint32_t vertices, uint32_t indices)
{
    if (vertices <= vertexCapacity_ && indices <= indexCapacity_ && mesh_.valid()) return true;

    /* GROWN WITH HEADROOM, like the debug line buffer: a panel that gains a row
     * as a list lengthens should not recreate its buffers every frame on the
     * way there. */
    const uint32_t wantedVertices = std::max(vertices + vertices / 4u, 4096u);
    const uint32_t wantedIndices  = std::max(indices + indices / 4u, 8192u);

    if (mesh_.valid())         device_.destroy(mesh_);
    if (vertexBuffer_.valid()) device_.destroy(vertexBuffer_);
    if (indexBuffer_.valid())  device_.destroy(indexBuffer_);
    mesh_ = {};
    vertexBuffer_ = {};
    indexBuffer_ = {};

    BufferDesc vertexDesc;
    vertexDesc.name   = "imgui vertices";
    vertexDesc.bytes  = static_cast<uint64_t>(wantedVertices) * sizeof(ImDrawVert);
    vertexDesc.usage  = BufferUsageVertex;
    vertexDesc.access = BufferAccess::CpuToGpuPerFrame;
    vertexBuffer_ = device_.createBuffer(vertexDesc);
    if (!vertexBuffer_.valid()) return false;

    BufferDesc indexDesc;
    indexDesc.name   = "imgui indices";
    indexDesc.bytes  = static_cast<uint64_t>(wantedIndices) * sizeof(std::uint32_t);
    indexDesc.usage  = BufferUsageIndex;
    indexDesc.access = BufferAccess::CpuToGpuPerFrame;
    indexBuffer_ = device_.createBuffer(indexDesc);
    if (!indexBuffer_.valid()) return false;

    mesh_ = device_.createMesh(imguiLayout(), vertexBuffer_, wantedVertices,
                               indexBuffer_, wantedIndices);
    if (!mesh_.valid()) return false;

    vertexCapacity_ = wantedVertices;
    indexCapacity_ = wantedIndices;
    return true;
}

void DeviceImGuiRenderer::render(const ImDrawData* drawData)
{
    if (!ready_ || drawData == nullptr) return;

    CW_PROFILE_ZONE_N("dev panel");

    /* ---- ImGui's texture requests, BEFORE anything is drawn -------------
     *
     * Serviced outside the pass, which is not a nicety: creating and uploading
     * a texture inside a render pass is undefined on Vulkan and needs a
     * different encoder on Metal. The same rule that made
     * copyBackbufferToTexture a device call rather than an encoder one. */
    if (drawData->Textures != nullptr)
        for (ImTextureData* texture : *drawData->Textures)
            updateTexture(texture);

    if (drawData->CmdListsCount <= 0) return;
    if (drawData->TotalVtxCount <= 0 || drawData->TotalIdxCount <= 0) return;

    const uint32_t totalVertices = static_cast<uint32_t>(drawData->TotalVtxCount);
    const uint32_t totalIndices  = static_cast<uint32_t>(drawData->TotalIdxCount);
    if (!ensureCapacity(totalVertices, totalIndices)) return;

    /* ---- flatten every draw list into the two buffers -------------------
     *
     * INDICES ARE WIDENED AND REBASED IN ONE PASS. ImGui's are 16-bit and
     * relative to their own list's vertex block; this RHI draws 32-bit indices
     * and `drawIndexed` has a first INDEX but no base VERTEX. Both facts are
     * answered by adding the running vertex offset while widening. See the
     * header on why this beats redefining ImDrawIdx. */
    vertexScratch_.clear();
    indexScratch_.clear();
    vertexScratch_.reserve(static_cast<std::size_t>(totalVertices) * sizeof(ImDrawVert));
    indexScratch_.reserve(totalIndices);

    for (int list = 0; list < drawData->CmdListsCount; list++) {
        const ImDrawList* commands = drawData->CmdLists[list];

        const auto vertexBase = static_cast<std::uint32_t>(
            vertexScratch_.size() / sizeof(ImDrawVert));

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(commands->VtxBuffer.Data);
        vertexScratch_.insert(vertexScratch_.end(), bytes,
                              bytes + static_cast<std::size_t>(commands->VtxBuffer.Size)
                                      * sizeof(ImDrawVert));

        for (int i = 0; i < commands->IdxBuffer.Size; i++)
            indexScratch_.push_back(static_cast<std::uint32_t>(commands->IdxBuffer.Data[i])
                                    + vertexBase);
    }

    device_.updateBuffer(vertexBuffer_, vertexScratch_.data(), vertexScratch_.size(), 0);
    device_.updateBuffer(indexBuffer_, indexScratch_.data(),
                         indexScratch_.size() * sizeof(std::uint32_t), 0);

    /* ---- the pass -------------------------------------------------------
     *
     * THE BACKBUFFER, LOADED: an attachment carrying no texture is the screen,
     * and the scene and the game's own UI are already on it. Clearing here
     * would paint the frame out and leave a panel floating on black. */
    PassDesc pass;
    pass.name = "dev panel";
    pass.colours[0].load  = LoadAction::Load;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    /* THE FRAMEBUFFER SCALE, DEFAULTED RATHER THAN TRUSTED. It is ImGui's
     * logical-pixels-to-device-pixels factor and it is normally (1,1) - but it
     * is set by the PLATFORM half of the backend, and this path's platform half
     * is rlImGui, which is not ours. A zero here multiplies the surface to
     * nothing, the vertex stage divides by zero, and the panel is invisible
     * with no error anywhere: the same failure shape as the push constants
     * trap in MIGRATION.md §5, and indistinguishable on screen from a backend
     * that never ran. */
    const float scaleX = drawData->FramebufferScale.x > 0.0f
                       ? drawData->FramebufferScale.x : 1.0f;
    const float scaleY = drawData->FramebufferScale.y > 0.0f
                       ? drawData->FramebufferScale.y : 1.0f;

    const float surfaceWidth  = drawData->DisplaySize.x * scaleX;
    const float surfaceHeight = drawData->DisplaySize.y * scaleY;

    if (surfaceWidth <= 0.0f || surfaceHeight <= 0.0f) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGGER.warn("dev panel: imgui reports a {}x{} display - nothing can be drawn",
                        drawData->DisplaySize.x, drawData->DisplaySize.y);
        }
        return;
    }

    ICommandEncoder& encoder = device_.beginPass(pass);
    encoder.bindPipeline(pipeline_);

    /* THE SURFACE SIZE, PUSHED AFTER bindPipeline AND NOT BEFORE. Push
     * constants are a uniform on the CURRENT program on GL, so pushing before
     * the bind writes them into whatever was bound last — and the failure is a
     * vertex stage dividing by zero, every vertex at infinity, nothing drawn.
     * Identical on screen to a font that failed to load. See MIGRATION.md §5. */
    float push[8][4] = {};
    push[0][0] = surfaceWidth;
    push[0][1] = surfaceHeight;
    encoder.pushConstants(push, sizeof push);

    uint32_t indexOffset = 0;
    TextureHandle boundTexture;

    for (int list = 0; list < drawData->CmdListsCount; list++) {
        const ImDrawList* commands = drawData->CmdLists[list];

        for (const ImDrawCmd& command : commands->CmdBuffer) {
            /* A USER CALLBACK IS NOT A DRAW. ImGui lets a caller splice its own
             * work into the list; nothing in this project does, and honouring
             * it would mean handing game code an encoder — the seam the whole
             * port exists to remove. Skipped, and the indices still advance so
             * the ones after it land in the right place. */
            if (command.UserCallback != nullptr) continue;
            if (command.ElemCount == 0) continue;

            /* ---- the clip rectangle -------------------------------------
             *
             * ImGui measures from the TOP LEFT and setScissor from the BOTTOM
             * LEFT — the convention copyBackbufferToTexture states for the
             * whole interface. Getting the flip wrong does not fail: panels
             * simply clip against the wrong half of the screen, which reads as
             * widgets being cut off at odd places rather than as an axis
             * problem. */
            const ImVec2 offset = drawData->DisplayPos;
            const float minX = std::max((command.ClipRect.x - offset.x)
                                        * scaleX, 0.0f);
            const float minY = std::max((command.ClipRect.y - offset.y)
                                        * scaleY, 0.0f);
            const float maxX = std::min((command.ClipRect.z - offset.x)
                                        * scaleX, surfaceWidth);
            const float maxY = std::min((command.ClipRect.w - offset.y)
                                        * scaleY, surfaceHeight);

            if (maxX <= minX || maxY <= minY) {
                indexOffset += command.ElemCount;
                continue;
            }

            encoder.setScissor(minX, surfaceHeight - maxY, maxX - minX, maxY - minY);

            const TextureHandle texture{ static_cast<uint32_t>(command.GetTexID()) };
            if (texture != boundTexture) {
                encoder.bindTexture(0, texture, sampler_);
                boundTexture = texture;
            }

            encoder.drawIndexed(mesh_, command.ElemCount,
                                indexOffset + command.IdxOffset, 1);
        }

        indexOffset += static_cast<uint32_t>(commands->IdxBuffer.Size);
    }

    /* THE SCISSOR PUT BACK BEFORE THE PASS CLOSES. It is pass state on this
     * backend, and a later pass inheriting a panel's clip rectangle would draw
     * a scene into a corner of the screen — the same class of bug as the
     * backbuffer size read from the current viewport in §5. */
    encoder.setScissor(0.0f, 0.0f, surfaceWidth, surfaceHeight);

    device_.endPass(encoder);
}

}  // namespace game
