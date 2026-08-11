#include "game/render/ui/WidgetGallery.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/ui/control/BorderButton.hpp"
#include "cromwell/ui/control/Label.hpp"
#include "cromwell/ui/control/SettingSlider.hpp"
#include "cromwell/ui/control/SettingStepper.hpp"
#include "cromwell/ui/control/TextButton.hpp"
#include "cromwell/ui/gauge/SegmentBar.hpp"
#include "cromwell/ui/gauge/SegmentRing.hpp"
#include "cromwell/ui/loader/ActivitySpinner.hpp"
#include "cromwell/ui/loader/LoadingBar.hpp"
#include "cromwell/ui/loader/LoadingRing.hpp"
#include "cromwell/ui/panel/BlurPanel.hpp"
#include "cromwell/ui/panel/TipPanel.hpp"
#include "cromwell/ui/paint/WorldAnchor.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace game {
namespace ui = cromwell::ui;

namespace {

/* EVERY CONSTANT IN THIS FILE IS IN REFERENCE PIXELS — what the layout would be
 * on a 96 DPI monitor at 100%. They reach a widget through px(), never
 * directly. That is the display-scale contract from the caller's side; see the
 * long note in cromwell/ui/core/UiContext.hpp. */
constexpr float kMargin = 24.0f;
constexpr float kColumnGap = 24.0f;
constexpr int   kColumnCount = 4;

constexpr float kRowGap = 18.0f;
constexpr float kHeadingGap = 10.0f;
constexpr float kCaptionHeight = 14.0f;

/* Where the world-anchored badges are pinned. Arbitrary points on the lattice —
 * the demo is about the projection and the crispness, not about the game. */
constexpr Vector3 kAnchorPoints[3] = {
    { 4.0f, 1.5f, 4.0f },
    { -6.0f, 1.5f, 2.0f },
    { 2.0f, 1.5f, -7.0f },
};
constexpr const char* kAnchorNames[3] = { "ALPHA", "BRAVO", "CHARLIE" };

ui::TextStyle headingStyle()
{
    return ui::TextStyle{ 11.0f, ui::FontWeight::Bold, 2.0f,
                          ui::UiColor::white().withAlpha(0.45f) };
}

ui::TextStyle captionStyle()
{
    return ui::TextStyle{ 11.0f, ui::FontWeight::Regular, 0.0f,
                          ui::UiColor::white().withAlpha(0.55f) };
}

}  // namespace

WidgetGallery::WidgetGallery()
{
    stepperOptions_ = { "LOW", "MEDIUM", "HIGH", "ULTRA" };
}

WidgetGallery::~WidgetGallery() = default;

float WidgetGallery::px(float referencePixels) const
{
    return context_ ? context_->px(referencePixels) : referencePixels;
}

/* A caption under a widget, centred on it. The gallery is unreadable without
 * them — a ring is a ring, and which STYLE of ring it is is the whole point. */
void WidgetGallery::caption(const ui::UiRect& box, const char* text)
{
    const ui::TextStyle style = ui::scaled(captionStyle(), context_->scale());
    const cromwell::Vec2 size = context_->metrics().measure(text, style);
    const ui::UiRect placed = ui::alignIn(box, size, ui::HorizontalAlign::Centre,
                                          ui::VerticalAlign::Middle);
    ui::TextRun run;
    run.text = text;
    run.position = placed.topLeft();
    run.style = style;
    context_->drawList().addText(std::move(run));
}

float WidgetGallery::heading(const ui::UiRect& column, float y, const char* text)
{
    const ui::TextStyle style = ui::scaled(headingStyle(), context_->scale());
    ui::TextRun run;
    run.text = text;
    run.position = { column.x, y };
    run.style = style;
    context_->drawList().addText(std::move(run));

    /* A hairline under the heading, which is what makes four columns of small
     * widgets read as four groups rather than as a wall. It goes through
     * addRect, which snaps it to a whole device pixel — a 1px rule at a
     * fractional y is two half-lit rows, and at a 1.5 scale almost every
     * computed y is fractional. */
    const float lineY = y + context_->metrics().lineHeight(style) + px(3.0f);
    ui::shapes::addRect(context_->drawList(), { column.x, lineY, column.width, px(1.0f) },
                        ui::UiColor::white().withAlpha(0.12f));

    return lineY + px(kHeadingGap);
}

