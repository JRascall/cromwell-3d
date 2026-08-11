/* SettingSlider.hpp — the settings-menu slider.
 *
 * SINGLE RESPONSIBILITY: draw a thin track with a tick thumb and a numeric
 * readout, drive it from the pointer, and hand back the value.
 *
 * A LINE AND A TICK, not a groove and a knob. The whole control is two
 * rectangles and a number, and it earns its place through what it does NOT
 * draw: no bevel, no filled progress portion, no rounded handle. The line is
 * long and quiet, and the eye goes to the number.
 *
 * THE VALUE IS RETURNED, NOT STORED. An immediate-mode control cannot own the
 * setting it edits — the setting belongs to whatever is being configured — so
 * the caller passes the current value in and writes the returned one back. That
 * also makes the "did the user change it" signal exact, which is what a
 * settings screen needs to know when to mark itself dirty.
 *
 * SNAPPING IS A DISPLAY DECISION AS MUCH AS A VALUE ONE. With `snapToStep` on,
 * a drag lands on whole steps, so a volume slider reads 74 rather than
 * 73.6274. It is on by default because a settings value with a fractional tail
 * looks like a bug to everybody who is not the person who wrote the slider.
 *
 * THE HIGHLIGHT SURVIVES THE DRAG LEAVING THE CONTROL. Grab the thumb, pull the
 * cursor off the row, and the accent stays until the button comes up — because
 * the control is still the one you are operating. Losing the highlight there is
 * the single most common bug in hand-rolled sliders.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiText.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

#include <string>

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct SettingSliderSpec {
    float value = 100.0f;
    float minValue = 0.0f;
    float maxValue = 100.0f;

    /* Increment the value snaps to while dragging, and the size of a
     * keyboard/gamepad nudge the caller applies itself. */
    float stepSize = 1.0f;
    bool  snapToStep = true;

    /* The readout to the right of the track. */
    bool        showValue = true;
    int         fractionDigits = 0;
    std::string valueSuffix;      /* appended verbatim, e.g. "%" */
    TextStyle   valueStyle{ 16.0f, FontWeight::Regular, 0.0f, UiColor::white() };

    /* Gap between the track and the readout, and the width reserved for the
     * readout so the track does not resize as digits come and go. */
    float valueGapPx = 24.0f;
    float valueMinWidthPx = 48.0f;

    UiColor normalColour = UiColor::white();
    UiColor accentColour = theme::accent();

    float fadeInSeconds = theme::kFadeInSeconds;
    float fadeOutSeconds = theme::kFadeOutSeconds;
    float fadeEase = theme::kFadeEase;

    float trackThicknessPx = 2.0f;
    Vec2  thumbSize{ 3.0f, 16.0f };
};

/* What the slider did this frame. One-shot carrier. */
struct SettingSliderResult {
    bool  hovered = false;
    bool  dragging = false;

    /* True on any frame the user moved it — the signal to write the setting
     * through and mark the screen dirty. */
    bool  changed = false;

    /* The value to store. Equal to the spec's value when nothing happened. */
    float value = 0.0f;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`.
 *
 * The VALUE, its range and its step are emphatically not dimensions. Scaling
 * them would make a volume slider read 150 on a 150% display, which is the
 * kind of bug that survives review because the screenshot looks fine. */
inline SettingSliderSpec scaled(const SettingSliderSpec& spec, float factor)
{
    SettingSliderSpec out = spec;
    out.valueStyle = ui::scaled(out.valueStyle, factor);
    out.valueGapPx *= factor;
    out.valueMinWidthPx *= factor;
    out.trackThicknessPx *= factor;
    out.thumbSize = out.thumbSize * factor;
    return out;
}

SettingSliderResult drawSettingSlider(UiContext& context, UiId id, const UiRect& bounds,
                                      const SettingSliderSpec& spec);

}  // namespace cromwell::ui
