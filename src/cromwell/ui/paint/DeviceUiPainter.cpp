#include "cromwell/ui/paint/DeviceUiPainter.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/paint/GlyphAtlas.hpp"

/* RAYLIB ARRIVES THROUGH THIS, and only through this: UiFontSet still declares
 * a raylib Font for the painter it is being replaced by. Nothing below names
 * one — the text path here goes through GlyphAtlas, which is neutral by
 * construction — and the include stops being transitive the day UiPainter is
 * deleted at parity. */
#include "cromwell/ui/paint/UiFontSet.hpp"

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

/* The glyph vertex: position, atlas UV, packed colour, twenty bytes. Same
 * reasoning as kVertexStride — stated once, beside the layout that depends on
 * it, and checked against the struct in ensureTextCapacity where the buffer is
 * sized. (The check lives in a member function because TextVertex is private,
 * which is where it belongs anyway: the assert and the allocation it protects
 * are the same line of reasoning.) */
constexpr uint32_t kTextVertexStride = 20;

VertexLayout uiTextLayout()
{
    VertexLayout layout;
    layout.stride = kTextVertexStride;
    layout.attributeCount = 3;
    layout.attributes[0] = { 0, 0,  VertexFormat::Float2 };            /* position */
    layout.attributes[1] = { 1, 8,  VertexFormat::Float2 };            /* atlas uv */
    layout.attributes[2] = { 2, 16, VertexFormat::UByte4Normalised };  /* colour   */
    return layout;
}

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

    if (textMesh_.valid())     device_.destroy(textMesh_);
    if (textVertices_.valid()) device_.destroy(textVertices_);
    if (textIndices_.valid())  device_.destroy(textIndices_);
    if (textPipeline_.valid()) device_.destroy(textPipeline_);
    if (textShader_.valid())   device_.destroy(textShader_);
    if (glyphSampler_.valid()) device_.destroy(glyphSampler_);

    mesh_ = {};
    vertices_ = {};
    indices_ = {};
    pipeline_ = {};
    shader_ = {};

    textMesh_ = {};
    textVertices_ = {};
    textIndices_ = {};
    textPipeline_ = {};
    textShader_ = {};
    glyphSampler_ = {};

    vertexCapacity_ = 0;
    indexCapacity_ = 0;
    textVertexCapacity_ = 0;
    textIndexCapacity_ = 0;
    ready_ = false;
}

