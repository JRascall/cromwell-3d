#include "cromwell/ui/paint/DeviceUiPainter.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/text/GlyphAtlas.hpp"

/* RAYLIB ARRIVES THROUGH THIS, and only through this: UiFontSet still declares
 * a raylib Font for the painter it is being replaced by. Nothing below names
 * one — the text path here goes through GlyphAtlas, which is neutral by
 * construction — and the include stops being transitive the day UiPainter is
 * deleted at parity. */
#include "cromwell/ui/text/UiFontSet.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cromwell::ui {
namespace {

using namespace cromwell::rhi;

/* THE VERTEX, AS THE DEVICE SEES IT — and it is UiVertex's layout exactly:
 * two floats and four bytes, twelve in all. Declared here rather than derived
 * so the stride is stated once beside the assert that pins it. */
constexpr uint32_t kVertexStride = 12;
static_assert(sizeof(UiVertex) == kVertexStride,
              "the device layout below describes UiVertex byte for byte");

VertexLayout uiLayout()
{
    VertexLayout layout;
    layout.stride = kVertexStride;
    layout.attributeCount = 2;
    layout.attributes[0] = { 0, 0, VertexFormat::Float2 };

    /* NORMALISED, so the shader reads 0..1 rather than 0..255. A UI colour is a
     * colour, and a shader multiplying by 255 instead of 1 is a white-hot
     * interface that looks like a blending bug. */
    layout.attributes[1] = { 1, 8, VertexFormat::UByte4Normalised };
    return layout;
}

/* The textured vertex: position, UV, packed colour, twenty bytes — shared by
 * the glyph quads and the blur outlines. Same reasoning as kVertexStride —
 * stated once, beside the layout that depends on it, and checked against the
 * struct in ensureTexturedCapacity where the buffer is sized. (The check lives
 * in a member function because TexturedVertex is private, which is where it
 * belongs anyway: the assert and the allocation it protects are the same line
 * of reasoning.) */
constexpr uint32_t kTexturedVertexStride = 20;

VertexLayout uiTexturedLayout()
{
    VertexLayout layout;
    layout.stride = kTexturedVertexStride;
    layout.attributeCount = 3;
    layout.attributes[0] = { 0, 0,  VertexFormat::Float2 };            /* position */
    layout.attributes[1] = { 1, 8,  VertexFormat::Float2 };            /* uv       */
    layout.attributes[2] = { 2, 16, VertexFormat::UByte4Normalised };  /* colour   */
    return layout;
}

/* HOW MANY SEGMENTS A ROUNDED CORNER OF THE FROSTED REGION GETS. Eight, which
 * is what UiPainter::executeBackdropBlur uses — borrowed rather than re-picked,
 * so the two paths round a corner to the same silhouette. */
constexpr int kBlurCornerSegments = 8;

}  // namespace

DeviceUiPainter::DeviceUiPainter(rhi::IRenderDevice& device) : device_(device) {}

DeviceUiPainter::~DeviceUiPainter() { release(); }

void DeviceUiPainter::releaseAtlasTextures()
{
    for (const AtlasTexture& entry : atlasTextures_) {
        if (entry.texture.valid()) device_.destroy(entry.texture);
    }
    atlasTextures_.clear();
}

void DeviceUiPainter::release()
{
    releaseAtlasTextures();

    if (mesh_.valid())     device_.destroy(mesh_);
    if (vertices_.valid()) device_.destroy(vertices_);
    if (indices_.valid())  device_.destroy(indices_);
    if (pipeline_.valid()) device_.destroy(pipeline_);
    if (shader_.valid())   device_.destroy(shader_);

    if (texturedMesh_.valid())     device_.destroy(texturedMesh_);
    if (texturedVertices_.valid()) device_.destroy(texturedVertices_);
    if (texturedIndices_.valid())  device_.destroy(texturedIndices_);
    if (textPipeline_.valid())     device_.destroy(textPipeline_);
    if (textShader_.valid())       device_.destroy(textShader_);
    if (glyphSampler_.valid())     device_.destroy(glyphSampler_);

    if (blurPipeline_.valid())   device_.destroy(blurPipeline_);
    if (blurShader_.valid())     device_.destroy(blurShader_);
    if (captureSampler_.valid()) device_.destroy(captureSampler_);
    if (captureTexture_.valid()) device_.destroy(captureTexture_);

    mesh_ = {};
    vertices_ = {};
    indices_ = {};
    pipeline_ = {};
    shader_ = {};

    texturedMesh_ = {};
    texturedVertices_ = {};
    texturedIndices_ = {};
    textPipeline_ = {};
    textShader_ = {};
    glyphSampler_ = {};

    blurPipeline_ = {};
    blurShader_ = {};
    captureSampler_ = {};
    captureTexture_ = {};
    captureWidth_ = 0;
    captureHeight_ = 0;

    vertexCapacity_ = 0;
    indexCapacity_ = 0;
    texturedVertexCapacity_ = 0;
    texturedIndexCapacity_ = 0;
    ready_ = false;
}

