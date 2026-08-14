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
 * ================= UNTEXTURED AND TEXTURED, NOT ONE OF EACH ===============
 *
 * Two vertex buffers and three pipelines, split along whether a thing samples
 * anything — which is the split the draw list itself already has.
 *
 * A shape is exact vertex geometry with a feathered edge — see ui/shape/Shapes.hpp
 * for why that beats a rounded-box shader at these sizes — so it is a position
 * and a colour, twelve bytes, and nothing samples. Glyph quads and frosted
 * regions both carry a UV as well, so both are the same twenty bytes and both
 * live in the SAME buffer; only their shader differs, which is precisely what a
 * pipeline is for. Giving every shape vertex a UV to unify all three would add
 * eight bytes to every corner of every panel to carry a constant, and branching
 * in the fragment shader would put the decision on every fragment in the frame.
 *
 * PAINTER'S ORDER SURVIVES THE SPLIT, which is the part worth being careful
 * about. The pass walks the command list in order and switches pipeline
 * whenever the kind changes — a label between two plates costs two switches,
 * not a reordering. Batching text separately from the shapes around it would
 * be faster and would draw every label on top of the panel that was meant to
 * cover it.
 *
 * ================= A FROSTED PANEL SPLITS THE PASS ========================
 *
 * The blur reads the backbuffer it is drawing into, and a copy out of a render
 * target cannot happen inside a render pass on three of the four target
 * backends — see IRenderDevice::copyBackbufferToTexture. So each backdrop blur
 * ends the pass, copies the region behind it, generates its mip chain, and opens
 * a new pass that LOADS what was there.
 *
 * THAT COST IS VISIBLE HERE ON PURPOSE. On a tiler a split stores and reloads
 * the whole attachment, so four frosted panels is four of them, and a UI that
 * quietly did this behind a call named `drawBlur` would be one nobody could
 * find. The raylib painter pays the same shape of cost — it flushes rlgl's
 * batch — and pays it invisibly.
 *
 * It also fixes the ordering for free: everything appended before the blur is on
 * the backbuffer when the copy happens, so a panel frosts the UI beneath it and
 * not merely the scene.
 *
 * ================= ONE UPLOAD A FRAME, FOUR GROWING BUFFERS ===============
 *
 * The draw list arrives as two contiguous arrays, so this uploads each once and
 * then issues a draw per command. Glyph quads and blur outlines are built into
 * two more arrays before the first pass opens and uploaded the same way — one
 * walk of the command list rather than a buffer update between draws.
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

/* For the blur's outline ring, kept as a member so it stops allocating after
 * the first frosted panel — see the reuse note in Outline.hpp. It brings
 * UiDrawList.hpp with it, which is this class's input anyway. */
#include "cromwell/ui/shape/Outline.hpp"

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

    /* Commands that did not reach the screen, counted over the last draw.
     *
     * EVERY KIND IS CONVERTED NOW, so this is no longer a statement about the
     * migration — it is a diagnostic, and a more useful one for it. A text run
     * whose weight never rasterised counts here, and so does a frosted panel
     * whose capture texture could not be made. "The HUD has no labels" and "the
     * HUD is not drawing" look identical on screen and have different causes;
     * this is what separates them. */
    int skippedCommands() const { return skippedCommands_; }

