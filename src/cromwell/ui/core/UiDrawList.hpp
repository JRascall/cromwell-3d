/* UiDrawList.hpp — one frame's worth of UI, as data.
 *
 * SINGLE RESPONSIBILITY: accumulate the triangles and text runs the widgets
 * produce, in the order they were produced, with the clip rectangle each was
 * produced under. It draws nothing and knows nothing about GL.
 *
 * WHY A DRAW LIST AT ALL, when the widgets could call the renderer directly.
 * Three reasons, in order of how much they matter:
 *
 *   1. It keeps every widget in cromwell_base. A widget that called
 *      rlBegin/rlVertex would need raylib, and the whole kit would move to the
 *      renderer's side of the engine where nothing can be tested without a
 *      window. Against a draw list, "does the segment ring leave a gap between
 *      chips" is an assertion about vertex positions.
 *
 *   2. Painter's order is explicit. UI is layered — halo under shape, plate
 *      under text, tooltip over everything — and the order these were appended
 *      IS the order they draw. No sorting, no layer ids to keep in step.
 *
 *   3. One upload per frame. All the triangles land in two contiguous arrays,
 *      so the painter uploads once and issues a handful of draw calls, rather
 *      than one per widget.
 *
 * THE MESH IS UNTEXTURED, ALWAYS. Every shape here is exact vertex geometry
 * with a feathered edge — see ui/shape/Shapes.hpp for why that beats a rounded
 * box shader at these sizes — so there is no UV, no atlas, and nothing for the
 * painter to bind but a white pixel. Text is the exception, and it is a separate
 * command kind for exactly that reason: it is the only thing here that samples
 * a texture.
 *
 * COMMANDS BATCH GREEDILY. Consecutive triangles under the same clip extend one
 * command; a text run, or a clip change, starts a new one. So a screen of
 * shapes with three labels on it costs four draw calls, not one per widget.
 *
 * ALLOCATION: clear() keeps capacity, so a UI redrawn every frame settles into
 * steady state after a few frames and then allocates nothing. This is the one
 * performance property the UI actually needs — it runs per frame, and per
 * CLAUDE.md that earns it a profiler zone, not a rewrite.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiText.hpp"

#include <cstdint>
#include <vector>

namespace cromwell::ui {

/* An axis-aligned rectangle in screen pixels, y down.
 *
 * ONE-SHOT DATA CARRIER (see the encapsulation note in UiColor.hpp): it is two
 * corners, it is passed by value everywhere, and no pair of them is invalid —
 * an empty or inverted rect is a legitimate "nothing here" that the callers
 * already test for. */
struct UiRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    static UiRect fromCorners(Vec2 topLeft, Vec2 bottomRight)
    {
        return { topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y };
    }

    /* A rect big enough to never clip anything, for the bottom of the clip
     * stack. Finite rather than infinite so it survives being intersected. */
    static UiRect unbounded() { return { -1.0e6f, -1.0e6f, 2.0e6f, 2.0e6f }; }

    float left() const { return x; }
    float top() const { return y; }
    float right() const { return x + width; }
    float bottom() const { return y + height; }

    Vec2 topLeft() const { return { x, y }; }
    Vec2 centre() const { return { x + width * 0.5f, y + height * 0.5f }; }
    Vec2 size() const { return { width, height }; }

    bool empty() const { return width <= 0.0f || height <= 0.0f; }

    bool contains(Vec2 point) const
    {
        return point.x >= x && point.x <= right() && point.y >= y && point.y <= bottom();
    }

    /* Shrunk by `amount` on every side — the padding operation, and the one the
     * widgets do most. Clamped at empty rather than allowed to invert, because
     * an inside-out content box silently mirrors a layout. */
    UiRect inset(float amount) const
    {
        return inset(amount, amount, amount, amount);
    }

    UiRect inset(float leftAmount, float topAmount, float rightAmount, float bottomAmount) const
    {
        UiRect out{ x + leftAmount, y + topAmount,
                    width - leftAmount - rightAmount, height - topAmount - bottomAmount };
        if (out.width < 0.0f) { out.width = 0.0f; }
        if (out.height < 0.0f) { out.height = 0.0f; }
        return out;
    }

    UiRect expanded(float amount) const { return inset(-amount); }

    /* The overlap of two rects, empty when they do not meet. Clip stacks push
     * intersections rather than replacements so a child can never draw outside
     * a parent that clipped it. */
    UiRect intersected(const UiRect& other) const;
};

/* Where content sits inside a box bigger than it needs.
 *
 * Here rather than with any one widget because five of them take an alignment
 * and none of them owns the concept. */
enum class HorizontalAlign { Left, Centre, Right };
enum class VerticalAlign { Top, Middle, Bottom };

/* Places a box of `size` inside `container` per the two alignments, and returns
 * where it lands. Content larger than its container overhangs the far edge
 * rather than being clamped — a label too wide for its button should look too
 * wide, not silently shift. */
UiRect alignIn(const UiRect& container, Vec2 size,
               HorizontalAlign horizontal, VerticalAlign vertical);