bool DeviceUiPainter::initialise()
{
    if (ready_) return true;

    const std::string vertexSource   = ShaderLibrary::preprocess("rhi/ui/ui.vs.glsl");
    const std::string fragmentSource = ShaderLibrary::preprocess("rhi/ui/ui.fs.glsl");

    if (vertexSource.empty() || fragmentSource.empty()) {
        LOGGER.error("DeviceUiPainter: rhi/ui shaders not found");
        return false;
    }

    shader_ = device_.createShader("ui", vertexSource.c_str(), fragmentSource.c_str());
    if (!shader_.valid()) return false;

    PipelineDesc desc;
    desc.name         = "ui";
    desc.shader       = shader_;
    desc.vertexLayout = uiLayout();

    /* THE BACKBUFFER'S FORMAT. The UI is the last thing in the frame and draws
     * straight onto the screen, past the tone map — see the note in
     * rhi/ui.fs.glsl about why nothing here touches a curve. */
    desc.colourFormats[0] = TextureFormat::RGBA8;
    desc.colourCount = 1;

    /* NO DEPTH, AND NO DEPTH FORMAT. The UI is layered by painter's order, not
     * by depth, and testing against a buffer this pass has no attachment for
     * discards every fragment on some drivers — the failure reads as "the HUD
     * is not drawing at all". */
    desc.depth.test  = false;
    desc.depth.write = false;

    /* STRAIGHT ALPHA, not premultiplied. Every feathered edge in the kit ramps
     * its alpha alone with the colour left at full strength — see
     * ui/shape/Shapes.hpp — so the blender has to scale the source by its own
     * coverage. Premultiplied here would darken every antialiased edge toward
     * black, which reads as a dirty fringe around every rounded corner. */
    desc.blend.enabled      = true;
    desc.blend.sourceColour = BlendFactor::SrcAlpha;
    desc.blend.destColour   = BlendFactor::OneMinusSrcAlpha;
    desc.blend.sourceAlpha  = BlendFactor::One;
    desc.blend.destAlpha    = BlendFactor::OneMinusSrcAlpha;

    /* NO CULLING. The shape builders wind their strips and fans for clarity
     * rather than for a facing test, and a UI triangle has no meaningful front
     * — the raylib painter disables culling around its own draws for the same
     * reason and hands it back afterwards. */
    desc.raster.cull = CullMode::None;

    pipeline_ = device_.createPipeline(desc);
    if (!pipeline_.valid()) return false;

    /* ---- text ---------------------------------------------------------- */

    const std::string textVertexSource   = ShaderLibrary::preprocess("rhi/ui/ui_text.vs.glsl");
    const std::string textFragmentSource = ShaderLibrary::preprocess("rhi/ui/ui_text.fs.glsl");

    if (textVertexSource.empty() || textFragmentSource.empty()) {
        LOGGER.error("DeviceUiPainter: rhi/ui_text shaders not found");
        return false;
    }

    textShader_ = device_.createShader("ui text", textVertexSource.c_str(),
                                       textFragmentSource.c_str());
    if (!textShader_.valid()) return false;

    /* EVERY FIXED-FUNCTION SETTING COPIED FROM THE SHAPE PIPELINE, on purpose.
     * A label and the plate under it are composited by the same rule, and a
     * text pipeline that blended differently would show up as haloed glyphs on
     * exactly the panels with a translucent fill. Only the shader and the
     * vertex layout differ. */
    PipelineDesc textDesc = desc;
    textDesc.name         = "ui text";
    textDesc.shader       = textShader_;
    textDesc.vertexLayout = uiTexturedLayout();

    textPipeline_ = device_.createPipeline(textDesc);
    if (!textPipeline_.valid()) return false;

    /* POINT, AND CLAMPED, and the filter is not a preference. Every glyph is
     * rasterised at the exact size it is drawn at and placed on a whole texel —
     * the fractional part of its position is baked into the coverage by the
     * phase, not applied at sample time. So each texel maps to one screen pixel
     * and a filter has nothing legitimate to interpolate; bilinear would merely
     * reintroduce, at sample time, the blur the phases exist to remove.
     *
     * The clamp matters at the atlas edge: a repeat would wrap the last row of
     * one glyph onto the first of another, which reads as a stray line of pixels
     * above a letter and gets blamed on the packer. */
    SamplerDesc glyph;
    glyph.minify  = FilterMode::Nearest;
    glyph.magnify = FilterMode::Nearest;
    glyph.mip     = FilterMode::Nearest;
    glyph.wrapU   = WrapMode::ClampToEdge;
    glyph.wrapV   = WrapMode::ClampToEdge;
    glyph.wrapW   = WrapMode::ClampToEdge;

    glyphSampler_ = device_.createSampler(glyph);
    if (!glyphSampler_.valid()) return false;

    /* ---- backdrop blur -------------------------------------------------- */

    const std::string blurVertexSource   = ShaderLibrary::preprocess("rhi/ui/ui_blur.vs.glsl");
    const std::string blurFragmentSource = ShaderLibrary::preprocess("rhi/ui/ui_blur.fs.glsl");

    if (blurVertexSource.empty() || blurFragmentSource.empty()) {
        LOGGER.error("DeviceUiPainter: rhi/ui_blur shaders not found");
        return false;
    }

    blurShader_ = device_.createShader("ui blur", blurVertexSource.c_str(),
                                       blurFragmentSource.c_str());
    if (!blurShader_.valid()) return false;

    PipelineDesc blurDesc = textDesc;
    blurDesc.name   = "ui blur";
    blurDesc.shader = blurShader_;

    blurPipeline_ = device_.createPipeline(blurDesc);
    if (!blurPipeline_.valid()) return false;

    /* TRILINEAR, and that is the whole mechanism rather than a quality setting:
     * the blur IS a mip level, the level is fractional, and a fractional level
     * has to interpolate between the two either side. A nearest mip filter would
     * make the strength dial step through powers of two.
     *
     * CLAMPED, so the edge of the captured region does not wrap around and smear
     * the opposite side of the panel into it. */
    SamplerDesc capture;
    capture.minify  = FilterMode::Linear;
    capture.magnify = FilterMode::Linear;
    capture.mip     = FilterMode::Linear;
    capture.wrapU   = WrapMode::ClampToEdge;
    capture.wrapV   = WrapMode::ClampToEdge;
    capture.wrapW   = WrapMode::ClampToEdge;

    captureSampler_ = device_.createSampler(capture);
    if (!captureSampler_.valid()) return false;

    ready_ = true;
    return true;
}