bool DeviceUiPainter::initialise()
{
    if (ready_) return true;

    const std::string vertexSource   = ShaderLibrary::preprocess("rhi/ui.vs.glsl");
    const std::string fragmentSource = ShaderLibrary::preprocess("rhi/ui.fs.glsl");

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

    const std::string textVertexSource   = ShaderLibrary::preprocess("rhi/ui_text.vs.glsl");
    const std::string textFragmentSource = ShaderLibrary::preprocess("rhi/ui_text.fs.glsl");

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
    textDesc.vertexLayout = uiTextLayout();

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

bool DeviceUiPainter::ensureTextCapacity(uint32_t vertexCount, uint32_t indexCount)
{
    static_assert(sizeof(TextVertex) == kTextVertexStride,
                  "uiTextLayout describes TextVertex byte for byte");

    if (vertexCount <= textVertexCapacity_ && indexCount <= textIndexCapacity_
        && textMesh_.valid())
        return true;

    /* Same headroom rule as the shapes', with a smaller floor: a screen of HUD
     * is a few hundred glyphs, where the shape list is a few thousand vertices
     * before anything is typed. */
    const uint32_t wantedVertices = std::max(vertexCount + vertexCount / 4u, 512u);
    const uint32_t wantedIndices  = std::max(indexCount + indexCount / 4u, 768u);

    if (textMesh_.valid())     device_.destroy(textMesh_);
    if (textVertices_.valid()) device_.destroy(textVertices_);
    if (textIndices_.valid())  device_.destroy(textIndices_);
    textMesh_ = {};

    BufferDesc vertexDesc;
    vertexDesc.name   = "ui text vertices";
    vertexDesc.bytes  = static_cast<uint64_t>(wantedVertices) * kTextVertexStride;
    vertexDesc.usage  = BufferUsageVertex;
    vertexDesc.access = BufferAccess::CpuToGpuPerFrame;

    textVertices_ = device_.createBuffer(vertexDesc);
    if (!textVertices_.valid()) return false;

    BufferDesc indexDesc;
    indexDesc.name   = "ui text indices";
    indexDesc.bytes  = static_cast<uint64_t>(wantedIndices) * sizeof(uint32_t);
    indexDesc.usage  = BufferUsageIndex;
    indexDesc.access = BufferAccess::CpuToGpuPerFrame;

    textIndices_ = device_.createBuffer(indexDesc);
    if (!textIndices_.valid()) return false;

    textMesh_ = device_.createMesh(uiTextLayout(), textVertices_, wantedVertices,
                                   textIndices_, wantedIndices);
    if (!textMesh_.valid()) return false;

    textVertexCapacity_ = wantedVertices;
    textIndexCapacity_  = wantedIndices;
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

    /* The run's position is the top-left of the LINE BOX and glyphs are placed
     * from the top of the GLYPH box, so the difference is split above and
     * below — see the note on line height in UiFontSet.hpp.
     *
     * Y IS SNAPPED, X IS NOT. Vertical subpixel positioning buys nothing: text
     * sits on a baseline and every glyph in a run shares it, so rounding once
     * costs no precision anyone can see. Horizontal is different — that is
     * where fractional letter spacing accumulates — and it is handled per glyph
     * below by choosing a phase rather than by rounding. */
    const float lineHeight = fonts.lineHeight(run.style);
    const float originY = std::round(run.position.y + (lineHeight - drawSize) * 0.5f);

    const std::uint32_t rgba = run.style.colour.toSrgb8();

    /* TRACKING, ROUNDED TO A WHOLE PIXEL, ONCE.
     *
     * FreeType's advanceX is already an integer, so letter spacing is the ONLY
     * source of fractional drift in the pen - and the kit's shouted styles
     * track at 1.8, 2.1 and 2.4 px. Accumulated and rounded per glyph, 2.4
     * comes out as alternating 2s and 3s, which the eye reads as ragged rather
     * than as tracking being 0.4 px off. Rounding here makes every gap
     * identical, and as a side effect every pen position lands on a whole pixel
     * by construction. See study/topics/surfaces/text_rendering.md section 3. */
    const float tracking = std::round(run.style.letterSpacingPx * drawSize
                                      / std::max(run.style.sizePx, 0.001f));

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
                batch.indexBegin = static_cast<uint32_t>(textIndexScratch_.size());
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

            const std::uint32_t base = static_cast<std::uint32_t>(textVertexScratch_.size());
            textVertexScratch_.push_back(TextVertex{ left,  top,    u0, v0, rgba });
            textVertexScratch_.push_back(TextVertex{ left,  bottom, u0, v1, rgba });
            textVertexScratch_.push_back(TextVertex{ right, bottom, u1, v1, rgba });
            textVertexScratch_.push_back(TextVertex{ right, top,    u1, v0, rgba });

            textIndexScratch_.push_back(base + 0);
            textIndexScratch_.push_back(base + 1);
            textIndexScratch_.push_back(base + 2);
            textIndexScratch_.push_back(base + 0);
            textIndexScratch_.push_back(base + 2);
            textIndexScratch_.push_back(base + 3);

            glyphBatches_.back().indexCount += 6;
        }

        /* The pen keeps its FRACTIONAL position. Rounding it here would be the
         * snapping this whole mechanism exists to avoid, and the error would
         * accumulate across the run. */
        pen += static_cast<float>(glyph.advanceX) * scale + tracking;
    }

    return range;
}

