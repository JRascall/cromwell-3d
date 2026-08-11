/* WidgetTests.cpp — headless verification of the UI widget kit.
 *
 * The kit's geometry lives in cromwell_base and produces a draw list rather
 * than pixels (see cromwell/ui/core/UiDrawList.hpp), which is what makes this
 * possible at all: every assertion below is about vertices, commands and
 * returned values, and none of it needs a window.
 *
 * WHAT IS WORTH TESTING HERE, AND WHAT IS NOT. Nothing here asserts that a
 * spinner looks good — that is what the gallery (F2) is for, and no test will
 * ever answer it. What IS worth pinning down is the arithmetic that is easy to
 * get subtly wrong and impossible to eyeball: that a fade reaches its target
 * exactly rather than asymptotically, that a rounded outline's normals point
 * outward, that a wrapped paragraph's lines fit, that a slider maps a drag to
 * the value it claims, that a card's measured height is the height it draws.
 * Those are the failures that survive a look at the screen.
 */
#include "cromwell/ui/control/BorderButton.hpp"
#include "cromwell/ui/control/Label.hpp"
#include "cromwell/ui/control/SettingSlider.hpp"
#include "cromwell/ui/control/SettingStepper.hpp"
#include "cromwell/ui/control/TextButton.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/gauge/SegmentBar.hpp"
#include "cromwell/ui/gauge/SegmentRing.hpp"
#include "cromwell/ui/loader/ActivitySpinner.hpp"
#include "cromwell/ui/loader/LoadingBar.hpp"
#include "cromwell/ui/loader/LoadingRing.hpp"
#include "cromwell/ui/panel/TipPanel.hpp"
#include "cromwell/ui/shape/Outline.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cromwell;
using namespace cromwell::ui;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

bool nearly(float a, float b, float tolerance = 1.0e-3f)
{
    return std::abs(a - b) <= tolerance;
}

/* Fixed-width glyphs, so every layout assertion below is exact arithmetic
 * rather than a comparison against whatever a real font happened to rasterise.
 * A character is half the point size wide, which is close enough to a real
 * face's average that the numbers in the tests stay plausible. */
class StubMetrics final : public TextMetrics {
public:
    static constexpr float kCharWidthFactor = 0.5f;
    static constexpr float kLineHeightFactor = 1.25f;

    Vec2 measure(std::string_view text, const TextStyle& style) const override
    {
        const float height = lineHeight(style);
        if (text.empty()) {
            return { 0.0f, height };
        }
        const auto count = static_cast<float>(text.size());
        return { count * style.sizePx * kCharWidthFactor
                     + (count - 1.0f) * style.letterSpacingPx,
                 height };
    }

    float lineHeight(const TextStyle& style) const override
    {
        return style.sizePx * kLineHeightFactor;
    }
};

/* A context wound to a known clock, so the time-driven widgets are
 * reproducible. Advancing it is how the fade and glide tests step. */
class Harness {
public:
    Harness() : context_(metrics_) { step(0.0f); }

    UiContext& context() { return context_; }

    /* Advances the clock by `seconds` and starts a frame. */
    void step(float seconds)
    {
        time_ += static_cast<double>(seconds);
        input_.timeSeconds = time_;
        input_.deltaSeconds = seconds;
        context_.beginFrame(input_);
    }

    void setMouse(Vec2 position, bool down = false, bool pressed = false)
    {
        input_.mousePosition = position;
        input_.mouseDown = down;
        input_.mousePressed = pressed;
    }

    /* A frame with the button up. Needed between two presses: the context holds
     * the press capture until the button comes up (so a drag that leaves a
     * control still belongs to it), which means a second press without a
     * release is not a second click — on a real mouse it could not be one. */
    void release()
    {
        setMouse(input_.mousePosition, false, false);
        step(0.016f);
    }

    void setScreen(Vec2 size) { input_.screenSize = size; }
    void setScale(float scale) { input_.scale = scale; }

private:
    StubMetrics metrics_;
    UiInput     input_;
    double      time_ = 0.0;
    UiContext   context_;
};

/* ---- colour ------------------------------------------------------------- */

void srgbRoundTrips()
{
    /* Every widget colour is authored as a hex triplet, decoded to linear for
     * blending and encoded back at the vertex. A round trip that drifted would
     * make the palette subtly wrong everywhere at once. */
    const std::uint8_t samples[] = { 0, 1, 17, 95, 128, 224, 254, 255 };
    for (const std::uint8_t value : samples) {
        const UiColor colour = UiColor::fromSrgb8(value, value, value, 255);
        const std::uint32_t packed = colour.toSrgb8();
        const auto back = static_cast<std::uint8_t>(packed & 0xFFu);
        CHECK(back == value, "srgb round trip: %u came back as %u", value, back);
    }

    /* Alpha is NOT transfer-encoded — it is coverage, not colour. Half opacity
     * has to come back as half. */
    const UiColor half = UiColor::white().withAlpha(0.5f);
    const auto alpha = static_cast<std::uint8_t>((half.toSrgb8() >> 24) & 0xFFu);
    CHECK(alpha == 128, "alpha should stay linear, got %u", alpha);
}

/* ---- layout ------------------------------------------------------------- */