void DeviceUiPainter::setSurfaceSize(uint32_t width, uint32_t height)
{
    surfaceWidth_  = width;
    surfaceHeight_ = height;
}

bool DeviceUiPainter::ensureCapacity(uint32_t vertexCount, uint32_t indexCount)
{
    if (vertexCount <= vertexCapacity_ && indexCount <= indexCapacity_ && mesh_.valid())
        return true;

    /* GROW WITH HEADROOM, so a UI that gains one widget a frame during a
     * transition does not recreate its buffers every frame on the way. A
     * quarter over the high-water mark settles within a few frames. */
    const uint32_t wantedVertices = std::max(vertexCount + vertexCount / 4u, 1024u);
    const uint32_t wantedIndices  = std::max(indexCount + indexCount / 4u, 2048u);

    if (mesh_.valid())     device_.destroy(mesh_);
    if (vertices_.valid()) device_.destroy(vertices_);
    if (indices_.valid())  device_.destroy(indices_);
    mesh_ = {};

    BufferDesc vertexDesc;
    vertexDesc.name  = "ui vertices";
    vertexDesc.bytes = static_cast<uint64_t>(wantedVertices) * kVertexStride;
    vertexDesc.usage = BufferUsageVertex;

    /* REWRITTEN EVERY FRAME, unlike the world's geometry — so the backend is
     * told to keep it somewhere the CPU can reach cheaply rather than in memory
     * that would have to be staged through on every update. */
    vertexDesc.access = BufferAccess::CpuToGpuPerFrame;

    vertices_ = device_.createBuffer(vertexDesc);
    if (!vertices_.valid()) return false;

    BufferDesc indexDesc;
    indexDesc.name   = "ui indices";
    indexDesc.bytes  = static_cast<uint64_t>(wantedIndices) * sizeof(uint32_t);
    indexDesc.usage  = BufferUsageIndex;
    indexDesc.access = BufferAccess::CpuToGpuPerFrame;

    indices_ = device_.createBuffer(indexDesc);
    if (!indices_.valid()) return false;

    mesh_ = device_.createMesh(uiLayout(), vertices_, wantedVertices,
                               indices_, wantedIndices);
    if (!mesh_.valid()) return false;

    vertexCapacity_ = wantedVertices;
    indexCapacity_  = wantedIndices;
    return true;
}