void WidgetGallery::draw(GameUi& gameUi, const Camera3D& camera)
{
    if (!visible_) {
        return;
    }

    context_ = &gameUi.begin();

    /* A scrim over the scene, so the widgets are judged against a flat ground
     * rather than against whatever happens to be behind them. Frosted, which
     * also exercises the backdrop blur on something the size of the screen. */
    ui::BlurPanelSpec scrim;
    scrim.blurStrengthPx = 10.0f;
    scrim.fillColour = ui::UiColor::black().withAlpha(0.55f);
    scrim.contentPadding = ui::UiPadding::all(kMargin);
    const ui::UiRect content = ui::drawBlurPanel(*context_, context_->screenRect(),
                                                 ui::scaled(scrim, context_->scale()));

    const float columnGap = px(kColumnGap);
    const float columnWidth =
        (content.width - columnGap * static_cast<float>(kColumnCount - 1))
        / static_cast<float>(kColumnCount);

    const auto column = [&](int index) {
        return ui::UiRect{ content.x + static_cast<float>(index) * (columnWidth + columnGap),
                           content.y, columnWidth, content.height };
    };

    drawLoaders(column(0));
    drawGauges(column(1));
    drawControls(column(2));
    drawPanels(column(3));

    /* Last, so the badges sit over the scrim — they are anchored to the world
     * behind it, which is exactly the point. */
    drawWorldAnchors(camera);

    gameUi.end();
    context_ = nullptr;
}

void WidgetGallery::drawLoaders(ui::UiRect col)
{
    const float scale = context_->scale();
    float y = heading(col, col.y, "LOADERS");

    /* The three ring styles, side by side, so the difference between them is
     * visible rather than described. */
    const float ringRadius = px(22.0f);
    const float ringBox = ringRadius * 2.0f;

    const float slot = col.width / 3.0f;
    const ui::LoadingRingStyle styles[3] = { ui::LoadingRingStyle::Spin,
                                             ui::LoadingRingStyle::Fill,
                                             ui::LoadingRingStyle::Progress };
    const char* names[3] = { "spin", "fill", "progress" };

    /* A progress value that sweeps up and back, so the determinate widgets are
     * never caught sitting still. */
    const float sweep = 0.5f - 0.5f * std::cos(static_cast<float>(context_->time()) * 0.7f);

    for (int index = 0; index < 3; ++index) {
        ui::LoadingRingSpec spec;
        spec.style = styles[index];
        spec.radiusPx = 22.0f;
        spec.thicknessPx = 5.0f;
        spec.progress = sweep;
        spec.arcColour = ui::theme::accent();

        const float centreX = col.x + slot * (static_cast<float>(index) + 0.5f);
        ui::drawLoadingRing(*context_, { centreX, y + ringRadius }, ui::scaled(spec, scale));
        caption({ col.x + slot * static_cast<float>(index), y + ringBox + px(2.0f),
                  slot, px(kCaptionHeight) }, names[index]);
    }
    y += ringBox + px(20.0f + kRowGap);

    /* Two spinners: Apple's eight spokes and the classic twelve. */
    {
        const float spinnerRadius = px(20.0f);
        const int spokeCounts[2] = { 8, 12 };
        const char* spinnerNames[2] = { "8 spokes", "12 spokes" };
        const float half = col.width / 2.0f;

        for (int index = 0; index < 2; ++index) {
            ui::ActivitySpinnerSpec spec;
            spec.radiusPx = 20.0f;
            spec.spokeCount = spokeCounts[index];
            spec.colour = ui::UiColor::white();

            const float centreX = col.x + half * (static_cast<float>(index) + 0.5f);
            ui::drawActivitySpinner(*context_, { centreX, y + spinnerRadius },
                                    ui::scaled(spec, scale));
            caption({ col.x + half * static_cast<float>(index),
                      y + spinnerRadius * 2.0f + px(2.0f), half, px(kCaptionHeight) },
                    spinnerNames[index]);
        }
        y += spinnerRadius * 2.0f + px(20.0f + kRowGap);
    }

    /* The bar, driven by a value that JUMPS in lumps, which is the case the
     * glide exists for — watch it catch up rather than teleport. */
    barProgress_ = std::min(std::floor(sweep * 5.0f) / 4.0f, 1.0f);

    {
        ui::LoadingBarSpec spec;
        spec.progress = barProgress_;
        spec.fillColour = ui::theme::accent();
        ui::drawLoadingBar(*context_, ui::UiContext::id("gallery.bar.round"),
                           { col.x, y, col.width, px(12.0f) }, ui::scaled(spec, scale));
        caption({ col.x, y + px(14.0f), col.width, px(kCaptionHeight) }, "pill, gliding");
        y += px(32.0f);

        spec.roundedEnds = false;
        spec.barHeightPx = 3.0f;
        ui::drawLoadingBar(*context_, ui::UiContext::id("gallery.bar.square"),
                           { col.x, y, col.width, px(12.0f) }, ui::scaled(spec, scale));
        caption({ col.x, y + px(14.0f), col.width, px(kCaptionHeight) }, "square ends");
    }
}