void DeviceUiPainter::buildTextGeometry(const UiDrawList& drawList, const UiFontSet& fonts)
{
    textVertexScratch_.clear();
    textIndexScratch_.clear();
    glyphBatches_.clear();
    textRanges_.clear();

    /* IN COMMAND ORDER, one entry per Text command. The draw loop below walks
     * the same list and consumes these with a counter, which is what keeps the
     * two in step without either of them indexing by payload — a run referenced
     * twice would otherwise be drawn from one range in two places. */
    for (const UiCommand& command : drawList.commands()) {
        if (command.kind != UiCommandKind::Text) continue;
        textRanges_.push_back(appendRun(drawList.textRuns()[command.payloadIndex], fonts));
    }

    /* THE UPLOADS HAPPEN HERE, OUTSIDE ANY PASS. Creating a texture between
     * beginPass and endPass is legal on GL and is not on the explicit backends,
     * and it would also disturb the texture unit the pass is binding through —
     * so the atlases a frame needs are resolved before a single draw is
     * recorded, and the pass loop only binds what it is handed. */
    for (GlyphBatch& batch : glyphBatches_) {
        if (batch.atlas != nullptr) batch.texture = textureFor(*batch.atlas);
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

    buildTextGeometry(drawList, fonts);

    const std::vector<UiVertex>& vertices = drawList.vertices();
    const std::vector<uint32_t>& indices  = drawList.indices();

    if (!ensureCapacity(static_cast<uint32_t>(vertices.size()),
                        static_cast<uint32_t>(indices.size())))
        return;

    const bool hasText = !textIndexScratch_.empty();
    if (hasText && !ensureTextCapacity(static_cast<uint32_t>(textVertexScratch_.size()),
                                       static_cast<uint32_t>(textIndexScratch_.size())))
        return;

    /* ONE UPLOAD EACH, BEFORE THE PASS OPENS. Every array is contiguous by
     * construction, and updating a buffer a pass is already reading is the kind
     * of hazard that works on one driver and tears on another. */
    if (!vertices.empty())
        device_.updateBuffer(vertices_, vertices.data(),
                             vertices.size() * sizeof(UiVertex), 0);
    if (!indices.empty())
        device_.updateBuffer(indices_, indices.data(),
                             indices.size() * sizeof(uint32_t), 0);

    if (hasText) {
        device_.updateBuffer(textVertices_, textVertexScratch_.data(),
                             textVertexScratch_.size() * kTextVertexStride, 0);
        device_.updateBuffer(textIndices_, textIndexScratch_.data(),
                             textIndexScratch_.size() * sizeof(uint32_t), 0);
    }

    /* THE BACKBUFFER, LOADED. An attachment carrying no texture is the screen;
     * loading rather than clearing is what puts the UI ON the resolved scene
     * rather than instead of it. */
    PassDesc pass;
    pass.name = "ui";
    pass.colours[0].load  = LoadAction::Load;
    pass.colours[0].store = StoreAction::Store;
    pass.colourCount = 1;

    ICommandEncoder& encoder = device_.beginPass(pass);

    /* The surface size, for the vertex stage's pixels-to-clip conversion.
     *
     * PUSHED AFTER EVERY PIPELINE BIND, not once per pass. Push constants are
     * emulated on GL as a uniform at location 0 of the CURRENT PROGRAM, so
     * switching pipeline abandons them — and a text pipeline that never
     * received the surface size divides by zero and puts every glyph at
     * infinity, which draws nothing and looks exactly like a missing font. */
    const float push[4] = { static_cast<float>(surfaceWidth_),
                            static_cast<float>(surfaceHeight_), 0.0f, 0.0f };

    /* Which pipeline is bound, so a screen of shapes with three labels on it
     * costs six binds rather than one per command. */
    enum class Bound { None, Shapes, Text };
    Bound bound = Bound::None;

    std::uint32_t textCommandIndex = 0;

    for (const UiCommand& command : drawList.commands()) {
        /* CONSUMED FIRST AND UNCONDITIONALLY, before any of the reasons below
         * to skip this command — the counter tracks the command list, not the
         * draws, and a range left unconsumed would shift every label after a
         * clipped-away one onto the wrong text. */
        TextRange range;
        if (command.kind == UiCommandKind::Text) {
            if (textCommandIndex < textRanges_.size()) {
                range = textRanges_[textCommandIndex];
            }
            ++textCommandIndex;
        }

        if (command.kind == UiCommandKind::BackdropBlur) {
            /* COUNTED, NOT IGNORED. Backdrop blur is not converted; saying how
             * many were dropped is what separates "the HUD has no frosting yet"
             * from "the HUD is not drawing". */
            ++skippedCommands_;
            continue;
        }

        if (command.kind == UiCommandKind::Triangles && command.indexCount == 0) continue;
        if (command.kind == UiCommandKind::Text && range.count == 0) continue;

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

        /* Y FLIPS HERE, and only here. The UI measures from the top and the
         * scissor box is measured from the bottom, so a panel clipped near the
         * top of the screen would otherwise be clipped near the bottom — which
         * looks like the clip rectangle being ignored rather than inverted. */
        encoder.setScissor(left, static_cast<float>(surfaceHeight_) - bottom,
                           right - left, bottom - top);

        if (command.kind == UiCommandKind::Triangles) {
            if (bound != Bound::Shapes) {
                encoder.bindPipeline(pipeline_);
                encoder.pushConstants(push, sizeof push);
                bound = Bound::Shapes;
            }
            encoder.drawIndexed(mesh_, command.indexCount, command.indexBegin);
            continue;
        }

        if (bound != Bound::Text) {
            encoder.bindPipeline(textPipeline_);
            encoder.pushConstants(push, sizeof push);
            bound = Bound::Text;
        }

        for (std::uint32_t i = 0; i < range.count; ++i) {
            const GlyphBatch& batch = glyphBatches_[range.begin + i];
            if (batch.indexCount == 0 || !batch.texture.valid()) continue;

            encoder.bindTexture(0, batch.texture, glyphSampler_);
            encoder.drawIndexed(textMesh_, batch.indexCount, batch.indexBegin);
        }
    }

    device_.endPass(encoder);
}

}  // namespace cromwell::ui
