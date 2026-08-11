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

ui::UiContext& GameUi::begin()
{
    setup();

    ui::UiInput input;
    const Vector2 mouse = GetMousePosition();
    input.mousePosition = { mouse.x, mouse.y };
    input.mouseDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    input.mousePressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    input.mouseReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    input.timeSeconds = GetTime();
    input.deltaSeconds = GetFrameTime();
    input.screenSize = { static_cast<float>(GetScreenWidth()),
                         static_cast<float>(GetScreenHeight()) };
    input.scale = displayScale();

    context_->beginFrame(input);
    return *context_;
}

void GameUi::end()
{
    if (!context_) {
        return;
    }
    context_->endFrame();
    painter_.draw(context_->drawList(), fonts_);
}

}  // namespace game