bool DeviceUiPainter::ensureTexturedCapacity(uint32_t vertexCount, uint32_t indexCount)
{
    static_assert(sizeof(TexturedVertex) == kTexturedVertexStride,
                  "uiTexturedLayout describes TexturedVertex byte for byte");

    if (vertexCount <= texturedVertexCapacity_ && indexCount <= texturedIndexCapacity_
        && texturedMesh_.valid())
        return true;

    /* Same headroom rule as the shapes', with a smaller floor: a screen of HUD
     * is a few hundred glyphs and a handful of panels, where the shape list is a
     * few thousand vertices before anything is typed. */
    const uint32_t wantedVertices = std::max(vertexCount + vertexCount / 4u, 512u);
    const uint32_t wantedIndices  = std::max(indexCount + indexCount / 4u, 768u);

    if (texturedMesh_.valid())     device_.destroy(texturedMesh_);
    if (texturedVertices_.valid()) device_.destroy(texturedVertices_);
    if (texturedIndices_.valid())  device_.destroy(texturedIndices_);
    texturedMesh_ = {};

    BufferDesc vertexDesc;
    vertexDesc.name   = "ui textured vertices";
    vertexDesc.bytes  = static_cast<uint64_t>(wantedVertices) * kTexturedVertexStride;
    vertexDesc.usage  = BufferUsageVertex;
    vertexDesc.access = BufferAccess::CpuToGpuPerFrame;

    texturedVertices_ = device_.createBuffer(vertexDesc);
    if (!texturedVertices_.valid()) return false;

    BufferDesc indexDesc;
    indexDesc.name   = "ui textured indices";
    indexDesc.bytes  = static_cast<uint64_t>(wantedIndices) * sizeof(uint32_t);
    indexDesc.usage  = BufferUsageIndex;
    indexDesc.access = BufferAccess::CpuToGpuPerFrame;

    texturedIndices_ = device_.createBuffer(indexDesc);
    if (!texturedIndices_.valid()) return false;

    texturedMesh_ = device_.createMesh(uiTexturedLayout(), texturedVertices_, wantedVertices,
                                       texturedIndices_, wantedIndices);
    if (!texturedMesh_.valid()) return false;

    texturedVertexCapacity_ = wantedVertices;
    texturedIndexCapacity_  = wantedIndices;
    return true;
}

bool DeviceUiPainter::ensureCaptureTexture(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return false;

    if (captureTexture_.valid() && width <= captureWidth_ && height <= captureHeight_)
        return true;

    /* GROWS ONLY. A panel that shrinks keeps the larger texture, so resizing a
     * window does not churn a GPU allocation on every frame of the drag — the
     * same rule UiPainter::ensureCaptureTexture follows, and the reason the
     * shader has to scale its UVs by the used sub-rectangle. */
    const uint32_t wantedWidth  = std::max(width, captureWidth_);
    const uint32_t wantedHeight = std::max(height, captureHeight_);

    /* THE FULL CHAIN, because the chain is the blur. Levels down to 1x1: the
     * strength is log2 of a pixel radius, and a panel asking for a radius wider
     * than the levels allow would otherwise clamp to a blur that is visibly not
     * what was asked for. */
    uint32_t mips = 1;
    for (uint32_t size = std::max(wantedWidth, wantedHeight); size > 1u; size /= 2u) ++mips;

    TextureDesc desc;
    desc.name      = "ui backdrop capture";
    desc.width     = wantedWidth;
    desc.height    = wantedHeight;
    desc.mipLevels = mips;

    /* RGBA8, matching the backbuffer it is copied from — the UI runs past the
     * tone map, so this is display colour and there is no radiance to preserve.
     * CopyDest states what it is for; every backend but this one needs to know
     * before it allocates. */
    desc.format = TextureFormat::RGBA8;
    desc.usage  = TextureUsageSampled | TextureUsageCopyDest;

    const TextureHandle texture = device_.createTexture(desc);
    if (!texture.valid()) return false;

    if (captureTexture_.valid()) device_.destroy(captureTexture_);
    captureTexture_ = texture;
    captureWidth_   = wantedWidth;
    captureHeight_  = wantedHeight;
    return true;
}

rhi::TextureHandle DeviceUiPainter::textureFor(const GlyphAtlas& atlas)
{
    for (const AtlasTexture& entry : atlasTextures_) {
        if (entry.atlas == &atlas) return entry.texture;
    }

    /* R8, ONE BYTE PER TEXEL. The atlas holds coverage and nothing else — see
     * GlyphAtlas.hpp — so the three channels raylib's font convention spends on
     * a constant 255 are not spent here. The shader reads .r as the alpha. */
    TextureDesc desc;
    desc.name   = "ui glyph atlas";
    desc.width  = static_cast<uint32_t>(atlas.width());
    desc.height = static_cast<uint32_t>(atlas.height());
    desc.format = TextureFormat::R8;
    desc.usage  = TextureUsageSampled;

    const TextureHandle texture = device_.createTexture(desc);
    if (texture.valid()) {
        device_.updateTexture(texture, atlas.coverage().data());
    } else {
        LOGGER.error("DeviceUiPainter: a {}x{} glyph atlas could not be created",
                     atlas.width(), atlas.height());
    }

    /* CACHED EVEN WHEN INVALID, so a device that refused the texture is asked
     * once rather than once per frame per label. */
    atlasTextures_.push_back(AtlasTexture{ &atlas, texture });
    return texture;
}