void rectsClipAndAlign()
{
    const UiRect outer{ 0.0f, 0.0f, 100.0f, 50.0f };

    const UiRect inset = outer.inset(10.0f);
    CHECK(nearly(inset.x, 10.0f) && nearly(inset.width, 80.0f), "inset should shrink on both sides");

    /* An inset larger than the box clamps to empty rather than inverting — an
     * inside-out content rect silently mirrors a layout. */
    const UiRect over = outer.inset(80.0f);
    CHECK(over.width == 0.0f && over.height == 0.0f, "over-inset should clamp to empty");

    const UiRect overlap = outer.intersected({ 50.0f, 25.0f, 200.0f, 200.0f });
    CHECK(nearly(overlap.width, 50.0f) && nearly(overlap.height, 25.0f), "intersection wrong");

    const UiRect apart = outer.intersected({ 500.0f, 500.0f, 10.0f, 10.0f });
    CHECK(apart.empty(), "disjoint rects should intersect to empty");

    const UiRect centred = alignIn(outer, { 20.0f, 10.0f },
                                   HorizontalAlign::Centre, VerticalAlign::Middle);
    CHECK(nearly(centred.x, 40.0f) && nearly(centred.y, 20.0f), "centre alignment wrong");

    const UiRect bottomRight = alignIn(outer, { 20.0f, 10.0f },
                                       HorizontalAlign::Right, VerticalAlign::Bottom);
    CHECK(nearly(bottomRight.x, 80.0f) && nearly(bottomRight.y, 40.0f), "far alignment wrong");
}

void drawListBatchesAndClips()
{
    UiDrawList list;

    shapes::addRect(list, { 0.0f, 0.0f, 10.0f, 10.0f }, UiColor::white());
    shapes::addRect(list, { 10.0f, 0.0f, 10.0f, 10.0f }, UiColor::white());
    CHECK(list.commands().size() == 1, "consecutive triangles should share one command, got %zu",
          list.commands().size());
    CHECK(list.indexCount() == 12, "two quads should be 12 indices, got %u", list.indexCount());

    /* Text is the only thing here that samples a texture, so it has to break
     * the batch — a text run drawn inside a triangle span would draw in the
     * wrong order. */
    TextRun run;
    run.text = "x";
    list.addText(run);
    shapes::addRect(list, { 0.0f, 20.0f, 10.0f, 10.0f }, UiColor::white());
    CHECK(list.commands().size() == 3, "text should split the batch, got %zu commands",
          list.commands().size());

    /* Nested clips can only shrink. */
    list.pushClip({ 0.0f, 0.0f, 100.0f, 100.0f });
    list.pushClip({ 50.0f, 50.0f, 100.0f, 100.0f });
    CHECK(nearly(list.clip().width, 50.0f), "nested clip should intersect, got %f", list.clip().width);
    list.popClip();
    CHECK(nearly(list.clip().width, 100.0f), "pop should restore the parent clip");
    list.popClip();

    list.clear();
    CHECK(list.empty() && list.commands().empty(), "clear should empty the list");
}

/* ---- outlines ----------------------------------------------------------- */

void outlineNormalsPointOutward()
{
    /* The feather, the glow and the stroke all offset ALONG these normals. One
     * pointing inward turns a shape's antialiasing into a bite out of it. */
    Outline outline;

    outline.buildCapsule({ 0.0f, 0.0f }, { 0.0f, 20.0f }, 5.0f, 8);
    CHECK(outline.size() == 18, "capsule should emit 2*(segments+1) points, got %zu", outline.size());

    const Vec2 axisCentre{ 0.0f, 10.0f };
    for (std::size_t index = 0; index < outline.size(); ++index) {
        const Vec2 normal = outline.normal(index);
        CHECK(nearly(normal.length(), 1.0f), "capsule normal %zu is not unit length", index);

        /* Outward for a convex shape means "away from the interior", which for
         * a capsule is: away from the nearest point on its axis. Using the
         * shape's centre is close enough for a test — the cap points are the
         * only ones where the two differ, and they still agree in sign. */
        const Vec2 fromCentre = outline.position(index) - axisCentre;
        CHECK(normal.dot(fromCentre) > 0.0f, "capsule normal %zu points inward", index);
    }

    /* A square plate: corners emit coincident points with rotating normals, so
     * a halo turns the corner instead of spiking off it. */
    outline.buildRect({ 0.0f, 0.0f, 40.0f, 20.0f }, 0.0f, 4);
    CHECK(outline.size() == 20, "rect should emit 4*(segments+1) points, got %zu", outline.size());

    const Vec2 rectCentre{ 20.0f, 10.0f };
    for (std::size_t index = 0; index < outline.size(); ++index) {
        const Vec2 fromCentre = outline.position(index) - rectCentre;
        CHECK(outline.normal(index).dot(fromCentre) > 0.0f, "rect normal %zu points inward", index);
    }

    /* A sheared chip — the segment bar's shape, and the case where a naive
     * per-edge normal would be wrong at the corners. */
    const Vec2 chip[4] = { { 10.0f, 0.0f }, { 30.0f, 0.0f }, { 20.0f, 40.0f }, { 0.0f, 40.0f } };
    outline.buildConvexPolygon(chip, 4);
    CHECK(outline.size() == 12, "convex polygon should emit 3 points per corner, got %zu",
          outline.size());

    const Vec2 chipCentre{ 15.0f, 20.0f };
    for (std::size_t index = 0; index < outline.size(); ++index) {
        const Vec2 normal = outline.normal(index);
        CHECK(nearly(normal.length(), 1.0f), "chip normal %zu is not unit length", index);
        CHECK(normal.dot(outline.position(index) - chipCentre) > 0.0f,
              "chip normal %zu points inward", index);
    }
}

/* ---- text --------------------------------------------------------------- */

