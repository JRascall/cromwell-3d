/* Before raylib.h — GL.hpp brings glad in first, and glad must precede anything
 * that could pull a system GL header. See the ordering note in GL.hpp. */
#include "cromwell/gpu/GL.hpp"

#include "cromwell/ui/paint/UiPainter.hpp"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/ui/shape/Outline.hpp"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {
namespace {

/* Vertices emitted between one rlBegin/rlEnd pair. rlgl's batch has a fixed
 * capacity and flushes when it is asked to; chunking keeps every span well
 * inside it without a limit check per triangle. */
constexpr int kBatchChunkVertices = 3000;

Color toRaylibColour(std::uint32_t packed)
{
    return Color{ static_cast<unsigned char>(packed & 0xFFu),
                  static_cast<unsigned char>((packed >> 8) & 0xFFu),
                  static_cast<unsigned char>((packed >> 16) & 0xFFu),
                  static_cast<unsigned char>((packed >> 24) & 0xFFu) };
}

/* True when the clip is wide enough that scissoring it would change nothing —
 * the unbounded rect every command starts with. Worth testing, because a
 * scissor rectangle is a state change and most frames need none. */
bool clipIsUnbounded(const UiRect& clip)
{
    return clip.left() <= 0.0f && clip.top() <= 0.0f
        && clip.right() >= static_cast<float>(GetScreenWidth())
        && clip.bottom() >= static_cast<float>(GetScreenHeight());
}

}  // namespace

UiPainter::~UiPainter()
{
    release();
}

void UiPainter::release()
{
    if (captureTextureId_ != 0) {
        rlUnloadTexture(captureTextureId_);
        captureTextureId_ = 0;
        captureWidth_ = 0;
        captureHeight_ = 0;
    }
}

void UiPainter::draw(const UiDrawList& drawList, const UiFontSet& fonts)
{
    CW_PROFILE_ZONE_N("ui");

    if (drawList.empty()) {
        return;
    }

    /* BACKFACE CULLING OFF FOR THE WHOLE PASS, and this is not a precaution —
     * it is a bug fix, and the bug was ugly.
     *
     * raylib enables GL_CULL_FACE with counter-clockwise front faces at init
     * (rlglInit), which is right for a 3D scene and meaningless for a 2D draw
     * list: a UI triangle has no front or back, it has a position on a screen.
     * The builders here do not all wind the same way — a convex fill's interior
     * fan walks the outline, while its feather ring walks along the edge and
     * back out along the normals, which is the opposite orientation. With
     * culling on, exactly one of those two survives.
     *
     * The symptom was a filled pill rendering as a hollow outline: the feather
     * band drew, the interior did not, and it looked for all the world like a
     * styling choice rather than half the mesh being thrown away.
     *
     * The alternative — making all five builders agree on a winding — is a
     * trap. It is invisible in the source, nothing checks it, and the next
     * builder added has a 50% chance of being wrong in a way that looks like a
     * design decision. Turning off a piece of state that cannot mean anything
     * here fixes every builder at once and cannot rot.
     *
     * THE FLUSH IS NOT OPTIONAL. rlgl queues geometry and applies GL state when
     * the batch is DRAWN, so changing the state without draining first would
     * apply it retroactively to whatever the caller had already queued. */
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();

    bool scissorOpen = false;

    for (const UiCommand& command : drawList.commands()) {
        /* Clipping is per command, and the scissor is opened and closed around
         * runs that need it rather than toggled per command — consecutive
         * commands under the same clip are the common case. */
        const bool wantsScissor = !clipIsUnbounded(command.clip);
        if (scissorOpen) {
            EndScissorMode();
            scissorOpen = false;
        }
        if (wantsScissor) {
            if (command.clip.empty()) {
                /* Clipped away entirely — skip rather than open a zero-size
                 * scissor, which some drivers treat as "no clipping". */
                continue;
            }
            /* Outward to whole pixels: floor the near edges, ceil the far ones.
             * Truncating both — which is what a cast does — shaves up to a
             * pixel off the right and bottom of every clipped region, and at a
             * fractional display scale almost every region is fractional. A
             * clip exists to contain, so erring outward is the harmless
             * direction. */
            const int clipX = static_cast<int>(std::floor(command.clip.left()));
            const int clipY = static_cast<int>(std::floor(command.clip.top()));
            const int clipRight = static_cast<int>(std::ceil(command.clip.right()));
            const int clipBottom = static_cast<int>(std::ceil(command.clip.bottom()));
            BeginScissorMode(clipX, clipY, clipRight - clipX, clipBottom - clipY);
            scissorOpen = true;
        }

        switch (command.kind) {
        case UiCommandKind::Triangles:
            executeTriangles(drawList, command);
            break;
        case UiCommandKind::Text:
            executeText(drawList.textRuns()[command.payloadIndex], fonts);
            break;
        case UiCommandKind::BackdropBlur:
            executeBackdropBlur(drawList.backdropBlurs()[command.payloadIndex]);
            break;
        }
    }

    if (scissorOpen) {
        EndScissorMode();
    }

    /* Drained while culling is still off, so the UI's own geometry is drawn
     * under the state it was built for, then handed back exactly as found —
     * the scene that draws next is 3D and wants its culling. */
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
}

