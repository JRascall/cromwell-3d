/* UiPainter.hpp — the only part of the UI that talks to the GPU.
 *
 * SINGLE RESPONSIBILITY: execute a draw list — upload its triangles, draw its
 * text, and blur its backdrops — and own the scratch resources that needs.
 *
 * WHY THE WHOLE KIT NARROWS TO THIS ONE FILE. Everything above it is arithmetic
 * over rectangles and produces data; this is where data becomes pixels. That
 * boundary is what lets the widgets live in cromwell_base and be tested without
 * a window, and it is worth keeping thin — if a widget ever needs something
 * this cannot express, the answer is a new COMMAND KIND, not a widget that
 * reaches for GL.
 *
 * IT DRAWS THROUGH rlgl, deliberately, rather than owning a VAO and a shader.
 * The UI is a few thousand triangles a frame in screen coordinates, which is
 * exactly what rlgl's batch already does well; a bespoke pipeline would buy
 * nothing measurable and would need its own projection, blend state and shader
 * to keep in step with raylib's. The one place it reaches past rlgl is the
 * backdrop blur, which needs to READ the framebuffer — see cromwell/gpu/GL.hpp,
 * where those entry points live behind the engine's one GL door.
 *
 * PROFILING: the whole call is one zone, `ui`. Per CLAUDE.md that is the right
 * granularity until a measurement says otherwise — one row that says where the
 * UI's time went, rather than a row per widget for something costing a fraction
 * of a millisecond. If it ever becomes a real slice of the frame, split it
 * along whatever line the measurement points at.
 *
 * CALL IT INSIDE BeginDrawing/EndDrawing, after the scene. It draws in screen
 * pixels with y down, which is raylib's default 2D space, and leaves the render
 * state as it found it.
 */
#pragma once

#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/paint/IUiPainter.hpp"
#include "cromwell/ui/text/UiFontSet.hpp"

namespace cromwell::ui {

class UiPainter final : public IUiPainter {
public:
    UiPainter() = default;
    ~UiPainter() override;

    UiPainter(const UiPainter&) = delete;
    UiPainter& operator=(const UiPainter&) = delete;

    /* Executes every command in order. `fonts` resolves the text runs' weights;
     * it must be the same set the widgets measured against, or the layout and
     * the drawing will disagree. */
    void draw(const UiDrawList& drawList, const UiFontSet& fonts) override;

    /* Releases the backdrop scratch texture. Called by the destructor; exposed
     * for a caller that needs to let go of GPU memory before the context
     * does. */
    void release() override;

private:
    void executeTriangles(const UiDrawList& drawList, const UiCommand& command);
    void executeText(const TextRun& run, const UiFontSet& fonts);
    void executeBackdropBlur(const UiBackdropBlur& blur);

    /* Grows the capture texture to at least this size, keeping it otherwise.
     * Returns false when the texture could not be created, which makes the
     * blur a no-op rather than an error — a panel without its frosting still
     * shows its fill. */
    bool ensureCaptureTexture(int width, int height);

    unsigned int captureTextureId_ = 0;
    int          captureWidth_ = 0;
    int          captureHeight_ = 0;
};

}  // namespace cromwell::ui