void paragraphsWrapWithinTheirWidth()
{
    StubMetrics metrics;
    TextStyle style;
    style.sizePx = 10.0f;   /* 5px per character with the stub */

    std::vector<std::string> lines;
    metrics.wrap("the quick brown fox jumps over the lazy dog", 60.0f, style, lines);

    CHECK(lines.size() > 1, "a long paragraph should wrap, got %zu line(s)", lines.size());
    for (const std::string& line : lines) {
        CHECK(metrics.measure(line, style).x <= 60.0f + 1.0e-3f,
              "wrapped line overflows: '%s'", line.c_str());
    }

    /* A single word longer than the limit is left over-long rather than broken
     * mid-word — hyphenation needs a dictionary. */
    metrics.wrap("supercalifragilistic", 10.0f, style, lines);
    CHECK(lines.size() == 1, "an over-long word should not be broken, got %zu lines", lines.size());

    /* An authored newline is a statement, not a suggestion. */
    metrics.wrap("one\ntwo", 1000.0f, style, lines);
    CHECK(lines.size() == 2, "explicit newline should break, got %zu lines", lines.size());

    /* The trailing-dots animation cycles and respects an authored peak. */
    CHECK(animateTrailingDots("Loading", 0.5f, 0.0) == "Loading.", "dots should start at one");
    CHECK(animateTrailingDots("Loading", 0.5f, 0.5) == "Loading..", "dots should step with time");
    CHECK(animateTrailingDots("Loading", 0.5f, 1.5) == "Loading.",
          "three dots is the default peak, so the fourth step wraps to one");

    /* Dots already on the authored string raise the peak, which is how the
     * animation is configured in the copy rather than beside it. */
    CHECK(animateTrailingDots("Loading....", 0.5f, 1.5) == "Loading....",
          "authored dots should set the peak");
}

/* ---- fades and glides --------------------------------------------------- */

void fadesArriveExactly()
{
    HoverFade fade;
    double now = 0.0;

    fade.advance(true, 0.1f, 0.2f, now);   /* first call only sets the clock */
    for (int step = 0; step < 12; ++step) {
        now += 0.01;
        fade.advance(true, 0.1f, 0.2f, now);
    }
    CHECK(nearly(fade.alpha(), 1.0f), "a 0.1s fade should be complete after 0.12s, at %f",
          fade.alpha());

    /* Idempotent within a frame: several parts of one control asking for the
     * same fade must not advance it several times. */
    HoverFade shared;
    shared.advance(true, 0.1f, 0.2f, 0.0);
    const float first = shared.advance(true, 0.1f, 0.2f, 0.05);
    const float second = shared.advance(true, 0.1f, 0.2f, 0.05);
    CHECK(nearly(first, second), "same timestamp advanced the fade twice: %f then %f", first, second);

    /* Zero snaps, which is the documented way to turn the animation off. */
    HoverFade instant;
    instant.advance(true, 0.0f, 0.0f, 0.0);
    instant.advance(true, 0.0f, 0.0f, 0.016);
    CHECK(nearly(instant.alpha(), 1.0f), "a zero fade time should snap");
}

void loadingBarGlidesAndArrives()
{
    Harness harness;
    const UiId id = UiContext::id("bar");
    const UiRect bounds{ 0.0f, 0.0f, 200.0f, 10.0f };

    LoadingBarSpec spec;
    spec.progress = 0.8f;
    spec.fillAnimationSeconds = 0.5f;

    /* First frame shows the bound value rather than animating up from zero — a
     * bar that appears mid-load should appear at the load's position. */
    drawLoadingBar(harness.context(), id, bounds, spec);
    CHECK(nearly(harness.context().state(id).displayedValue(), 0.8f),
          "first frame should snap to the value, at %f",
          harness.context().state(id).displayedValue());

    /* A later jump glides: one frame in, it must have moved but not arrived. */
    spec.progress = 0.2f;
    harness.step(0.1f);
    drawLoadingBar(harness.context(), id, bounds, spec);
    const float midway = harness.context().state(id).displayedValue();
    CHECK(midway < 0.8f && midway > 0.2f, "the fill should glide, not jump: at %f", midway);

    /* And it must ARRIVE — the whole reason for a constant rate rather than an
     * exponential approach. */
    for (int step = 0; step < 10; ++step) {
        harness.step(0.1f);
        drawLoadingBar(harness.context(), id, bounds, spec);
    }
    CHECK(nearly(harness.context().state(id).displayedValue(), 0.2f),
          "the fill should reach its target, stuck at %f",
          harness.context().state(id).displayedValue());
}

/* ---- loaders ------------------------------------------------------------ */

void loadingRingDrawsTrackThenArc()
{
    Harness harness;
    const Vec2 centre{ 100.0f, 100.0f };

    LoadingRingSpec spec;
    spec.style = LoadingRingStyle::Progress;
    spec.progress = 0.0f;
    spec.glowStrength = 0.0f;   /* the halo is tested by its own arithmetic */

    drawLoadingRing(harness.context(), centre, spec);
    const std::uint32_t trackOnly = harness.context().drawList().vertexCount();
    CHECK(trackOnly > 0, "an empty ring should still draw its track");

    /* Every vertex must sit inside the ring's own box — a radius or a feather
     * applied twice shows up here before it shows up on screen. */
    for (const UiVertex& vertex : harness.context().drawList().vertices()) {
        const float distance = std::sqrt((vertex.x - centre.x) * (vertex.x - centre.x)
                                       + (vertex.y - centre.y) * (vertex.y - centre.y));
        CHECK(distance <= spec.radiusPx + shapes::kFeatherPx + 1.0e-3f,
              "ring vertex escapes its radius: %f", distance);
    }

    harness.step(0.016f);
    spec.progress = 0.5f;
    drawLoadingRing(harness.context(), centre, spec);
    const std::uint32_t half = harness.context().drawList().vertexCount();
    CHECK(half > trackOnly, "an arc at 50%% should add geometry over the bare track");

    /* A full circle has no ends, so it must not pay for caps. */
    harness.step(0.016f);
    spec.progress = 1.0f;
    drawLoadingRing(harness.context(), centre, spec);
    const std::uint32_t full = harness.context().drawList().vertexCount();

    harness.step(0.016f);
    spec.progress = 0.99f;
    drawLoadingRing(harness.context(), centre, spec);
    const std::uint32_t almost = harness.context().drawList().vertexCount();
    CHECK(almost > full, "a closed ring should drop its caps (full %u, 99%% %u)", full, almost);
}