void UiPainter::executeTriangles(const UiDrawList& drawList, const UiCommand& command)
{
    if (command.indexCount == 0) {
        return;
    }

    const std::vector<UiVertex>& vertices = drawList.vertices();
    const std::vector<std::uint32_t>& indices = drawList.indices();

    /* The mesh is untextured (see UiDrawList.hpp), so everything binds rlgl's
     * own white pixel and the whole list can batch as one draw. */
    rlSetTexture(rlGetTextureIdDefault());

    std::uint32_t emitted = 0;
    while (emitted < command.indexCount) {
        const std::uint32_t chunk = std::min<std::uint32_t>(command.indexCount - emitted,
                                                            kBatchChunkVertices);
        rlCheckRenderBatchLimit(static_cast<int>(chunk));
        rlBegin(RL_TRIANGLES);
        for (std::uint32_t offset = 0; offset < chunk; ++offset) {
            const UiVertex& vertex = vertices[indices[command.indexBegin + emitted + offset]];
            const Color colour = toRaylibColour(vertex.rgba);
            rlColor4ub(colour.r, colour.g, colour.b, colour.a);
            rlTexCoord2f(0.0f, 0.0f);
            rlVertex2f(vertex.x, vertex.y);
        }
        rlEnd();
        emitted += chunk;
    }

    rlSetTexture(0);
}

void UiPainter::executeText(const TextRun& run, const UiFontSet& fonts)
{
    if (run.text.empty()) {
        return;
    }

    /* The run's position is the top-left of the LINE BOX, and raylib draws from
     * the top-left of the glyph box, which is the font size rather than the
     * line height. Centring the glyphs inside the line box here is what keeps
     * text vertically where the layout said it would be — see the note on line
     * height in UiFontSet.hpp. */
    const float lineHeight = fonts.lineHeight(run.style);
    const float glyphOffset = (lineHeight - run.style.sizePx) * 0.5f;

    DrawTextEx(fonts.fontFor(run.style.weight, run.style.sizePx), run.text.c_str(),
               Vector2{ run.position.x, run.position.y + glyphOffset },
               run.style.sizePx, run.style.letterSpacingPx,
               toRaylibColour(run.style.colour.toSrgb8()));
}

