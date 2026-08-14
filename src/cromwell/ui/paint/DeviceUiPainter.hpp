/* DeviceUiPainter.hpp — the UI kit, drawn through IRenderDevice.
 *
 * SINGLE RESPONSIBILITY: execute a UiDrawList against the render device, and
 * own the per-frame buffers and glyph textures that needs.
 *
 * ================= WHY THIS EXISTS BESIDE UiPainter =======================
 *
 * The same bargain the rest of the port makes: UiPainter draws the kit through
 * rlgl and is untouched, this draws it through the device, and one of the two
 * is deleted at parity. Both take the same UiDrawList, so the widgets, the
 * layout and the shapes above them cannot tell which is running — which is the
 * property that made the UI far cheaper to port than the renderer.
 *
 * ====================== WHAT IS NOT CONVERTED YET =========================
 *
 * BACKDROP BLUR, and only that. It needs to READ the target it is drawing into
 * — copy a region, mip it, sample it back. The device can express that; it
 * wants a scratch texture and a second pipeline, and a panel without its
 * frosting still shows its fill, so it is last. It is named in the switch and
 * counted rather than silently dropped.
 *
 * ========================= TWO PIPELINES, NOT ONE =========================
 *
 * Shapes and text are drawn by different pipelines over different vertex
 * buffers, and that split is the whole shape of this file.
 *
 * A shape here is exact vertex geometry with a feathered edge — see
 * ui/shape/Shapes.hpp for why that beats a rounded-box shader at these sizes —
 * so it has a position and a colour and nothing else. Text is the only thing in
 * the kit that samples a texture. Giving every shape vertex a UV to unify them
 * would add eight bytes to every corner of every panel to carry a constant, and
 * branching in the fragment shader would put the decision on every fragment in
 * the frame; a second pipeline puts it on the handful of commands that are
 * actually text.
 *
 * PAINTER'S ORDER SURVIVES THE SPLIT, which is the part worth being careful
 * about. The pass walks the command list in order and switches pipeline
 * whenever the kind changes — a label between two plates costs two switches,
 * not a reordering. Batching text separately from the shapes around it would
 * be faster and would draw every label on top of the panel that was meant to
 * cover it.
 *
 * ================= ONE UPLOAD A FRAME, FOUR GROWING BUFFERS ===============
 *
 * The draw list arrives as two contiguous arrays, so this uploads each once and
 * then issues a draw per command. Glyph quads are built into two more arrays
 * before the pass opens and uploaded the same way — one pass over the text runs
 * rather than a buffer update between draws.
 *
 * All four grow to the high-water mark and stay there — a UI redrawn every
 * frame settles after a few frames and then neither allocates nor reallocates,
 * which is the same steady state UiDrawList itself is built for.
 *
 * They are recreated rather than resized when they grow, because a device
 * buffer has no resize: the old one is destroyed and a bigger one made, which
 * happens a handful of times at startup and never again.
 */
#pragma once

#include "cromwell/rhi/Handles.hpp"
#include "cromwell/ui/paint/IUiPainter.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {
namespace rhi { class ICommandEncoder; class IRenderDevice; }
}  // namespace cromwell

namespace cromwell::ui {

class GlyphAtlas;
class UiDrawList;
struct TextRun;

class DeviceUiPainter final : public IUiPainter {
public:
    explicit DeviceUiPainter(rhi::IRenderDevice& device);
    ~DeviceUiPainter() override;

    DeviceUiPainter(const DeviceUiPainter&) = delete;
    DeviceUiPainter& operator=(const DeviceUiPainter&) = delete;

    /* Loads the shaders and builds the pipelines. False means the UI cannot be
     * drawn and the caller should say so rather than present a blank HUD. */
    bool initialise();

    /* THE TARGET'S SIZE IN PIXELS, which the vertex stage needs to turn screen
     * coordinates into clip space and which nothing in a draw list carries.
     * Set it before draw() whenever the surface changes; it is remembered. */
    void setSurfaceSize(uint32_t width, uint32_t height);

    /* OPENS ITS OWN PASS, targeting the backbuffer. The UI is the last thing in
     * a frame and draws in display colour over a resolved scene, so it loads
     * what is there rather than clearing it. */
    void draw(const UiDrawList& drawList, const UiFontSet& fonts) override;

    void release() override;

    /* Commands skipped because their kind is not converted, counted over the
     * last draw. A diagnostic: "the HUD has no frosting" and "the HUD is not
     * drawing" look identical on screen and have different causes.
     *
     * A text run whose weight could not be rasterised counts here too. It is
     * the same class of fact — something in the list did not reach the screen —
     * and a HUD silently missing one label is exactly what this exists to make
     * visible. */
    int skippedCommands() const { return skippedCommands_; }

private:
    /* One glyph quad corner. A DIFFERENT VERTEX FROM THE SHAPES', which is the
     * point of the two-pipeline split — see the header. Twenty bytes: position,
     * atlas UV, packed colour, in the order ui_text.vs.glsl declares them. */
    struct TextVertex {
        float         x = 0.0f;
        float         y = 0.0f;
        float         u = 0.0f;
        float         v = 0.0f;
        std::uint32_t rgba = 0xFFFFFFFFu;
    };