void spinnerFadesItsTail()
{
    Harness harness;

    ActivitySpinnerSpec spec;
    spec.spokeCount = 8;
    spec.glowStrength = 0.0f;
    drawActivitySpinner(harness.context(), { 50.0f, 50.0f }, spec);
    const std::uint32_t eight = harness.context().drawList().vertexCount();

    harness.step(0.016f);
    spec.spokeCount = 12;
    drawActivitySpinner(harness.context(), { 50.0f, 50.0f }, spec);
    const std::uint32_t twelve = harness.context().drawList().vertexCount();
    CHECK(twelve > eight, "more spokes should mean more geometry (%u vs %u)", twelve, eight);
    CHECK(twelve * 2 == eight * 3, "geometry should scale linearly with spoke count");

    /* The tail: at t = 0 the bright spoke is index 0, and the one just behind
     * it in the walk order must be dimmer. Read off the packed alphas, which is
     * the only thing that distinguishes the spokes. */
    harness.step(0.016f);
    spec.spokeCount = 8;
    drawActivitySpinner(harness.context(), { 50.0f, 50.0f }, spec);

    const auto& vertices = harness.context().drawList().vertices();
    CHECK(!vertices.empty(), "spinner drew nothing");
    if (!vertices.empty()) {
        const std::uint32_t firstAlpha = (vertices.front().rgba >> 24) & 0xFFu;
        const std::uint32_t lastAlpha = (vertices[vertices.size() / 2].rgba >> 24) & 0xFFu;
        CHECK(firstAlpha != lastAlpha, "every spoke has the same alpha - the tail is not fading");
    }
}

/* ---- gauges ------------------------------------------------------------- */

void segmentRingRespectsItsGapsAndFill()
{
    Harness harness;

    SegmentRingSpec spec;
    spec.segmentCount = 4;
    spec.progress = 0.0f;
    spec.glowStrength = 0.0f;
    spec.discColour = UiColor::transparent();

    drawSegmentRing(harness.context(), { 100.0f, 100.0f }, spec);
    const std::uint32_t trackOnly = harness.context().drawList().vertexCount();

    /* At full progress the fill covers exactly what the track does, so it must
     * cost the same again — the chip-cut arithmetic is shared, and a fill that
     * came out a different size would mean the two disagree about where a chip
     * ends. */
    harness.step(0.016f);
    spec.progress = 1.0f;
    drawSegmentRing(harness.context(), { 100.0f, 100.0f }, spec);
    const std::uint32_t filled = harness.context().drawList().vertexCount();
    CHECK(filled == trackOnly * 2, "a full fill should mirror the track (%u vs %u)",
          filled, trackOnly);

    /* The backing disc is optional and genuinely absent when transparent —
     * this is what makes the slots between chips see-through. */
    harness.step(0.016f);
    spec.discColour = UiColor::black().withAlpha(0.8f);
    drawSegmentRing(harness.context(), { 100.0f, 100.0f }, spec);
    CHECK(harness.context().drawList().vertexCount() > filled,
          "a visible disc should add geometry");
}

void segmentBarPreviewsUnderTheCursor()
{
    Harness harness;
    const UiId id = UiContext::id("segbar");

    SegmentBarSpec spec;
    spec.segmentCount = 6;
    spec.value = 2;
    spec.hoverPreview = true;
    spec.segmentSize = { 20.0f, 40.0f };
    spec.slantPx = 0.0f;
    spec.spacingPx = 0.0f;
    spec.horizontalAlign = HorizontalAlign::Left;
    spec.verticalAlign = VerticalAlign::Top;

    const UiRect bounds{ 0.0f, 0.0f, 120.0f, 40.0f };

    /* Cursor away: the bar shows its own value. */
    harness.setMouse({ 500.0f, 500.0f });
    harness.step(0.016f);
    SegmentBarResult result = drawSegmentBar(harness.context(), id, bounds, spec);
    CHECK(!result.hovered && result.previewValue == 2,
          "an unhovered bar should report its own value, got %d", result.previewValue);

    /* Cursor over the fourth chip (x 60..80): preview 4. */
    harness.setMouse({ 70.0f, 20.0f });
    harness.step(0.016f);
    result = drawSegmentBar(harness.context(), id, bounds, spec);
    CHECK(result.hovered, "the bar should report hover");
    CHECK(result.previewValue == 4, "expected a preview of 4, got %d", result.previewValue);

    /* Preview does not commit — the caller decides what a click means. */
    CHECK(spec.value == 2, "the bar must not write back to its spec");
}

/* ---- controls ----------------------------------------------------------- */