private:
    /* One corner of anything that SAMPLES — a glyph quad or a frosted region.
     * Twenty bytes: position, UV, packed colour, in the order both
     * ui_text.vs.glsl and ui_blur.vs.glsl declare them.
     *
     * ONE VERTEX FOR BOTH, and therefore one buffer: they differ in what the UV
     * addresses and in which shader reads it, and neither of those is a reason
     * to grow, upload and keep in step a second array of the same layout. */
    struct TexturedVertex {
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

    /* One frosted region: how blurred, and the outline fan to draw it with.
     *
     * NO RECTANGLE, because the capture is the WHOLE SCREEN — see the note on
     * captureTexture_ for why copying only the region behind each panel is the
     * version that does not work. */
    struct BlurRegion {
        /* log2 of the strength in pixels — the mip level to read. Fractional,
         * and deliberately so; see ui_blur.fs.glsl. */
        float         lod = 0.0f;

        std::uint32_t indexBegin = 0;
        std::uint32_t indexCount = 0;
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

    /* The same, for the textured geometry — glyphs and blur outlines together. */
    bool ensureTexturedCapacity(uint32_t vertexCount, uint32_t indexCount);

    /* Grows the capture texture to cover at least this much, keeping it
     * otherwise. GROWS ONLY, so dragging a window smaller does not churn a GPU
     * allocation on every frame of the drag. */
    bool ensureCaptureTexture(uint32_t width, uint32_t height);

    /* Builds the glyph quads and blur outlines into the scratch arrays and
     * fills the batch lists. One walk before the first pass opens, so the
     * buffers are uploaded once rather than updated between draws — updating a
     * buffer a pass is already reading is the kind of hazard that works on one
     * driver and tears on another. */
    void buildTexturedGeometry(const UiDrawList& drawList, const UiFontSet& fonts);

    /* Appends one run's quads, returning the range of batches it produced. */
    TextRange appendRun(const TextRun& run, const UiFontSet& fonts);

    /* Appends one frosted region's outline fan. */
    BlurRegion appendBlur(const UiBackdropBlur& blur);

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

    rhi::ShaderHandle   blurShader_;
    rhi::PipelineHandle blurPipeline_;
    rhi::SamplerHandle  captureSampler_;

    /* WHAT IS BEHIND THE PANELS: the whole screen, with a full mip chain,
     * because the chain IS the blur. One texture reused by every frosted region
     * in the frame — they are drawn one at a time, each copying into it just
     * before it is read, so a second would buy nothing.
     *
     * ======= WHY THE WHOLE SCREEN AND NOT JUST THE REGION BEHIND EACH =======
     *
     * Copying only the panel's own rectangle is the obvious economy, it is what
     * UiPainter does, and it is wrong — measurably, on this project's own
     * gallery screen, where a frosted panel over a BLACK scrim came out WHITE.
     *
     * The texture only grows, so a small panel copies into the corner of a
     * texture sized by the largest one. glGenerateMipmap then averages the WHOLE
     * texture, including every texel the copy did not touch — which still holds
     * whatever the last, bigger capture left there. At a blur radius of 24 px a
     * single texel of the level being read covers a 24-px square of source, so
     * the stale content bleeds in from the edge of the used sub-rectangle and, a
     * few levels up, dominates.
     *
     * Capturing the whole screen means the chain is built over a fully valid
     * image, so there are no unwritten texels to average in. Bleeding across a
     * panel's edge then becomes CORRECT rather than a bug: real frosted glass
     * gathers light from just outside its frame too.
     *
     * The cost is a full-screen copy and mip chain per frosted panel rather than
     * a region-sized one. That is the price of the correct answer, and if it
     * ever matters the fix is not to go back — it is a chain that can be built
     * over a sub-rectangle, which means generating the levels by hand. */
    rhi::TextureHandle captureTexture_;
    uint32_t           captureWidth_ = 0;
    uint32_t           captureHeight_ = 0;

    rhi::BufferHandle texturedVertices_;
    rhi::BufferHandle texturedIndices_;
    rhi::MeshHandle   texturedMesh_;

    uint32_t texturedVertexCapacity_ = 0;
    uint32_t texturedIndexCapacity_ = 0;

    /* KEPT ACROSS FRAMES FOR THEIR CAPACITY, cleared rather than freed — the
     * same steady state the draw list itself reaches, and the reason a HUD
     * redrawn sixty times a second allocates nothing after the first few. */
    std::vector<TexturedVertex> texturedVertexScratch_;
    std::vector<std::uint32_t>  texturedIndexScratch_;
    std::vector<GlyphBatch>     glyphBatches_;
    std::vector<TextRange>      textRanges_;
    std::vector<BlurRegion>     blurRegions_;

    Outline blurOutline_;

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