/* Padding, in the CSS order the whole kit uses: left, top, right, bottom. */
struct UiPadding {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static UiPadding all(float amount) { return { amount, amount, amount, amount }; }
    static UiPadding symmetric(float horizontal, float vertical)
    {
        return { horizontal, vertical, horizontal, vertical };
    }

    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

/* Display-scale conversions. See the contract in UiContext.hpp: a layout is
 * authored at a reference scale and multiplied ONCE on the way in, so that
 * everything past that point is in device pixels. */
inline UiPadding scaled(const UiPadding& padding, float factor)
{
    return UiPadding{ padding.left * factor, padding.top * factor,
                      padding.right * factor, padding.bottom * factor };
}

inline UiRect scaled(const UiRect& rect, float factor)
{
    return UiRect{ rect.x * factor, rect.y * factor,
                   rect.width * factor, rect.height * factor };
}

/* One vertex: a position in screen pixels and a packed sRGB colour. No UV — see
 * the header note on why the mesh is untextured. */
struct UiVertex {
    float         x = 0.0f;
    float         y = 0.0f;
    std::uint32_t rgba = 0xFFFFFFFFu;
};

enum class UiCommandKind {
    Triangles,
    Text,

    /* Blur whatever has already been drawn behind this rectangle. The only
     * command that READS the framebuffer, which is why it cannot be geometry:
     * see BackdropBlur below. */
    BackdropBlur,
};

/* A frosted-glass region.
 *
 * WHY THIS IS A COMMAND AND NOT A SHAPE. Everything else in the draw list is
 * "put these colours here"; this one is "take what is already there and smear
 * it". That needs the framebuffer as an input, which no amount of vertex
 * geometry can provide. Keeping it a command means the headless half can still
 * SAY a panel is frosted — it lands in the list, the tests can see it — while
 * only the painter has to know how. */
struct UiBackdropBlur {
    UiRect rect;
    float  cornerRadii[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    /* Blur radius in screen pixels at full resolution. */
    float strengthPx = 12.0f;
};

/* A run of the draw list the painter can execute in one go. */
struct UiCommand {
    UiCommandKind kind = UiCommandKind::Triangles;
    UiRect        clip = UiRect::unbounded();

    /* Triangles: a half-open range into indices(). */
    std::uint32_t indexBegin = 0;
    std::uint32_t indexCount = 0;

    /* Text and BackdropBlur: an index into textRuns() or backdropBlurs()
     * respectively — which one is decided by `kind`, so the two kinds do not
     * each need a field the other leaves at zero. */
    std::uint32_t payloadIndex = 0;
};

class UiDrawList {
public:
    /* Empties it WITHOUT giving the memory back — see the allocation note in
     * the header. */
    void clear();

    /* ---- geometry ------------------------------------------------------ */

    /* Appends a vertex and returns its index, which is what the shape builders
     * thread through their strip and fan arithmetic. */
    std::uint32_t addVertex(Vec2 position, const UiColor& colour);

    void addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c);

    /* Two triangles over four corners wound a-b-c-d. Every band, strip and
     * feather ring in the kit is quads, so this is the hot path of the
     * builders and worth not spelling twice per use. */
    void addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d);

    /* ---- text ---------------------------------------------------------- */

    void addText(TextRun run);

    /* ---- backdrop ------------------------------------------------------ */

    /* Blurs what has already been drawn behind `blur.rect`. Append it BEFORE
     * the panel's fill and content, like any other layer. */
    void addBackdropBlur(const UiBackdropBlur& blur);

    /* ---- clipping ------------------------------------------------------ */

    /* Pushes the INTERSECTION of `rect` with the current clip, so nesting can
     * only ever shrink the visible area. Every push needs a matching pop. */
    void pushClip(const UiRect& rect);
    void popClip();
    const UiRect& clip() const { return clipStack_.back(); }

    /* ---- readback, for the painter and the tests ------------------------ */

    const std::vector<UiVertex>&  vertices() const { return vertices_; }
    const std::vector<std::uint32_t>& indices() const { return indices_; }
    const std::vector<UiCommand>& commands() const { return commands_; }
    const std::vector<TextRun>&   textRuns() const { return textRuns_; }
    const std::vector<UiBackdropBlur>& backdropBlurs() const { return backdropBlurs_; }

    std::uint32_t vertexCount() const { return static_cast<std::uint32_t>(vertices_.size()); }
    std::uint32_t indexCount() const { return static_cast<std::uint32_t>(indices_.size()); }
    bool empty() const { return indices_.empty() && textRuns_.empty(); }

private:
    /* Returns the command triangles should be appended to, opening a new one if
     * the top of the list is a text run or was recorded under a different
     * clip. */
    UiCommand& trianglesCommand();

    std::vector<UiVertex>      vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<UiCommand>     commands_;
    std::vector<TextRun>       textRuns_;
    std::vector<UiBackdropBlur> backdropBlurs_;

    /* Bottom entry is UiRect::unbounded() and is never popped, so clip() is
     * always valid without a null check at every call site. */
    std::vector<UiRect> clipStack_{ UiRect::unbounded() };
};

}  // namespace cromwell::ui