void WidgetGallery::drawGauges(ui::UiRect col)
{
    const float scale = context_->scale();
    float y = heading(col, col.y, "GAUGES");

    const float sweep = 0.5f - 0.5f * std::cos(static_cast<float>(context_->time()) * 0.5f);

    /* Segment rings at three chip counts. 1 is the degenerate continuous ring,
     * and it is here because that path is easy to break and hard to notice. */
    {
        const float radius = px(30.0f);
        const int counts[3] = { 8, 4, 1 };
        const char* names[3] = { "8 chips", "4 chips", "continuous" };
        const float slot = col.width / 3.0f;

        for (int index = 0; index < 3; ++index) {
            ui::SegmentRingSpec spec;
            spec.radiusPx = 30.0f;
            spec.thicknessPx = 6.0f;
            spec.segmentCount = counts[index];
            spec.progress = sweep;
            spec.fillColour = ui::theme::accent();
            spec.centreText = std::format("{}", static_cast<int>(sweep * 100.0f));
            spec.centreStyle = ui::TextStyle{ 12.0f, ui::FontWeight::Bold, 0.0f,
                                              ui::UiColor::white() };

            const float centreX = col.x + slot * (static_cast<float>(index) + 0.5f);
            ui::drawSegmentRing(*context_, { centreX, y + radius }, ui::scaled(spec, scale));
            caption({ col.x + slot * static_cast<float>(index),
                      y + radius * 2.0f + px(2.0f), slot, px(kCaptionHeight) }, names[index]);
        }
        y += radius * 2.0f + px(20.0f + kRowGap);
    }

    /* The segment bar, with the cursor preview on — hover it and the chips
     * fill to the cursor; click to commit. */
    {
        ui::SegmentBarSpec spec;
        spec.segmentCount = 6;
        spec.value = segmentValue_;
        spec.hoverPreview = true;
        spec.segmentSize = { 14.0f, 34.0f };
        spec.slantPx = 14.0f;
        spec.fillColour = ui::theme::accent();
        spec.highlightColour = ui::UiColor::white();

        const ui::UiRect bounds{ col.x, y, col.width, px(40.0f) };
        const ui::SegmentBarResult result =
            ui::drawSegmentBar(*context_, ui::UiContext::id("gallery.segbar"), bounds,
                               ui::scaled(spec, scale));

        /* The bar previews; committing is the caller's, which is the split the
         * widget's header argues for. */
        if (result.hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            segmentValue_ = result.previewValue;
        }

        caption({ col.x, y + px(44.0f), col.width, px(kCaptionHeight) },
                result.hovered ? "click to set" : "hover to preview");
        y += px(66.0f);
    }

    /* A row of tags, one highlighted — the label's two states next to each
     * other, which is the only way to judge either. */
    {
        const char* tags[3] = { "STANDARD", "VETERAN", "ELITE" };
        float x = col.x;
        for (int index = 0; index < 3; ++index) {
            ui::LabelSpec spec;
            spec.text = tags[index];
            spec.highlighted = (selectedTag_ == index);
            spec.textStyle.sizePx = 11.0f;
            spec.highlightAnim = ui::HighlightAnim::SweepRight;

            const ui::LabelSpec device = ui::scaled(spec, scale);
            const cromwell::Vec2 size = ui::measureLabel(*context_, device);
            const ui::UiRect bounds{ x, y, size.x, size.y };
            ui::drawLabel(*context_, ui::UiContext::id("gallery.tag", index), bounds, device);

            if (context_->isHovered(bounds) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                selectedTag_ = index;
            }
            x += size.x + px(6.0f);
        }
        caption({ col.x, y + px(30.0f), col.width, px(kCaptionHeight) }, "labels - click to select");
    }
}