    /* A stretch of glyph indices sharing one atlas texture.
     *
     * A RUN CAN GENUINELY SWITCH ATLAS MID-WORD. Every subpixel phase of a
     * given weight and size is its own atlas, so a run whose pen lands on
     * different fractions from one character to the next draws from several.
     * With UiFontSet::kPhaseCount at one that never happens today, and the
     * batching is written for it anyway because the alternative is a bug that
     * appears the day someone raises that constant and shows up as a word drawn
     * in the wrong glyphs. */
    struct GlyphBatch {
        const GlyphAtlas*  atlas = nullptr;
        rhi::TextureHandle texture;
        std::uint32_t      indexBegin = 0;
        std::uint32_t      indexCount = 0;
    };

    /* The batches belonging to one Text command, in the order the commands were
     * walked — so the draw loop consumes these with a counter rather than
     * indexing by payload. */
    struct TextRange {
        std::uint32_t begin = 0;
        std::uint32_t count = 0;
    };

    /* An atlas uploaded to the device. KEYED BY ADDRESS, which is sound because
     * UiFontSet caches atlases in a std::map and a map keeps its elements put —
     * see the note on cacheKey there. The generation guard below is what makes
     * the address safe across a font reload, when the old nodes are freed and
     * new ones can land at the same addresses. */
    struct AtlasTexture {
        const GlyphAtlas*  atlas = nullptr;
        rhi::TextureHandle texture;
    };

    /* Grows the shape vertex and index buffers to hold at least this much,
     * keeping them otherwise. False if either could not be created. */
    bool ensureCapacity(uint32_t vertexCount, uint32_t indexCount);

    /* The same, for the glyph quads. */
    bool ensureTextCapacity(uint32_t vertexCount, uint32_t indexCount);

    /* Builds every text run's quads into the scratch arrays and fills the batch
     * lists. One pass before the render pass opens, so the buffers are uploaded
     * once rather than updated between draws — updating a buffer a pass is
     * already reading is the kind of hazard that works on one driver and tears
     * on another. */
    void buildTextGeometry(const UiDrawList& drawList, const UiFontSet& fonts);

    /* Appends one run's quads, returning the range of batches it produced. */
    TextRange appendRun(const TextRun& run, const UiFontSet& fonts);

    /* The device texture for this atlas, uploading it on first use. Invalid
     * when it could not be created, which makes the run draw nothing. */
    rhi::TextureHandle textureFor(const GlyphAtlas& atlas);

    /* Drops every cached glyph texture. Called when the font set says its
     * atlases have been rebuilt, and by release(). */
    void releaseAtlasTextures();

    rhi::IRenderDevice& device_;

    rhi::ShaderHandle   shader_;
    rhi::PipelineHandle pipeline_;

    rhi::BufferHandle vertices_;
    rhi::BufferHandle indices_;
    rhi::MeshHandle   mesh_;

    uint32_t vertexCapacity_ = 0;
    uint32_t indexCapacity_ = 0;

    rhi::ShaderHandle   textShader_;
    rhi::PipelineHandle textPipeline_;
    rhi::SamplerHandle  glyphSampler_;

    rhi::BufferHandle textVertices_;
    rhi::BufferHandle textIndices_;
    rhi::MeshHandle   textMesh_;

    uint32_t textVertexCapacity_ = 0;
    uint32_t textIndexCapacity_ = 0;

    /* KEPT ACROSS FRAMES FOR THEIR CAPACITY, cleared rather than freed — the
     * same steady state the draw list itself reaches, and the reason a HUD
     * redrawn sixty times a second allocates nothing after the first few. */
    std::vector<TextVertex>    textVertexScratch_;
    std::vector<std::uint32_t> textIndexScratch_;
    std::vector<GlyphBatch>    glyphBatches_;
    std::vector<TextRange>     textRanges_;

    /* A handful of entries — one per (weight, size, phase) the UI has actually
     * asked for, which is five or six across the whole kit. A vector scanned
     * linearly beats a map at that size and this is cold code either way. */
    std::vector<AtlasTexture> atlasTextures_;
    std::uint32_t             fontGeneration_ = 0;

    uint32_t surfaceWidth_ = 0;
    uint32_t surfaceHeight_ = 0;

    int  skippedCommands_ = 0;
    bool ready_ = false;
};

}  // namespace cromwell::ui
