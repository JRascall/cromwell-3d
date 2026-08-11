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

    /* Builds and paints the gallery into the game's UI surface, which is what
     * brackets the frame and owns the fonts.
     *
     * `camera` is the scene's, for the world-anchored badges — the point of
     * which is to show that a widget over a world position is drawn in screen
     * space and is therefore exactly as crisp as one in a menu.
     *
     * No-op when hidden. Call inside BeginDrawing, after the scene. */
    void draw(GameUi& gameUi, const Camera3D& camera);

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
    void drawWorldAnchors(const Camera3D& camera);

    /* A section heading, and the running y cursor it advances. */
    float heading(const cromwell::ui::UiRect& column, float y, const char* text);

    /* A caption under a widget, centred in the box given. */
    void caption(const cromwell::ui::UiRect& box, const char* text);

    /* Reference pixels to device pixels, at the current display scale. Every
     * layout constant in this file goes through it. */
    float px(float referencePixels) const;

    bool visible_ = false;

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