DeviceUiPainter::TextRange DeviceUiPainter::appendRun(const TextRun& run, const UiFontSet& fonts)
{
    TextRange range;
    range.begin = static_cast<uint32_t>(glyphBatches_.size());

    if (run.text.empty()) return range;

    /* ================= THIS ARITHMETIC IS UiPainter::executeText ===========
     *
     * Ported rather than reinvented, and that is deliberate. Every line of it
     * was arrived at by looking at text on a screen — the whole-pixel snapping,
     * the rounded tracking, the phase carry — and a second derivation would be
     * a second set of subtly different answers with no way to tell which was
     * right except by looking again. Where the two paths differ they are wrong;
     * see the "tuning invented rather than borrowed" note in rhi/MIGRATION.md.
     * =====================================================================*/

    /* Drawn at the size the atlas was RASTERISED at, not the style's. They are
     * the same number at a 100% display scale and differ the moment one is
     * applied; a glyph baked for 13 px and drawn at 13.5 is resampled, which is
     * the one thing per-size atlases exist to prevent. */
    const float drawSize = UiFontSet::rasterSize(run.style.sizePx);

    /* The run's position is the top-left of the LINE BOX; this turns that into
     * the top of the GLYPH box, centring the cap band rather than the whole
     * line — see UiFontSet::runOriginY, which owns that decision for both
     * painters.
     *
     * Y IS SNAPPED, X IS NOT. Vertical subpixel positioning buys nothing: text
     * sits on a baseline and every glyph in a run shares it, so rounding once
     * costs no precision anyone can see. Horizontal is different — that is
     * where fractional letter spacing accumulates — and it is handled per glyph
     * below by choosing a phase rather than by rounding. */
    const float originY = fonts.runOriginY(run.position.y, run.style);

    const std::uint32_t rgba = run.style.colour.toSrgb8();

    /* TRACKING, ROUNDED TO A WHOLE PIXEL, ONCE — and asked for rather than
     * computed, because the LAYOUT above reserved space using the same answer.
     * A painter with its own copy of this rounding is a painter that will one
     * day disagree with the box it was given. See UiFontSet::trackingPx, which
     * carries the reasoning and the bug that produced it. */
    const float tracking = UiFontSet::trackingPx(run.style);

    const GlyphAtlas* batchAtlas = nullptr;

    float pen = run.position.x;
    for (const char character : run.text) {
        /* THE PHASE. Split the wanted position into the whole pixel the quad
         * sits on and the fraction the RASTERISER absorbed. Rounding to the
         * nearest phase can carry into the next pixel, which is why the carry
         * is handled rather than clamped. */
        const float wholeX = std::floor(pen);
        int phase = static_cast<int>(std::round((pen - wholeX) * UiFontSet::kPhaseCount));
        float pixelX = wholeX;
        if (phase >= UiFontSet::kPhaseCount) {
            phase = 0;
            pixelX += 1.0f;
        }

        const GlyphAtlas* atlas = fonts.atlasFor(run.style.weight, run.style.sizePx, phase);
        if (atlas == nullptr) {
            /* NO ATLAS MEANS NO TYPEFACE AT ALL — every weight failed to load,
             * because the fallback to Regular has already happened inside the
             * font set. Counted, so a HUD drawn without its labels says why
             * rather than merely looking wrong. */
            ++skippedCommands_;
            break;
        }

        const GlyphAtlas::Glyph& glyph =
            atlas->glyph(GlyphAtlas::indexOf(static_cast<unsigned char>(character)));

        /* One by construction — the atlas is rasterised at atlasSizeFor(sizePx)
         * and drawn at rasterSize(sizePx) — and kept as a term because the two
         * are separate decisions and a future one of them could move. */
        const float scale = atlas->sizePx() > 0
            ? drawSize / static_cast<float>(atlas->sizePx()) : 1.0f;

        if (glyph.width > 0 && glyph.height > 0) {
            if (atlas != batchAtlas) {
                GlyphBatch batch;
                batch.atlas      = atlas;
                batch.indexBegin = static_cast<uint32_t>(texturedIndexScratch_.size());
                glyphBatches_.push_back(batch);
                batchAtlas = atlas;
                ++range.count;
            }

            /* Whole pixels on both axes. The fractional part of the position is
             * not lost — it is in the coverage, put there by the phase. */
            const float left   = pixelX + static_cast<float>(glyph.offsetX) * scale;
            const float top    = originY + static_cast<float>(glyph.offsetY) * scale;
            const float right  = left + static_cast<float>(glyph.width) * scale;
            const float bottom = top + static_cast<float>(glyph.height) * scale;

            const float texWidth  = static_cast<float>(atlas->width());
            const float texHeight = static_cast<float>(atlas->height());
            const float u0 = static_cast<float>(glyph.x) / texWidth;
            const float v0 = static_cast<float>(glyph.y) / texHeight;
            const float u1 = static_cast<float>(glyph.x + glyph.width) / texWidth;
            const float v1 = static_cast<float>(glyph.y + glyph.height) / texHeight;

            const std::uint32_t base = static_cast<std::uint32_t>(texturedVertexScratch_.size());
            texturedVertexScratch_.push_back(TexturedVertex{ left,  top,    u0, v0, rgba });
            texturedVertexScratch_.push_back(TexturedVertex{ left,  bottom, u0, v1, rgba });
            texturedVertexScratch_.push_back(TexturedVertex{ right, bottom, u1, v1, rgba });
            texturedVertexScratch_.push_back(TexturedVertex{ right, top,    u1, v0, rgba });

            texturedIndexScratch_.push_back(base + 0);
            texturedIndexScratch_.push_back(base + 1);
            texturedIndexScratch_.push_back(base + 2);
            texturedIndexScratch_.push_back(base + 0);
            texturedIndexScratch_.push_back(base + 2);
            texturedIndexScratch_.push_back(base + 3);

            glyphBatches_.back().indexCount += 6;
        }

        /* The pen keeps its FRACTIONAL position. Rounding it here would be the
         * snapping this whole mechanism exists to avoid, and the error would
         * accumulate across the run. */
        pen += static_cast<float>(glyph.advanceX) * scale + tracking;
    }

    return range;
}

