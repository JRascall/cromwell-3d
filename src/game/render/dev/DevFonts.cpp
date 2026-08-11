#include "game/render/dev/DevFonts.hpp"

#include "cromwell/ui/FontAwesomeIcons.hpp"
#include "cromwell/ui/UiFontAssets.hpp"

#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <cmath>
#include <string>

namespace game {

namespace {

/* 16, not ImGui's 13. ProggyClean is a hand-tuned bitmap face that is legible
 * at 13 because every pixel was placed by hand; Inter is an outline face and
 * at 13 the stb rasteriser has too few pixels to put a stem in, so it reads
 * soft. 16 is where it sharpens up and is also the size Inter itself is
 * designed around. The panel got taller; that is the trade. */
constexpr float kBaseSizePixels = 16.0f;

/* Icons at 0.85 of the text size. A Font Awesome glyph fills its em box, where
 * a letter uses about two thirds of one, so an icon merged at the text size
 * looks a weight heavier than the words beside it. This is the same ratio
 * rlImGui picked for its own bundled face (11 against 13) and it is worth
 * matching rather than re-deriving. */
constexpr float kIconSizeRatio = 0.85f;

/* Keep Font Awesome away from the letters and digits. Both icon faces alias
 * printable ASCII — 'A' and '0' are icons in the classic set — so a merge that
 * did not exclude this range would let the icon font answer for characters
 * Inter is already drawing, and the first face to claim a codepoint wins. That
 * ordering does protect us today, since Inter is added first. Excluding the
 * range says so explicitly, and the merge stops depending on the order two
 * calls happen to be written in.
 *
 * Static because ImGui keeps the pointer: the array has to outlive the atlas,
 * not the function. */
const ImWchar kAsciiExcluded[] = { 0x0020, 0x00FF, 0 };

/* The DPI factor to hand ImGui. rlImGui does this multiply itself when sizing
 * its own default font, because without the HIGHDPI window flag raylib reports
 * a framebuffer the size of the window and leaves the scaling to us — a 16 px
 * font on a 150% display would otherwise be 16 physical pixels and read as 11.
 * GetWindowScaleDPI is what rlImGui's internal GetDisplayScale wraps; that
 * wrapper is not declared in its header. */
float dpiScale()
{
#if defined(__APPLE__)
    return 1.0f;
#else
    if (IsWindowState(FLAG_WINDOW_HIGHDPI)) return 1.0f;
    const float scale = GetWindowScaleDPI().y;
    return scale > 0.0f ? scale : 1.0f;
#endif
}

/* Merges the two icon faces into whatever font was added last. Both, always:
 * splitting them across two ImFonts would push the choice of face out to every
 * call site, and a call site that wants ICON_FA_STEAM should not have to know
 * that Font Awesome ships brands in a separate file. */
void mergeIcons(float textSize)
{
    static const ImWchar range[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

    ImFontConfig config;
    config.MergeMode = true;
    config.GlyphExcludeRanges = kAsciiExcluded;
    config.ExtraSizeScale = kIconSizeRatio;

    /* Uniform advance, so a column of icons in a menu or a toolbar lines up
     * whatever each glyph's natural width is. Min only, not min and max — an
     * icon wider than the cell should overhang rather than be squeezed. */
    config.GlyphMinAdvanceX = textSize;

    /* Nudged down by the difference the size ratio opens up between the two
     * baselines, so an icon sits on the line rather than floating above it. */
    config.GlyphOffset = ImVec2{ 0.0f, std::floor(textSize * 0.1f) };

    ImGuiIO& io = ImGui::GetIO();
    const std::string solid = cromwell::ui::UiFontAssets::iconSolid();
    if (!solid.empty())
        io.Fonts->AddFontFromFileTTF(solid.c_str(), textSize, &config, range);

    const std::string brands = cromwell::ui::UiFontAssets::iconBrands();
    if (!brands.empty())
        io.Fonts->AddFontFromFileTTF(brands.c_str(), textSize, &config, range);
}

ImFont* addFace(cromwell::ui::FontWeight weight, float size)
{
    const std::string path = cromwell::ui::UiFontAssets::inter(weight);
    if (path.empty()) return nullptr;

    ImFontConfig config;
    /* Inter's own hinting assumes integral advances at UI sizes, and ImGui
     * draws at integral positions anyway. */
    config.PixelSnapH = true;

    ImFont* font =
        ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &config);
    if (font != nullptr) mergeIcons(size);
    return font;
}

ImFont* g_text = nullptr;
ImFont* g_heading = nullptr;
float   g_sizePixels = 0.0f;

}  // namespace

bool DevFonts::setup(bool darkTheme)
{
    if (!cromwell::ui::UiFontAssets::installed()) {
        rlImGuiSetup(darkTheme);
        TraceLog(LOG_INFO,
                 "UI: font pack not installed, using ImGui's default face "
                 "(see src/cromwell/assets/fonts/README.md)");
        return false;
    }

    /* The three-call form rather than rlImGuiSetup, because the fonts have to
     * be added between the context existing and the backend being set up, and
     * rlImGuiSetup is exactly those three calls with no seam in the middle. */
    rlImGuiBeginInitImGui();

    /* Rasterised at the UNSCALED size, and scaled by the style rather than by
     * baking DPI into the atlas. Since 1.92 an ImFont has no single size — it
     * rasterises at whatever size is asked for — so the size passed here is
     * only the default, and the DPI factor belongs in style.FontScaleDpi where
     * update() can change it when the window moves to another monitor. Baking
     * it in here was the earlier bug: correct on the display the app started
     * on and never again. */
    g_sizePixels = kBaseSizePixels;
    g_text = addFace(cromwell::ui::FontWeight::Regular, g_sizePixels);
    g_heading = addFace(cromwell::ui::FontWeight::SemiBold, g_sizePixels);

    /* Not merely the first font added — rlImGuiBeginInitImGui has already added
     * ProggyClean as index 0, and ImGui's default is whichever font is first
     * unless told otherwise. Saying it explicitly also means the fallback path
     * above and this one leave the context in states that differ only in the
     * face, not in which font anything draws with. */
    if (g_text != nullptr) ImGui::GetIO().FontDefault = g_text;

    if (darkTheme) ImGui::StyleColorsDark();
    else           ImGui::StyleColorsLight();

    rlImGuiEndInitImGui();

    ImGui::GetStyle().FontSizeBase = g_sizePixels;
    update();

    TraceLog(LOG_INFO, "UI: Inter at %.0f px (dpi x%.2f) with Font Awesome merged",
             static_cast<double>(g_sizePixels), static_cast<double>(dpiScale()));
    return g_text != nullptr;
}

void DevFonts::update()
{
    if (ImGui::GetCurrentContext() == nullptr) return;

    /* Every frame, because a window can be dragged between monitors of
     * different scale and nothing announces it. Assigning the same float again
     * is free; ImGui only re-rasterises when the resulting size changes. */
    ImGui::GetStyle().FontScaleDpi = dpiScale();
}

ImFont* DevFonts::text() { return g_text; }
ImFont* DevFonts::heading() { return g_heading; }
/* The SCALED size — what a layout reserving a row actually needs. GetFontSize
 * is FontSizeBase times every global factor, which is the number text is drawn
 * at; g_sizePixels alone would be wrong on any display that is not at 100%. */
float DevFonts::sizePixels()
{
    if (ImGui::GetCurrentContext() == nullptr) return g_sizePixels;
    return ImGui::GetFontSize();
}

}  // namespace game
