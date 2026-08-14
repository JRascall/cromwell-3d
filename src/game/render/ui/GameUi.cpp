#include "game/render/ui/GameUi.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"

#include "raylib.h"

#include <string>

namespace game {
namespace ui = cromwell::ui;

namespace {

/* The UI typeface. Named once so swapping the family is one edit — though the
 * letter spacings in the widget specs are Inter's own, so a swap means
 * re-tuning them. See assets/fonts/README.md. */
constexpr const char* kFontFamily = "Inter";

/* Device pixels per reference pixel.
 *
 * Mirrors DevFonts::dpiScale, and deliberately: the ImGui panel and the widget
 * kit have to agree about how big a 16 px thing is, or the same number means
 * two sizes on one screen.
 *
 * Without the HIGHDPI window flag raylib hands back a framebuffer the size of
 * the window in real pixels and leaves the scaling to us — so this is a
 * question of SIZE, not of sharpness. Nothing is ever resampled either way. */
float displayScale()
{
#if defined(__APPLE__)
    return 1.0f;
#else
    if (IsWindowState(FLAG_WINDOW_HIGHDPI)) {
        return 1.0f;
    }
    const float scale = GetWindowScaleDPI().y;
    return scale > 0.0f ? scale : 1.0f;
#endif
}

}  // namespace

void GameUi::setup()
{
    if (loaded_) {
        return;
    }
    loaded_ = true;

    /* Same probe as every other asset the app loads, so it resolves when run
     * from the staging directory and from the project root alike. */
    const std::string root = cromwell::ShaderLibrary::assetRoot();

    const struct { ui::FontWeight weight; const char* suffix; } faces[] = {
        { ui::FontWeight::Regular,   "Regular" },
        { ui::FontWeight::Medium,    "Medium" },
        { ui::FontWeight::SemiBold,  "SemiBold" },
        { ui::FontWeight::Bold,      "Bold" },
        { ui::FontWeight::ExtraBold, "ExtraBold" },
    };

    for (const auto& face : faces) {
        const std::string path = root + "/fonts/" + kFontFamily + "-" + face.suffix + ".ttf";
        fonts_.loadWeight(face.weight, path);
    }

    context_ = std::make_unique<ui::UiContext>(fonts_);
    LOGGER->info("ui: {} loaded from {}/fonts, display scale {:.2f}",
                 kFontFamily, root, displayScale());
}

/* THE FRAME'S UI INPUT ARRIVES, it is not sampled here.
 *
 * This used to read GetMousePosition, IsMouseButtonDown, GetTime, GetFrameTime
 * and GetScreenWidth itself — six raylib calls in a class whose job is drawing
 * widgets. Two things were wrong with that beyond the dependency: it sampled a
 * SECOND time, so a click could be seen by the UI and not by the world (or the
 * reverse) on the frame the button changed under it; and it read the raw
 * pointer in logical units while everything else works in surface pixels,
 * which on a high-DPI display puts every hit test out by the scale factor.
 *
 * One sample, taken in the loop where the platform's services live, shared by
 * the world and the interface. See Application::run. */
ui::UiContext& GameUi::begin(const ui::UiInput& input)
{
    setup();

    /* The one field the caller cannot know: the font atlas's own scale, which
     * belongs to this class. Everything else travels in. */
    ui::UiInput scaled = input;
    scaled.scale = displayScale();

    context_->beginFrame(scaled);
    return *context_;
}

void GameUi::end()
{
    if (!context_) {
        return;
    }
    context_->endFrame();

    /* THROUGH WHOEVER IS PAINTING. The draw list is the same either way — that
     * is the whole reason the kit narrowed to one painter — so the widgets
     * above here cannot tell which renderer is running. */
    cromwell::ui::IUiPainter& painter =
        external_ != nullptr ? *external_ : static_cast<cromwell::ui::IUiPainter&>(painter_);
    painter.draw(context_->drawList(), fonts_);
}

}  // namespace game