DeviceUiPainter::BlurRegion DeviceUiPainter::appendBlur(const UiBackdropBlur& blur)
{
    BlurRegion region;

    /* CLAMPED TO THE SURFACE FIRST. The copy reads the backbuffer, so the region
     * has to be inside it — a panel hanging off the edge of the window would
     * otherwise copy pixels that are undefined rather than empty. */
    const UiRect clamped = blur.rect.intersected(
        { 0.0f, 0.0f, static_cast<float>(surfaceWidth_), static_cast<float>(surfaceHeight_) });

    /* One pixel is not a region: there is nothing to frost and the outline
     * would be degenerate. */
    if (clamped.width <= 1.0f || clamped.height <= 1.0f) return region;

    /* STRENGTH IN PIXELS BECOMES A MIP LEVEL: each level halves the resolution,
     * so a radius of 2^n pixels is level n. Borrowed from
     * UiPainter::executeBackdropBlur rather than re-derived — the dial has been
     * tuned against this curve. */
    region.lod = std::log2(std::max(blur.strengthPx, 1.0f));

    blurOutline_.buildRect(clamped, blur.cornerRadii, kBlurCornerSegments);
    if (blurOutline_.size() < 3) return region;

    region.indexBegin = static_cast<uint32_t>(texturedIndexScratch_.size());

    /* THE UV IS THE POINT'S PLACE ON THE SCREEN, because the capture is the
     * whole screen — see the note on captureTexture_. The scale onto a texture
     * that may be larger than the surface is a push constant, decided at draw
     * time when the size is final. */
    const float surfaceW = static_cast<float>(surfaceWidth_);
    const float surfaceH = static_cast<float>(surfaceHeight_);

    const std::uint32_t base = static_cast<std::uint32_t>(texturedVertexScratch_.size());
    for (std::size_t index = 0; index < blurOutline_.size(); ++index) {
        const Vec2 point = blurOutline_.position(index);

        const float u = point.x / surfaceW;

        /* FLIPPED: the copy puts the screen's BOTTOM row at texture row 0,
         * because GL's framebuffer origin is bottom-left and the UI's is
         * top-left. Getting this wrong mirrors the frosting vertically, which
         * over a blurred backdrop is almost invisible until the panel sits over
         * something with a horizon in it. */
        const float v = 1.0f - point.y / surfaceH;

        texturedVertexScratch_.push_back(TexturedVertex{ point.x, point.y, u, v, 0xFFFFFFFFu });
    }

    /* Fan from point 0 — the outline is convex, so this is a valid
     * triangulation without needing to prove anything about it. */
    for (std::uint32_t index = 1; index + 1 < static_cast<std::uint32_t>(blurOutline_.size());
         ++index) {
        texturedIndexScratch_.push_back(base);
        texturedIndexScratch_.push_back(base + index);
        texturedIndexScratch_.push_back(base + index + 1);
    }

    region.indexCount =
        static_cast<uint32_t>(texturedIndexScratch_.size()) - region.indexBegin;
    return region;
}

void DeviceUiPainter::buildTexturedGeometry(const UiDrawList& drawList, const UiFontSet& fonts)
{
    texturedVertexScratch_.clear();
    texturedIndexScratch_.clear();
    glyphBatches_.clear();
    textRanges_.clear();
    blurRegions_.clear();

    /* IN COMMAND ORDER, one entry per Text and one per BackdropBlur command. The
     * draw loop below walks the same list and consumes these with a counter,
     * which is what keeps the two in step without either of them indexing by
     * payload — a run referenced twice would otherwise be drawn from one range
     * in two places. */
    for (const UiCommand& command : drawList.commands()) {
        if (command.kind == UiCommandKind::Text) {
            textRanges_.push_back(appendRun(drawList.textRuns()[command.payloadIndex], fonts));
        } else if (command.kind == UiCommandKind::BackdropBlur) {
            blurRegions_.push_back(appendBlur(drawList.backdropBlurs()[command.payloadIndex]));
        }
    }

    /* THE UPLOADS HAPPEN HERE, OUTSIDE ANY PASS. Creating a texture between
     * beginPass and endPass is legal on GL and is not on the explicit backends,
     * and it would also disturb the texture unit the pass is binding through —
     * so the atlases a frame needs are resolved before a single draw is
     * recorded, and the pass loop only binds what it is handed. */
    for (GlyphBatch& batch : glyphBatches_) {
        if (batch.atlas != nullptr) batch.texture = textureFor(*batch.atlas);
    }

    /* THE CAPTURE, SCREEN SIZED, once — see captureTexture_ for why the whole
     * screen rather than each panel's own rectangle. Made only when something in
     * this frame is actually frosted, so a HUD with no blur panels never
     * allocates it at all. */
    bool anyBlur = false;
    for (const BlurRegion& region : blurRegions_) anyBlur = anyBlur || region.indexCount != 0;

    if (anyBlur && !ensureCaptureTexture(surfaceWidth_, surfaceHeight_)) {
        /* Every frosted panel this frame loses its frosting and says so. The
         * fill underneath is still drawn, which is the degradation the raylib
         * painter makes for the same failure.
         *
         * EMPTIED, NOT CLEARED: the draw loop consumes one of these per
         * BackdropBlur command by position, so removing them would shift every
         * later panel onto the wrong region. */
        for (BlurRegion& region : blurRegions_) {
            if (region.indexCount == 0) continue;
            region.indexCount = 0;
            ++skippedCommands_;
        }
    }
}