void buttonsMeasureTheirContent()
{
    Harness harness;

    TextButtonSpec text;
    text.text = "play";       /* 4 characters */
    text.textStyle.sizePx = 16.0f;
    text.textStyle.letterSpacingPx = 0.0f;
    text.hoverAnim = HighlightAnim::Fade;

    /* Under cross-fade the button is exactly its text: padding around nothing
     * visible would be a hit area bigger than the thing you can see. */
    Vec2 size = measureTextButton(harness.context(), text);
    CHECK(nearly(size.x, 4.0f * 8.0f), "cross-fade button should not pad, got %f", size.x);

    /* Under a sweep the plate is real, so its padding counts. */
    text.hoverAnim = HighlightAnim::SweepRight;
    text.platePadding = UiPadding::symmetric(10.0f, 4.0f);
    size = measureTextButton(harness.context(), text);
    CHECK(nearly(size.x, 4.0f * 8.0f + 20.0f), "sweep button should pad, got %f", size.x);

    /* The border button's key gap collapses with the prompt. */
    BorderButtonSpec border;
    border.text = "close";
    border.keyText = "ESC";
    border.labelStyle.letterSpacingPx = 0.0f;
    border.keyStyle.letterSpacingPx = 0.0f;
    const Vec2 withKey = measureBorderButton(harness.context(), border);

    border.keyText.clear();
    const Vec2 withoutKey = measureBorderButton(harness.context(), border);
    CHECK(withoutKey.x < withKey.x - border.keyGapPx,
          "dropping the keycap should drop the gap too (%f vs %f)", withoutKey.x, withKey.x);
}

void buttonsClickOnPress()
{
    Harness harness;
    const UiId id = UiContext::id("button");
    const UiRect bounds{ 0.0f, 0.0f, 100.0f, 30.0f };

    TextButtonSpec spec;
    spec.text = "go";

    /* Hover without pressing: no click. */
    harness.setMouse({ 50.0f, 15.0f }, false, false);
    harness.step(0.016f);
    CHECK(!drawTextButton(harness.context(), id, bounds, spec).clicked,
          "hovering must not click");

    /* Press: click, on the press itself — see the note in UiContext.hpp. */
    harness.setMouse({ 50.0f, 15.0f }, true, true);
    harness.step(0.016f);
    const InteractionResult pressed = drawTextButton(harness.context(), id, bounds, spec);
    CHECK(pressed.clicked && pressed.held, "a press should click and hold");

    /* Holding is not a second click. */
    harness.setMouse({ 50.0f, 15.0f }, true, false);
    harness.step(0.016f);
    CHECK(!drawTextButton(harness.context(), id, bounds, spec).clicked,
          "holding must not re-click");

    /* Pressing outside the control does nothing to it. */
    harness.release();
    harness.setMouse({ 500.0f, 500.0f }, true, true);
    harness.step(0.016f);
    CHECK(!drawTextButton(harness.context(), id, bounds, spec).clicked,
          "a press elsewhere must not click this control");

    /* While one control holds the press, a second cannot take it — the capture
     * is what stops a drag that started on a slider from clicking whatever it
     * is dragged over. */
    harness.release();
    const UiId other = UiContext::id("other button");
    const UiRect otherBounds{ 0.0f, 40.0f, 100.0f, 30.0f };

    harness.setMouse({ 50.0f, 15.0f }, true, true);
    harness.step(0.016f);
    drawTextButton(harness.context(), id, bounds, spec);          /* takes the press */

    harness.setMouse({ 50.0f, 55.0f }, true, true);
    harness.step(0.016f);
    CHECK(!drawTextButton(harness.context(), other, otherBounds, spec).clicked,
          "a held press must not be claimed by another control");
}

void sliderMapsDragsToValues()
{
    Harness harness;
    const UiId id = UiContext::id("slider");

    SettingSliderSpec spec;
    spec.value = 0.0f;
    spec.minValue = 0.0f;
    spec.maxValue = 100.0f;
    spec.stepSize = 10.0f;
    spec.snapToStep = true;
    spec.showValue = false;      /* keep the track's geometry simple to reason about */
    spec.thumbSize = { 0.0f, 16.0f };

    const UiRect bounds{ 0.0f, 0.0f, 100.0f, 24.0f };

    /* Press at the middle of the track: half of 0..100, snapped to 50. */
    harness.setMouse({ 50.0f, 12.0f }, true, true);
    harness.step(0.016f);
    SettingSliderResult result = drawSettingSlider(harness.context(), id, bounds, spec);
    CHECK(result.dragging, "a press on the track should start a drag");
    CHECK(nearly(result.value, 50.0f), "expected 50 at the midpoint, got %f", result.value);
    CHECK(result.changed, "moving the value should report a change");

    /* Dragging past the end clamps rather than running away. */
    spec.value = result.value;
    harness.setMouse({ 500.0f, 12.0f }, true, false);
    harness.step(0.016f);
    result = drawSettingSlider(harness.context(), id, bounds, spec);
    CHECK(nearly(result.value, 100.0f), "a drag past the end should clamp, got %f", result.value);

    /* Snapping lands on whole steps, which is the difference between a settings
     * value of 74 and one of 73.6274. */
    spec.value = result.value;
    harness.setMouse({ 33.0f, 12.0f }, true, false);
    harness.step(0.016f);
    result = drawSettingSlider(harness.context(), id, bounds, spec);
    CHECK(nearly(std::fmod(result.value, 10.0f), 0.0f) || nearly(std::fmod(result.value, 10.0f), 10.0f),
          "a snapped drag should land on a step, got %f", result.value);

    /* Releasing ends the drag, and the value stops following the cursor. */
    spec.value = result.value;
    harness.setMouse({ 90.0f, 12.0f }, false, false);
    harness.step(0.016f);
    const float before = spec.value;
    result = drawSettingSlider(harness.context(), id, bounds, spec);
    CHECK(!result.dragging, "releasing should end the drag");
    CHECK(nearly(result.value, before), "a released slider should not follow the cursor");
}