void WidgetGallery::drawControls(ui::UiRect col)
{
    const float scale = context_->scale();
    float y = heading(col, col.y, "CONTROLS");

    /* Text buttons, one per hover style, so the two treatments can be compared
     * by moving the cursor between them. */
    {
        const ui::HighlightAnim anims[2] = { ui::HighlightAnim::Fade,
                                             ui::HighlightAnim::SweepRight };
        const char* names[2] = { "CROSS-FADE", "SWEEP" };

        for (int index = 0; index < 2; ++index) {
            ui::TextButtonSpec spec;
            spec.text = names[index];
            spec.hoverAnim = anims[index];
            spec.horizontalAlign = ui::HorizontalAlign::Left;

            const ui::TextButtonSpec device = ui::scaled(spec, scale);
            const cromwell::Vec2 size = ui::measureTextButton(*context_, device);
            const ui::UiRect bounds{ col.x, y, col.width, size.y };
            ui::drawTextButton(*context_, ui::UiContext::id("gallery.textbutton", index),
                               bounds, device);
            y += size.y + px(8.0f);
        }
        caption({ col.x, y, col.width, px(kCaptionHeight) }, "text buttons");
        y += px(kCaptionHeight + kRowGap);
    }

    /* Border buttons: with a keycap, and without one so the gap collapse is
     * visible. */
    {
        for (int index = 0; index < 2; ++index) {
            ui::BorderButtonSpec spec;
            spec.text = index == 0 ? "Close" : "Continue";
            spec.keyText = index == 0 ? "ESC" : "";
            spec.hoverAnim = index == 0 ? ui::HighlightAnim::Fade
                                        : ui::HighlightAnim::SweepUp;

            const ui::BorderButtonSpec device = ui::scaled(spec, scale);
            const cromwell::Vec2 size = ui::measureBorderButton(*context_, device);
            const ui::UiRect bounds{ col.x, y, size.x, size.y };
            ui::drawBorderButton(*context_, ui::UiContext::id("gallery.borderbutton", index),
                                 bounds, device);
            y += size.y + px(8.0f);
        }
        caption({ col.x, y, col.width, px(kCaptionHeight) }, "border buttons");
        y += px(kCaptionHeight + kRowGap);
    }

    /* The slider — draggable, and the readout is live. */
    {
        ui::SettingSliderSpec spec;
        spec.value = sliderValue_;
        spec.valueSuffix = "%";
        spec.valueStyle.sizePx = 13.0f;
        spec.valueMinWidthPx = 40.0f;
        spec.valueGapPx = 14.0f;

        const ui::UiRect bounds{ col.x, y, col.width, px(24.0f) };
        const ui::SettingSliderResult result =
            ui::drawSettingSlider(*context_, ui::UiContext::id("gallery.slider"), bounds,
                                  ui::scaled(spec, scale));
        sliderValue_ = result.value;

        caption({ col.x, y + px(26.0f), col.width, px(kCaptionHeight) }, "slider - drag it");
        y += px(48.0f);
    }

    /* The stepper — click the chevrons, or anywhere on the row. */
    {
        ui::SettingStepperSpec spec;
        spec.options = stepperOptions_;
        spec.selectedIndex = stepperIndex_;
        spec.valueMinWidthPx = 80.0f;
        spec.valueStyle.sizePx = 13.0f;

        /* The default chevrons are outside the ASCII set raylib rasterises by
         * default (see UiFontSet::loadWeight), so the gallery asks for glyphs
         * that are certainly present rather than showing two empty boxes. */
        spec.previousGlyph = "<";
        spec.nextGlyph = ">";

        const ui::UiRect bounds{ col.x, y, col.width, px(24.0f) };
        const ui::SettingStepperResult result =
            ui::drawSettingStepper(*context_, ui::UiContext::id("gallery.stepper"), bounds,
                                   ui::scaled(spec, scale));
        stepperIndex_ = result.selectedIndex;

        caption({ col.x, y + px(26.0f), col.width, px(kCaptionHeight) },
                "stepper - click either side");
    }
}