bool UiPainter::ensureCaptureTexture(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    /* Grows only. A panel that shrinks keeps the larger texture, so resizing a
     * window does not churn GPU allocations every frame of the drag. */
    if (captureTextureId_ != 0 && width <= captureWidth_ && height <= captureHeight_) {
        return true;
    }

    const int newWidth = std::max(width, captureWidth_);
    const int newHeight = std::max(height, captureHeight_);

    /* mipmapCount 1 allocates level 0 only; glGenerateMipmap fills in the rest
     * and allocates them on first use. */
    const unsigned int id = rlLoadTexture(nullptr, newWidth, newHeight,
                                          RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    if (id == 0) {
        return false;
    }

    /* Trilinear, because the blur IS the mip level and a fractional level has
     * to interpolate between two of them. Clamped, so the edge of the captured
     * region does not wrap around and smear the opposite side into it. */
    rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_MIP_LINEAR);
    rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);

    if (captureTextureId_ != 0) {
        rlUnloadTexture(captureTextureId_);
    }
    captureTextureId_ = id;
    captureWidth_ = newWidth;
    captureHeight_ = newHeight;
    return true;
}

void UiPainter::executeBackdropBlur(const UiBackdropBlur& blur)
{
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());

    /* The copy reads the framebuffer, so the region has to be inside it — a
     * panel hanging off the edge of the window would otherwise copy undefined
     * pixels. */
    const UiRect region = blur.rect.intersected({ 0.0f, 0.0f, screenWidth, screenHeight });
    const int width = static_cast<int>(region.width);
    const int height = static_cast<int>(region.height);
    if (width <= 1 || height <= 1) {
        return;
    }

    /* Everything already batched has to be on the framebuffer before it can be
     * read back — an unflushed batch would blur the frame BEFORE this UI, which
     * shows up as a panel frosting the scene but not the UI beneath it. */
    rlDrawRenderBatchActive();

    if (!ensureCaptureTexture(width, height)) {
        return;
    }

    /* GL's origin is bottom-left; the UI's is top-left. */
    const int glY = static_cast<int>(screenHeight - (region.y + region.height));
    gl::copyFramebufferToTexture(captureTextureId_, static_cast<int>(region.x), glY, width, height);
    gl::generateTextureMipmaps(captureTextureId_);

    /* Strength in pixels becomes a mip level: each level halves the resolution,
     * so a radius of 2^n pixels is level n. Fractional levels interpolate,
     * which is what makes the dial continuous rather than stepping through
     * powers of two. */
    const float lod = std::log2(std::max(blur.strengthPx, 1.0f));
    gl::setTextureLodRange(captureTextureId_, lod, lod);

    /* The captured region may be smaller than the texture (which only grows),
     * so the UVs cover the used sub-rectangle rather than the whole thing. */
    const float uScale = static_cast<float>(width) / static_cast<float>(captureWidth_);
    const float vScale = static_cast<float>(height) / static_cast<float>(captureHeight_);

    Outline outline;
    outline.buildRect(region, blur.cornerRadii, 8);

    rlSetTexture(captureTextureId_);
    rlCheckRenderBatchLimit(static_cast<int>(outline.size()) * 3);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 255);

    const auto emit = [&](std::size_t index) {
        const Vec2 point = outline.position(index);
        const float u = (point.x - region.x) / region.width * uScale;
        /* Flipped: texture row 0 is the BOTTOM of the copied region. */
        const float v = (1.0f - (point.y - region.y) / region.height) * vScale;
        rlTexCoord2f(u, v);
        rlVertex2f(point.x, point.y);
    };

    /* Fan from point 0 — the outline is convex, so this is a valid
     * triangulation without needing to prove anything about it. */
    for (std::size_t index = 1; index + 1 < outline.size(); ++index) {
        emit(0);
        emit(index);
        emit(index + 1);
    }

    rlEnd();
    rlSetTexture(0);

    /* Undo the LOD pin, so the texture is a plain sampled texture again if
     * anything else ever binds it. */
    gl::setTextureLodRange(captureTextureId_, -1000.0f, 1000.0f);
}

}  // namespace cromwell::ui