void stepperClampsAndWraps()
{
    Harness harness;
    const UiId id = UiContext::id("stepper");
    const std::vector<std::string> options = { "LOW", "MEDIUM", "HIGH" };

    SettingStepperSpec spec;
    spec.options = options;
    spec.selectedIndex = 0;
    spec.clickAreaSteps = true;
    spec.valueMinWidthPx = 60.0f;

    const UiRect bounds{ 0.0f, 0.0f, 200.0f, 24.0f };

    /* A click on the right half steps forward. */
    harness.setMouse({ 180.0f, 12.0f }, true, true);
    harness.step(0.016f);
    SettingStepperResult result = drawSettingStepper(harness.context(), id, bounds, spec);
    CHECK(result.changed && result.selectedIndex == 1,
          "a click on the right should step forward, got %d", result.selectedIndex);

    /* At the end, without wrap, it stops — and reports no change, which is what
     * stops a caller from writing a setting that did not move. */
    harness.release();
    spec.selectedIndex = 2;
    harness.setMouse({ 180.0f, 12.0f }, true, true);
    harness.step(0.016f);
    result = drawSettingStepper(harness.context(), id, bounds, spec);
    CHECK(!result.changed && result.selectedIndex == 2,
          "the last option should clamp, got %d (changed %d)", result.selectedIndex, result.changed);

    /* With wrap, it comes round. */
    harness.release();
    spec.wrap = true;
    harness.setMouse({ 180.0f, 12.0f }, true, true);
    harness.step(0.016f);
    result = drawSettingStepper(harness.context(), id, bounds, spec);
    CHECK(result.changed && result.selectedIndex == 0,
          "wrap should return to the first option, got %d", result.selectedIndex);

    /* A click on the left half steps back. */
    harness.release();
    spec.wrap = false;
    spec.selectedIndex = 2;
    harness.setMouse({ 20.0f, 12.0f }, true, true);
    harness.step(0.016f);
    result = drawSettingStepper(harness.context(), id, bounds, spec);
    CHECK(result.selectedIndex == 1, "a click on the left should step back, got %d",
          result.selectedIndex);
}

/* ---- panels ------------------------------------------------------------- */

void tipCardMeasuresWhatItDraws()
{
    Harness harness;

    TipPanelSpec spec;
    spec.title = "Energy";
    spec.bodyText = "Reactors produce energy each turn, and a base that draws more than "
                    "it makes browns out.";
    spec.footerKeyText = "TAB";
    spec.footerText = "Open Manual";
    spec.mediaHeightPx = 60.0f;

    const float width = 240.0f;
    const float measured = measureTipPanel(harness.context(), width, spec);

    harness.step(0.016f);
    const TipPanelResult drawn = drawTipPanel(harness.context(), { 0.0f, 0.0f, width, 0.0f }, spec);
    CHECK(nearly(measured, drawn.height),
          "measure and draw disagree about the card's height: %f vs %f", measured, drawn.height);
    CHECK(!drawn.mediaRect.empty(), "a card with a media height should return a media rect");

    /* Sections collapse when their content is empty — the property that lets
     * one widget serve a tutorial card and a one-line hint. */
    TipPanelSpec hint;
    hint.bodyText = "Hold Shift to queue a move order.";
    const float hintHeight = measureTipPanel(harness.context(), width, hint);
    CHECK(hintHeight < measured, "a body-only card should be shorter (%f vs %f)",
          hintHeight, measured);

    harness.step(0.016f);
    const TipPanelResult hintDrawn = drawTipPanel(harness.context(), { 0.0f, 0.0f, width, 0.0f }, hint);
    CHECK(hintDrawn.mediaRect.empty(), "a card with no media should return an empty media rect");
}

/* ---- display scale ------------------------------------------------------ */

void hardEdgesSnapToWholePixels()
{
    /* The reason addRect exists separately from the feathered builders: a hard
     * edge at a fractional coordinate is two rows at half intensity, not a
     * line. At a display scale of 1.5 almost every computed coordinate is
     * fractional, so this is the difference between crisp UI and grey mush. */
    UiDrawList list;
    shapes::addRect(list, { 10.4f, 100.5f, 50.3f, 1.2f }, UiColor::white());

    CHECK(!list.vertices().empty(), "addRect drew nothing");
    for (const UiVertex& vertex : list.vertices()) {
        CHECK(nearly(vertex.x, std::round(vertex.x)), "x %f is not on a pixel boundary", vertex.x);
        CHECK(nearly(vertex.y, std::round(vertex.y)), "y %f is not on a pixel boundary", vertex.y);
    }

    /* A hairline thinner than a pixel is still a request for a line, so it must
     * not round away to nothing. */
    list.clear();
    shapes::addRect(list, { 0.0f, 20.2f, 40.0f, 0.4f }, UiColor::white());
    CHECK(!list.vertices().empty(), "a sub-pixel hairline should still draw");

    float minimumY = 1.0e9f;
    float maximumY = -1.0e9f;
    for (const UiVertex& vertex : list.vertices()) {
        minimumY = std::min(minimumY, vertex.y);
        maximumY = std::max(maximumY, vertex.y);
    }
    CHECK(nearly(maximumY - minimumY, 1.0f), "a hairline should end up exactly 1px, got %f",
          maximumY - minimumY);
}

/* Is `point` inside any triangle of the list? Winding-agnostic on purpose —
 * the builders here deliberately do not agree on one (see the note on backface
 * culling in UiPainter.cpp), so a coverage test that assumed an orientation
 * would fail on half the kit for no reason. */