void WidgetGallery::drawPanels(ui::UiRect col)
{
    const float scale = context_->scale();
    float y = heading(col, col.y, "PANELS");

    /* A full tip card: title, media area, body and footer. */
    {
        ui::TipPanelSpec spec;
        spec.title = "Energy";
        spec.bodyText = "Reactors produce energy each turn. A base that draws more than it "
                        "makes browns out, and life support goes first.";
        spec.footerKeyText = "TAB";
        spec.footerText = "Open Manual";
        spec.mediaHeightPx = 60.0f;
        spec.blurMediaBackground = true;
        spec.titleStyle.sizePx = 12.0f;
        spec.bodyStyle.sizePx = 12.0f;
        spec.footerStyle.sizePx = 11.0f;
        for (float& radius : spec.cornerRadii) {
            radius = 4.0f;
        }

        const ui::TipPanelSpec device = ui::scaled(spec, scale);
        const float height = ui::measureTipPanel(*context_, col.width, device);
        const ui::TipPanelResult result =
            ui::drawTipPanel(*context_, { col.x, y, col.width, height }, device);

        /* Something in the media area, to show it is the caller's to fill: the
         * loading ring, at the size the slot allows. */
        if (!result.mediaRect.empty()) {
            ui::LoadingRingSpec ring;
            ring.radiusPx = std::min(result.mediaRect.height, result.mediaRect.width) * 0.5f;
            ring.thicknessPx = px(4.0f);
            ring.glowRadiusPx = px(ring.glowRadiusPx);
            ring.style = ui::LoadingRingStyle::Spin;
            ring.arcColour = ui::theme::accent();

            /* Already in device pixels — the radius came from a device-pixel
             * rect — so this one is NOT run through scaled(). */
            ui::drawLoadingRing(*context_, result.mediaRect.centre(), ring);
        }

        y += height + px(kRowGap);
        caption({ col.x, y, col.width, px(kCaptionHeight) }, "tip card - all sections");
        y += px(kCaptionHeight + kRowGap);
    }

    /* A one-line hint: the same widget with everything but the body empty,
     * which is the collapse behaviour its header promises. */
    {
        ui::TipPanelSpec spec;
        spec.bodyText = "Hold Shift to queue a move order.";
        spec.bodyStyle.sizePx = 12.0f;
        for (float& radius : spec.cornerRadii) {
            radius = 4.0f;
        }

        const ui::TipPanelSpec device = ui::scaled(spec, scale);
        const float height = ui::measureTipPanel(*context_, col.width, device);
        ui::drawTipPanel(*context_, { col.x, y, col.width, height }, device);
        y += height + px(4.0f);
        caption({ col.x, y, col.width, px(kCaptionHeight) }, "same widget, body only");
        y += px(kCaptionHeight + kRowGap);
    }

    /* A frosted card with rounded corners, over the scrim and the scene — the
     * blur reading both is the thing to check. */
    {
        ui::BlurPanelSpec spec;
        spec.blurStrengthPx = 24.0f;
        spec.fillColour = ui::UiColor::white().withAlpha(0.06f);
        spec.contentPadding = ui::UiPadding::all(12.0f);
        for (float& radius : spec.cornerRadii) {
            radius = 8.0f;
        }

        const ui::UiRect bounds{ col.x, y, col.width, px(70.0f) };
        const ui::UiRect inner = ui::drawBlurPanel(*context_, bounds, ui::scaled(spec, scale));

        ui::TextRun run;
        run.text = "Frosted panel";
        run.position = inner.topLeft();
        run.style = ui::scaled(
            ui::TextStyle{ 13.0f, ui::FontWeight::SemiBold, 0.0f, ui::UiColor::white() }, scale);
        context_->drawList().addText(std::move(run));

        caption({ col.x, bounds.bottom() + px(4.0f), col.width, px(kCaptionHeight) },
                "blur panel - 24px");
    }
}

void WidgetGallery::drawWorldAnchors(const Camera3D& camera)
{
    const float scale = context_->scale();

    for (int index = 0; index < 3; ++index) {
        /* The offset is in PIXELS, scaled — so the badge sits the same distance
         * above its anchor however far away the anchor is, which is what makes
         * it a UI offset rather than a second world position. */
        ui::WorldAnchorSettings settings;
        settings.referenceDistance = 12.0f;
        settings.minScale = 0.6f;
        settings.maxDistance = 60.0f;

        const ui::WorldAnchor anchor =
            ui::anchorToWorld(kAnchorPoints[index], camera, { 0.0f, px(-18.0f) }, settings);
        if (!anchor.visible) {
            continue;
        }

        /* Display scale AND perspective, multiplied together. The perspective
         * term is quantised by the anchor (see WorldAnchor.hpp), so the label
         * steps between a handful of crisp sizes rather than sliding through a
         * hundred rasterisations. */
        const float total = scale * anchor.scale;

        ui::LabelSpec spec;
        spec.text = kAnchorNames[index];
        spec.highlighted = true;
        spec.textStyle.sizePx = 11.0f;
        spec.padding = ui::UiPadding::symmetric(8.0f, 3.0f);

        const ui::LabelSpec device = ui::scaled(spec, total);
        const cromwell::Vec2 size = ui::measureLabel(*context_, device);

        /* Centred on the projected point, and the position is NOT snapped —
         * the badge should track the world smoothly. Its interior hard edges
         * are snapped where they are drawn, which is the right place. */
        const ui::UiRect bounds{ anchor.screenPosition.x - size.x * 0.5f,
                                 anchor.screenPosition.y - size.y * 0.5f,
                                 size.x, size.y };
        ui::drawLabel(*context_, ui::UiContext::id("gallery.anchor", index), bounds, device);
    }
}

}  // namespace game