void DeviceUiPainter::draw(const UiDrawList& drawList, const UiFontSet& fonts)
{
    CW_PROFILE_ZONE_N("ui");

    skippedCommands_ = 0;
    if (!ready_ || drawList.empty()) return;
    if (surfaceWidth_ == 0 || surfaceHeight_ == 0) return;

    /* THE FONT SET'S ATLASES MAY HAVE BEEN REBUILT, and the glyph textures are
     * keyed by atlas ADDRESS. A map's nodes are freed on unload and new ones
     * can land at the same addresses, so without this a reload draws labels
     * from whatever typeface happened to occupy that memory before. */
    if (fonts.generation() != fontGeneration_) {
        releaseAtlasTextures();
        fontGeneration_ = fonts.generation();
    }

    buildTexturedGeometry(drawList, fonts);

    const std::vector<UiVertex>& vertices = drawList.vertices();
    const std::vector<uint32_t>& indices  = drawList.indices();

    if (!ensureCapacity(static_cast<uint32_t>(vertices.size()),
                        static_cast<uint32_t>(indices.size())))
        return;

    const bool hasTextured = !texturedIndexScratch_.empty();
    if (hasTextured
        && !ensureTexturedCapacity(static_cast<uint32_t>(texturedVertexScratch_.size()),
                                   static_cast<uint32_t>(texturedIndexScratch_.size())))
        return;

    /* ONE UPLOAD EACH, BEFORE THE FIRST PASS OPENS. Every array is contiguous by
     * construction, and updating a buffer a pass is already reading is the kind
     * of hazard that works on one driver and tears on another. */
    if (!vertices.empty())
        device_.updateBuffer(vertices_, vertices.data(),
                             vertices.size() * sizeof(UiVertex), 0);
    if (!indices.empty())
        device_.updateBuffer(indices_, indices.data(),
                             indices.size() * sizeof(uint32_t), 0);

    if (hasTextured) {
        device_.updateBuffer(texturedVertices_, texturedVertexScratch_.data(),
                             texturedVertexScratch_.size() * kTexturedVertexStride, 0);
        device_.updateBuffer(texturedIndices_, texturedIndexScratch_.data(),
                             texturedIndexScratch_.size() * sizeof(uint32_t), 0);
    }

    /* THE BACKBUFFER, LOADED. An attachment carrying no texture is the screen;
     * loading rather than clearing is what puts the UI ON the resolved scene
     * rather than instead of it — and it is what lets the pass be reopened after
     * a blur without losing everything drawn before it. */
    PassDesc pass;
    pass.name = "ui";
    pass.colours[0].load  = LoadAction::Load;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder* encoder = &device_.beginPass(pass);

    /* Which pipeline is bound, so a screen of shapes with three labels on it
     * costs six binds rather than one per command. RESET WHENEVER THE PASS IS
     * REOPENED: a new encoder has bound nothing, and remembering across the
     * split would skip the bind and draw the panel with whatever the driver
     * still had. */
    enum class Bound { None, Shapes, Text, Blur };
    Bound bound = Bound::None;

    /* The surface size, for the vertex stage's pixels-to-clip conversion, plus
     * whatever the bound pipeline needs after it.
     *
     * PUSHED AFTER EVERY PIPELINE BIND, not once per pass. Push constants are
     * emulated on GL as a uniform at location 0 of the CURRENT PROGRAM, so
     * switching pipeline abandons them — and a text pipeline that never
     * received the surface size divides by zero and puts every glyph at
     * infinity, which draws nothing and looks exactly like a missing font. */
    float push[8] = { static_cast<float>(surfaceWidth_),
                      static_cast<float>(surfaceHeight_), 0.0f, 0.0f,
                      0.0f, 0.0f, 0.0f, 0.0f };

    std::uint32_t textCommandIndex = 0;
    std::uint32_t blurCommandIndex = 0;

    for (const UiCommand& command : drawList.commands()) {
        /* CONSUMED FIRST AND UNCONDITIONALLY, before any of the reasons below
         * to skip this command — the counters track the command list, not the
         * draws, and a range left unconsumed would shift every label after a
         * clipped-away one onto the wrong text. */
        TextRange range;
        BlurRegion blur;
        if (command.kind == UiCommandKind::Text) {
            if (textCommandIndex < textRanges_.size()) range = textRanges_[textCommandIndex];
            ++textCommandIndex;
        } else if (command.kind == UiCommandKind::BackdropBlur) {
            if (blurCommandIndex < blurRegions_.size()) blur = blurRegions_[blurCommandIndex];
            ++blurCommandIndex;
        }

        if (command.kind == UiCommandKind::Triangles && command.indexCount == 0) continue;
        if (command.kind == UiCommandKind::Text && range.count == 0) continue;
        if (command.kind == UiCommandKind::BackdropBlur && blur.indexCount == 0) continue;

        /* THE CLIP, AS A SCISSOR. Every command carries the rectangle it was
         * recorded under — already intersected with its parents by
         * UiDrawList::pushClip, so honouring this one honours the whole stack.
         *
         * CLAMPED TO THE SURFACE because the unbounded default is a rectangle a
         * million pixels wide, and a scissor box with a negative origin or a
         * width past the target is a GL error on some drivers and silently
         * empty on others. */
        const float left   = std::max(command.clip.left(), 0.0f);
        const float top    = std::max(command.clip.top(), 0.0f);
        const float right  = std::min(command.clip.right(), static_cast<float>(surfaceWidth_));
        const float bottom = std::min(command.clip.bottom(), static_cast<float>(surfaceHeight_));

        if (right <= left || bottom <= top) continue;   /* clipped away entirely */

        /* ---- the frosted panel, which splits the pass ---------------------
         *
         * Everything appended before this command has to be ON the backbuffer
         * before it can be copied back — which is what ending the pass
         * guarantees, and it is why a panel frosts the UI beneath it rather than
         * only the scene. The raylib painter flushes rlgl's batch here for
         * exactly the same reason. */
        if (command.kind == UiCommandKind::BackdropBlur) {
            device_.endPass(*encoder);

            /* THE WHOLE SCREEN, ORIGIN AT THE BOTTOM LEFT — see captureTexture_
             * for why the whole screen, and setScissor above for the same
             * convention. Nothing to flip when the rectangle is everything. */
            const bool copied = device_.copyBackbufferToTexture(captureTexture_, 0, 0,
                                                                surfaceWidth_, surfaceHeight_);
            if (copied) device_.generateMips(captureTexture_);
            else        ++skippedCommands_;

            encoder = &device_.beginPass(pass);
            bound = Bound::None;

            if (!copied) continue;

            encoder->setScissor(left, static_cast<float>(surfaceHeight_) - bottom,
                                right - left, bottom - top);

            encoder->bindPipeline(blurPipeline_);
            push[2] = blur.lod;

            /* The screen's corner of a texture that only grows — a window
             * resized smaller keeps the larger capture, so this is not always
             * one. See appendBlur on why it is decided here. */
            push[4] = static_cast<float>(surfaceWidth_) / static_cast<float>(captureWidth_);
            push[5] = static_cast<float>(surfaceHeight_) / static_cast<float>(captureHeight_);
            encoder->pushConstants(push, sizeof push);

            encoder->bindTexture(0, captureTexture_, captureSampler_);
            encoder->drawIndexed(texturedMesh_, blur.indexCount, blur.indexBegin);

            bound = Bound::Blur;
            continue;
        }

        /* Y FLIPS HERE, and only here. The UI measures from the top and the
         * scissor box is measured from the bottom, so a panel clipped near the
         * top of the screen would otherwise be clipped near the bottom — which
         * looks like the clip rectangle being ignored rather than inverted. */
        encoder->setScissor(left, static_cast<float>(surfaceHeight_) - bottom,
                            right - left, bottom - top);

        if (command.kind == UiCommandKind::Triangles) {
            if (bound != Bound::Shapes) {
                encoder->bindPipeline(pipeline_);
                encoder->pushConstants(push, sizeof push);
                bound = Bound::Shapes;
            }
            encoder->drawIndexed(mesh_, command.indexCount, command.indexBegin);
            continue;
        }

        if (bound != Bound::Text) {
            encoder->bindPipeline(textPipeline_);
            encoder->pushConstants(push, sizeof push);
            bound = Bound::Text;
        }

        for (std::uint32_t i = 0; i < range.count; ++i) {
            const GlyphBatch& batch = glyphBatches_[range.begin + i];
            if (batch.indexCount == 0 || !batch.texture.valid()) continue;

            encoder->bindTexture(0, batch.texture, glyphSampler_);
            encoder->drawIndexed(texturedMesh_, batch.indexCount, batch.indexBegin);
        }
    }

    device_.endPass(*encoder);
}

}  // namespace cromwell::ui