bool meshCovers(const UiDrawList& list, Vec2 point)
{
    const auto& vertices = list.vertices();
    const auto& indices = list.indices();

    const auto side = [](Vec2 a, Vec2 b, Vec2 p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    };

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const UiVertex& v0 = vertices[indices[i]];
        const UiVertex& v1 = vertices[indices[i + 1]];
        const UiVertex& v2 = vertices[indices[i + 2]];

        const Vec2 a{ v0.x, v0.y };
        const Vec2 b{ v1.x, v1.y };
        const Vec2 c{ v2.x, v2.y };

        /* DEGENERATE TRIANGLES ARE SKIPPED, and they must be: a zero-area
         * triangle puts every point exactly on all three of its edges, so a
         * naive test reports it as containing the whole plane. The kit emits
         * plenty of them — three consecutive points on a tight corner arc are
         * collinear to within float precision — so without this the test
         * passes for any input and proves nothing. */
        if (std::abs(side(a, b, c)) < 1.0e-3f) {
            continue;
        }

        const float ab = side(a, b, point);
        const float bc = side(b, c, point);
        const float ca = side(c, a, point);

        const bool allNonNegative = ab >= 0.0f && bc >= 0.0f && ca >= 0.0f;
        const bool allNonPositive = ab <= 0.0f && bc <= 0.0f && ca <= 0.0f;
        if (allNonNegative || allNonPositive) {
            return true;
        }
    }
    return false;
}

void filledShapesAreActuallyFilled()
{
    /* A shape asked to be filled must have geometry over its middle, not just
     * around its edge. Written after a pill rendered as a hollow outline for a
     * whole session — the interior fan was being thrown away and it read as a
     * deliberate style rather than as half a mesh.
     *
     * This covers the half that lives in the mesh. The other half was render
     * state (backface culling, see UiPainter.cpp) and is not visible from
     * here. */
    UiDrawList list;

    const UiRect pill{ 100.0f, 200.0f, 240.0f, 12.0f };
    shapes::addRoundedRect(list, pill, pill.height * 0.5f, UiColor::white(), shapes::kFeatherPx);

    CHECK(meshCovers(list, pill.centre()), "a filled pill has nothing over its centre");
    CHECK(meshCovers(list, { pill.x + 6.0f, pill.centre().y }),
          "a filled pill has nothing over its left cap");
    CHECK(!meshCovers(list, { pill.x - 20.0f, pill.centre().y }),
          "the pill covers a point well outside itself");

    /* Same for a spinner spoke, which is a capsule through the convex-fill
     * path, and for a slanted chip, which is the mitred-polygon path. */
    list.clear();
    Outline outline;
    outline.buildCapsule({ 50.0f, 50.0f }, { 50.0f, 90.0f }, 6.0f, 8);
    shapes::addConvexFill(list, outline, UiColor::white(), shapes::kFeatherPx);
    CHECK(meshCovers(list, { 50.0f, 70.0f }), "a capsule has nothing over its middle");

    list.clear();
    const Vec2 chip[4] = { { 20.0f, 0.0f }, { 40.0f, 0.0f }, { 30.0f, 40.0f }, { 10.0f, 40.0f } };
    outline.buildConvexPolygon(chip, 4);
    shapes::addConvexFill(list, outline, UiColor::white(), shapes::kFeatherPx);
    CHECK(meshCovers(list, { 25.0f, 20.0f }), "a slanted chip has nothing over its middle");

    /* And the disc, which fans from its centre rather than from a rim point. */
    list.clear();
    shapes::addDisc(list, { 0.0f, 0.0f }, 30.0f, UiColor::white(), shapes::kFeatherPx);
    CHECK(meshCovers(list, { 5.0f, -5.0f }), "a disc has nothing over its middle");
}

void specsScaleTheirPixelsAndNothingElse()
{
    /* Sizes scale; the things that are not sizes must not. Scaling a slider's
     * VALUE would make a volume setting read 150 on a 150% display, and the
     * screenshot would look perfectly fine. */
    SettingSliderSpec slider;
    slider.value = 70.0f;
    slider.maxValue = 100.0f;
    slider.stepSize = 5.0f;
    slider.trackThicknessPx = 2.0f;
    slider.valueStyle.sizePx = 16.0f;

    const SettingSliderSpec scaledSlider = scaled(slider, 2.0f);
    CHECK(nearly(scaledSlider.trackThicknessPx, 4.0f), "track thickness should scale");
    CHECK(nearly(scaledSlider.valueStyle.sizePx, 32.0f), "font size should scale");
    CHECK(nearly(scaledSlider.value, 70.0f), "the VALUE must not scale, got %f", scaledSlider.value);
    CHECK(nearly(scaledSlider.maxValue, 100.0f), "the range must not scale");
    CHECK(nearly(scaledSlider.stepSize, 5.0f), "the step must not scale");

    /* Times and counts are not dimensions either. */
    LoadingRingSpec ring;
    ring.radiusPx = 16.0f;
    ring.periodSeconds = 1.0f;
    ring.sweepDegrees = 270.0f;
    const LoadingRingSpec scaledRing = scaled(ring, 1.5f);
    CHECK(nearly(scaledRing.radiusPx, 24.0f), "radius should scale");
    CHECK(nearly(scaledRing.periodSeconds, 1.0f), "the period must not scale");
    CHECK(nearly(scaledRing.sweepDegrees, 270.0f), "the sweep angle must not scale");

    SegmentBarSpec bar;
    bar.segmentCount = 6;
    bar.value = 3;
    bar.segmentSize = { 16.0f, 40.0f };
    const SegmentBarSpec scaledBar = scaled(bar, 2.0f);
    CHECK(scaledBar.segmentCount == 6 && scaledBar.value == 3, "counts must not scale");
    CHECK(nearly(scaledBar.segmentSize.y, 80.0f), "segment size should scale");
}

