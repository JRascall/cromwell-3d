/* WidgetGallery.hpp — every widget in the kit, on one screen, live.
 *
 * SINGLE RESPONSIBILITY: exercise cromwell's UI controls so they can be LOOKED
 * at, because that is the only way to tell whether a fade is too slow or a
 * spinner's spokes are too fat.
 *
 * WHY A GALLERY AND NOT A TEST. The headless tests (xcom_widget_tests) pin down
 * what is checkable — that a ring's arc grows with its progress, that a chip row
 * leaves gaps, that a slider maps a drag to a value. None of that is the
 * question you actually have about a widget kit, which is "does it look right",
 * and no assertion answers it. So: one screen, everything on it, animating, with
 * the interactive controls actually interactive.
 *
 * IT LIVES IN game/ RATHER THAN IN cromwell BECAUSE IT IS AN APPLICATION. The
 * engine ships the widgets; a screen that arranges them, owns a font set and
 * responds to a key is exactly the sort of thing the one architectural rule
 * keeps out of the engine. A future project embedding cromwell writes its own,
 * or copies this one.
 *
 * F2 toggles it. It draws over everything, takes the cursor while it is up, and
 * costs nothing at all while it is down.
 */
#pragma once

#include "cromwell/camera/Camera.hpp"   /* named rather than relied on arriving
                                       * transitively — and QUALIFIED at every
                                       * use below, because raylib's global
                                       * `Camera` alias makes the bare name
                                       * ambiguous. Same trap FrameView.hpp
                                       * documents. */
#include "cromwell/sdf/WorldText.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "game/render/ui/GameUi.hpp"

#include <string>
#include <vector>

namespace game {

class WidgetGallery {
public:
    WidgetGallery();
    ~WidgetGallery();

    WidgetGallery(const WidgetGallery&) = delete;
    WidgetGallery& operator=(const WidgetGallery&) = delete;

    bool visible() const { return visible_; }
    void setVisible(bool visible) { visible_ = visible; }
    void toggleVisible() { visible_ = !visible_; }

    /* THE MSDF SAMPLE OFF, for a renderer that cannot draw it.
     *
     * Everything else in this gallery is UiDrawList commands, which means it
     * runs on any painter — that is the whole reason the kit narrowed to one.
     * The distance-field sample is the exception: it is real geometry in the
     * scene, drawn through raylib's BeginMode3D, so on the device path it would
     * be a raylib draw in the middle of a device frame.
     *
     * DEFAULTS ON, so the shipping renderer keeps it and nobody has to remember
     * to ask. The device path turns it off, and loses one of the gallery's
     * fifteen sections rather than the gallery.
     *
     * It goes when WorldText does — this is migration scaffolding, and the sign
     * of that is that the flag names a capability of the RENDERER while sitting
     * on a screen that should not know which one it is drawing through. */
    void setWorldTextSample(bool enabled) { worldTextSample_ = enabled; }

    /* Builds and paints the gallery into the game's UI surface, which is what
     * brackets the frame and owns the fonts.
     *
     * `camera` is the scene's, for the world-anchored badges — the point of
     * which is to show that a widget over a world position is drawn in screen
     * space and is therefore exactly as crisp as one in a menu.
     *
     * TAKES THE ENGINE'S CAMERA, NOT A raylib ONE, and converts at the one
     * place inside that needs it. The device renderer has to be able to call
     * this, and a `Camera3D` in the signature would put raylib back into a file
     * whose entire purpose is not to have it.
     *
     * No-op when hidden. Call after the scene, in place of GameUi::begin/end —
     * it brackets the frame itself. */
    void draw(GameUi& gameUi, const cromwell::ui::UiInput& input,
              const cromwell::Camera& camera);

private:
    /* Sub-sections, in the order they appear. Split up because one function
     * that drew all of this would be four hundred lines of layout arithmetic
     * and nobody would find anything in it. */
    void drawLoaders(cromwell::ui::UiRect column);
    void drawGauges(cromwell::ui::UiRect column);
    void drawControls(cromwell::ui::UiRect column);
    void drawPanels(cromwell::ui::UiRect column);

    /* Badges pinned to world positions, to show that world-space UI here is
     * screen-space UI at a projected point — see cromwell/ui/paint/WorldAnchor.hpp. */
    /* A size ladder, from the smallest the kit uses to far larger than it does.
     * THE POINT IS DIAGNOSTIC, not decorative: "the text looks soft" has two
     * completely different causes that look identical on one sample - a broken
     * rasterisation path, which is wrong at every size, and simply not having
     * many pixels to draw a letter with, which is only wrong at small ones.
     * One column of the same string at nine sizes separates them in a glance.
     * Every weight is shown too, because a hinting or coverage problem often
     * shows in the thin ones first. */
    void drawTypeSpecimen(cromwell::ui::UiRect band);

    void drawWorldAnchors(const Camera3D& camera);

    /* The OTHER kind of world-space text, beside the anchored badges above, so
     * the difference is visible rather than argued about. A badge is
     * screen-space UI at a projected point: constant size, perspective
     * quantised to a few crisp rasterisations. This is a distance field lying
     * in the scene, so it scales continuously with distance and stays sharp at
     * any of them, with no quantisation to hide a resample. Nameplates want
     * the first; signage and floating numbers want the second. */
    void drawMsdfSample(const Camera3D& camera);

    /* A section heading, and the running y cursor it advances. */
    float heading(const cromwell::ui::UiRect& column, float y, const char* text);

    /* A caption under a widget, centred in the box given. */
    void caption(const cromwell::ui::UiRect& box, const char* text);

    /* Reference pixels to device pixels, at the current display scale. Every
     * layout constant in this file goes through it. */
    float px(float referencePixels) const;

    bool visible_ = false;
    bool worldTextSample_ = true;

    /* Loaded on first use rather than at setup: the atlas is baked from the
     * licensed font pack (tools/fonts/bake_msdf.py) and is simply absent on a
     * checkout without it, so this stays unready and draws nothing. */
    cromwell::sdf::WorldText worldText_;
    bool worldTextTried_ = false;

    /* Borrowed for the duration of one draw, so the section helpers do not each
     * need it threaded through. Never outlives the call. */
    cromwell::ui::UiContext* context_ = nullptr;

    /* Live state the interactive controls edit, so the gallery behaves like a
     * real screen rather than a set of stills. */
    float                    sliderValue_ = 70.0f;
    int                      stepperIndex_ = 1;
    std::vector<std::string> stepperOptions_;
    int                      segmentValue_ = 3;
    int                      selectedTag_ = 1;
    float                    barProgress_ = 0.0f;
};

}  // namespace game