void theFeatherIsNeverScaled()
{
    /* THE ONE THAT MATTERS. Antialiasing is a one-DEVICE-pixel band; if it
     * scaled with everything else, a 200% display would get two pixels of
     * softening and a 300% display three, which is precisely the "why is the UI
     * blurry on my good monitor" complaint — and it would be invisible on the
     * machine it was written on.
     *
     * Measured as the ring's outermost vertex: at scale 1 that is radius + 1,
     * at scale 2 it must be 2*radius + 1, NOT 2*(radius + 1). */
    const auto outermost = [](Harness& harness, float scale) {
        LoadingRingSpec spec;
        spec.style = LoadingRingStyle::Progress;
        spec.progress = 0.0f;         /* the track alone — no caps, no halo */
        spec.radiusPx = 20.0f;
        spec.thicknessPx = 5.0f;
        spec.glowStrength = 0.0f;

        harness.step(0.016f);
        drawLoadingRing(harness.context(), { 0.0f, 0.0f }, scaled(spec, scale));

        float furthest = 0.0f;
        for (const UiVertex& vertex : harness.context().drawList().vertices()) {
            furthest = std::max(furthest, std::sqrt(vertex.x * vertex.x + vertex.y * vertex.y));
        }
        return furthest;
    };

    Harness single;
    Harness doubled;
    const float atOne = outermost(single, 1.0f);
    const float atTwo = outermost(doubled, 2.0f);

    CHECK(nearly(atOne, 21.0f, 0.05f), "at scale 1, radius 20 + 1px feather = 21, got %f", atOne);
    CHECK(nearly(atTwo, 41.0f, 0.05f),
          "at scale 2 expected 40 + 1px feather = 41; got %f (42 means the feather scaled)", atTwo);
}

void contextClampsAndConvertsScale()
{
    Harness harness;

    harness.setScale(2.0f);
    harness.step(0.016f);
    CHECK(nearly(harness.context().scale(), 2.0f), "scale should latch");
    CHECK(nearly(harness.context().px(12.0f), 24.0f), "px() should convert reference to device");

    /* A driver that has not resolved the monitor's content scale reports zero,
     * and a default-constructed input carries it. Collapsing the whole UI to a
     * point is a worse failure than being the wrong size. */
    harness.setScale(0.0f);
    harness.step(0.016f);
    CHECK(harness.context().scale() >= 0.25f, "a zero scale must be clamped, got %f",
          harness.context().scale());

    harness.setScale(1000.0f);
    harness.step(0.016f);
    CHECK(harness.context().scale() <= 8.0f, "an absurd scale must be clamped, got %f",
          harness.context().scale());
}

void layoutsScaleWholesale()
{
    /* A card measured at 2x should be about twice as tall as the same card at
     * 1x — the text, the padding and the line height all scaled together. Not
     * exactly twice: the stub's line height is proportional, but wrapping is
     * re-decided at the scaled width, so this checks the ratio rather than an
     * equality. */
    Harness harness;

    TipPanelSpec spec;
    spec.title = "Energy";
    spec.bodyText = "Reactors produce energy each turn.";
    spec.footerKeyText = "TAB";
    spec.footerText = "Open Manual";
    spec.mediaHeightPx = 60.0f;

    const float single = measureTipPanel(harness.context(), 240.0f, spec);
    const float doubled = measureTipPanel(harness.context(), 480.0f, scaled(spec, 2.0f));

    CHECK(nearly(doubled, single * 2.0f, 1.0f),
          "a card at 2x should be twice as tall: %f vs %f", doubled, single * 2.0f);
}

/* ---- ids and state ------------------------------------------------------ */

void idsAreStableAndDistinct()
{
    CHECK(UiContext::id("play") == UiContext::id("play"), "the same name should hash the same");
    CHECK(UiContext::id("play") != UiContext::id("quit"), "different names should differ");
    CHECK(UiContext::id("row", 0) != UiContext::id("row", 1), "indices should distinguish ids");
    CHECK(UiContext::childId(UiContext::id("stepper"), "prev")
              != UiContext::childId(UiContext::id("stepper"), "next"),
          "child ids should differ by part");

    /* Two instances of one compound control must not share their parts. */
    CHECK(UiContext::childId(UiContext::id("a"), "prev")
              != UiContext::childId(UiContext::id("b"), "prev"),
          "child ids should differ by parent");
}

}  // namespace

int main()
{
    srgbRoundTrips();
    rectsClipAndAlign();
    drawListBatchesAndClips();
    outlineNormalsPointOutward();
    paragraphsWrapWithinTheirWidth();
    fadesArriveExactly();
    loadingBarGlidesAndArrives();
    loadingRingDrawsTrackThenArc();
    spinnerFadesItsTail();
    segmentRingRespectsItsGapsAndFill();
    segmentBarPreviewsUnderTheCursor();
    buttonsMeasureTheirContent();
    buttonsClickOnPress();
    sliderMapsDragsToValues();
    stepperClampsAndWraps();
    tipCardMeasuresWhatItDraws();
    hardEdgesSnapToWholePixels();
    filledShapesAreActuallyFilled();
    specsScaleTheirPixelsAndNothingElse();
    theFeatherIsNeverScaled();
    contextClampsAndConvertsScale();
    layoutsScaleWholesale();
    idsAreStableAndDistinct();

    if (g_failures == 0) {
        std::printf("widget tests: all passed\n");
        return 0;
    }
    std::printf("widget tests: %d failure(s)\n", g_failures);
    return 1;
}
