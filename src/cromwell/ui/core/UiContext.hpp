/* UiContext.hpp — the immediate-mode frame: input in, draw list out, and the
 * small amount of state a widget is allowed to remember between the two.
 *
 * SINGLE RESPONSIBILITY: give the widget functions somewhere to put their
 * triangles, somewhere to read the cursor, and somewhere to keep the handful of
 * values that cannot be recomputed from scratch each frame.
 *
 * WHY IMMEDIATE MODE, GIVEN THE ORIGINALS WERE RETAINED WIDGETS. The Slate
 * versions of these controls are objects with a lifetime, a parent, a layout
 * pass and an invalidation protocol, and roughly two thirds of each file is
 * that machinery rather than the look. The look is what is worth carrying
 * across. Called immediately, a control is a function of (rect, spec, state) —
 * the same geometry, none of the tree.
 *
 * WHICH LEAVES EXACTLY ONE PROBLEM, AND THIS CLASS IS IT: some of what a
 * control shows is not derivable from its inputs. A hover fade is halfway
 * through, a progress bar's fill is gliding toward a value it has not reached,
 * a slider is mid-drag. That state belongs to the widget but cannot live in it,
 * because the widget is a function call that ends. So it lives here, keyed by
 * an id the caller supplies.
 *
 * IDS ARE THE ONE THING THE CALLER MUST GET RIGHT. Two controls sharing an id
 * share a hover fade and a drag; the same control changing id between frames
 * forgets everything and re-animates from scratch. Derive them from something
 * stable — a literal name, or a name plus a loop index — never from a pointer
 * that a reallocation can move, and never from the frame counter.
 *
 * CLICKS FIRE ON PRESS, NOT ON RELEASE. Deliberate, and inherited from the PO
 * buttons: menus feel markedly more responsive when the press is the commit,
 * and the press-drag-off-to-cancel gesture that release-firing buys is a
 * desktop-application idiom that nothing in a game menu needs. Anything that
 * genuinely wants cancellable presses can read held() and decide for itself.
 *
 * COLD CODE, AND THAT IS THE POINT. A frame of UI is a few dozen widgets and a
 * hash lookup each, against a simulation that is doing thousands of spatial
 * queries. Per CLAUDE.md that puts every rule about hot loops out of scope here
 * and buys clarity instead — but it still runs per frame, so it gets a profiler
 * zone (see ui/paint/UiPainter.hpp), because an unzoned system is invisible
 * rather than zero.
 *
 * ================= DISPLAY SCALE, AND WHY IT WORKS THIS WAY ================
 *
 * EVERYTHING PAST THIS CLASS IS IN DEVICE PIXELS. A rect, a spec's radiusPx, a
 * style's sizePx, a vertex, the cursor — all of them, all the way down to the
 * painter. There is no second coordinate space anywhere in the kit.
 *
 * The display scale is applied ONCE, ON THE WAY IN: a caller authors its layout
 * at a reference scale (1.0 = a 96 DPI monitor at 100%), multiplies through
 * `px()` and `scaled(spec, scale())`, and hands over device pixels. That is the
 * whole mechanism.
 *
 * WHY NOT SCALE AT THE PAINTER, which would be one multiply instead of many.
 * Because of the feather. Every shape here is antialiased by an explicit
 * one-pixel band of geometry (see ui/shape/Shapes.hpp), and that band has to be
 * ONE DEVICE PIXEL to do its job. Scaling the draw list at the end scales the
 * feather with it — two pixels of softening at 200%, three at 300% — which is
 * exactly the "why does the UI look blurry on my good monitor" complaint, and
 * it would be invisible on the machine it was written on. Scaling on the way in
 * leaves the feather as the one quantity that is never multiplied, which is
 * precisely what it should be.
 *
 * It is also what makes text crisp: sizePx arrives already multiplied, so the
 * font set rasterises an atlas at the size the glyphs are actually drawn at
 * rather than sampling one atlas at a different size. See UiFontSet.hpp, which
 * states the same contract from the other end.
 *
 * WHAT THE SCALE SHOULD BE: the monitor's content scale (raylib's
 * GetWindowScaleDPI) times whatever UI-size preference the player has set.
 * RESOLUTION is not part of it — a 4K monitor at 100% wants small crisp UI, and
 * pretending otherwise is how UI ends up enormous on a 27" 4K panel.
 * ==========================================================================
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/HoverFade.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/core/UiText.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace cromwell::ui {

/* A widget's identity, as a hash. Opaque — compare it, do not decode it. */
using UiId = std::uint64_t;

/* One frame of device state, as the UI needs it.
 *
 * ONE-SHOT DATA CARRIER (see UiColor.hpp): built by the caller from whatever
 * input plumbing the game has, read once by beginFrame, dead immediately after.
 * It is deliberately NOT cromwell::FrameInput — that type is the game's own
 * action mapping and includes raylib, and the UI needs a cursor and a clock,
 * nothing more. */
struct UiInput {
    Vec2   mousePosition;
    bool   mouseDown = false;      /* held right now                            */
    bool   mousePressed = false;   /* went down THIS frame                      */
    bool   mouseReleased = false;  /* came up THIS frame                        */

    double timeSeconds = 0.0;      /* monotonic; drives every fade and spinner  */
    float  deltaSeconds = 0.0f;

    Vec2   screenSize;             /* device pixels, for full-screen layouts    */

    /* Device pixels per reference pixel — the monitor's content scale times the
     * player's UI-size preference. 1.0 means "author in device pixels", which
     * is the right answer on a 100% display and a sane default everywhere.
     *
     * Clamped on the way in rather than trusted: a zero from a driver that has
     * not reported a scale yet would collapse the entire UI to a point. */
    float  scale = 1.0f;
};

/* What a widget is allowed to remember between frames.
 *
 * Private members with accessors rather than a bag of fields: it outlives the
 * frame that made it, which is precisely the line the project's one-shot-carrier
 * exception draws. The set is deliberately small and named after what the
 * widgets actually need — a generic scratch map would let any widget stash
 * anything, and the next person would have no way to know what is live. */
class WidgetState {
public:
    /* The control's own hover/highlight blend. */
    HoverFade& fade() { return fade_; }

    /* Extra blends for compound controls whose parts highlight separately — the
     * stepper's two chevrons are the case that needs them. Index is clamped, so
     * a caller asking for a slot that does not exist gets a real fade rather
     * than undefined behaviour. */
    HoverFade& auxFade(int index);

    /* Where an animated value actually is on screen, as opposed to where it was
     * told to be — the loading bar's gliding fill. */
    float displayedValue() const { return displayedValue_; }
    void  setDisplayedValue(float value) { displayedValue_ = value; }

    /* False until the first frame the widget was drawn, so a bar can show its
     * bound value immediately instead of animating up from zero on the frame it
     * appears. */
    bool seen() const { return seen_; }
    void markSeen() { seen_ = true; }

    /* True while this widget holds the pointer — a slider being dragged keeps
     * its highlight even when the cursor strays off the track. */
    bool dragging() const { return dragging_; }
    void setDragging(bool dragging) { dragging_ = dragging; }

    /* The frame this state was last touched, so abandoned entries can be
     * dropped rather than accumulating for the life of the process. */
    std::uint64_t lastFrame() const { return lastFrame_; }
    void setLastFrame(std::uint64_t frame) { lastFrame_ = frame; }

private:
    HoverFade     fade_;
    HoverFade     auxFades_[2];
    float         displayedValue_ = 0.0f;
    bool          seen_ = false;
    bool          dragging_ = false;
    std::uint64_t lastFrame_ = 0;
};

/* What happened to a control this frame. One-shot carrier: returned by value,
 * read at the call site, gone. */
struct InteractionResult {
    bool hovered = false;
    bool held = false;      /* pressed and still down over this control */
    bool clicked = false;   /* fired on press — see the header note     */
};

class UiContext {
public:
    /* `metrics` must outlive the context. It is a reference rather than a
     * pointer because a UI with no way to measure text cannot lay anything out,
     * so "absent" is not a state worth representing. */
    explicit UiContext(const TextMetrics& metrics) : metrics_(&metrics) {}

    /* Clears the draw list, latches the frame's input, and retires widget state
     * nothing has touched for a while. */
    void beginFrame(const UiInput& input);

    /* Closes the frame. Currently only resolves the active id — kept as an
     * explicit call so the balance with beginFrame is visible at the call site,
     * and so there is somewhere obvious for anything deferred to go. */
    void endFrame();

    /* ---- what widgets draw into ---------------------------------------- */
    UiDrawList&       drawList() { return drawList_; }
    const UiDrawList& drawList() const { return drawList_; }
    const TextMetrics& metrics() const { return *metrics_; }

    /* ---- what widgets read --------------------------------------------- */
    double time() const { return input_.timeSeconds; }
    float  deltaSeconds() const { return input_.deltaSeconds; }
    Vec2   mousePosition() const { return input_.mousePosition; }
    bool   mouseDown() const { return input_.mouseDown; }
    bool   mousePressed() const { return input_.mousePressed; }
    bool   mouseReleased() const { return input_.mouseReleased; }
    Vec2   screenSize() const { return input_.screenSize; }
    UiRect screenRect() const { return { 0.0f, 0.0f, input_.screenSize.x, input_.screenSize.y }; }

    /* ---- display scale -------------------------------------------------- */

    /* Device pixels per reference pixel. See the contract in the header. */
    float scale() const { return scale_; }

    /* A reference-pixel measurement in device pixels. The vocabulary a caller's
     * layout is written in: `context.px(24.0f)` is a 24px margin at any
     * display scale.
     *
     * NOT rounded. Rounding here would compound — twenty stacked rows each
     * losing half a pixel drift visibly apart — and the things that genuinely
     * need whole pixels (hard-edged rects) are snapped where they are drawn,
     * once, against their final position. */
    float px(float referencePixels) const { return referencePixels * scale_; }
    Vec2  px(Vec2 referencePixels) const { return referencePixels * scale_; }
    UiRect px(const UiRect& referencePixels) const { return scaled(referencePixels, scale_); }

    /* ---- identity ------------------------------------------------------- */

    /* FNV-1a over the name. Cheap, stable across runs, and good enough for a
     * few dozen widgets — a collision would need two names in the same frame
     * hashing identically, at which point they would share a hover fade and the
     * symptom would be obvious rather than subtle. */
    static UiId id(std::string_view name);

    /* The same, with an index folded in, for a control drawn in a loop. */
    static UiId id(std::string_view name, int index);

    /* An id for a part of a compound control — the stepper's two chevrons, say
     * — derived from the whole control's id, so the caller only has to name one
     * thing and the parts cannot collide with a neighbouring instance's. */
    static UiId childId(UiId parent, std::string_view part);

    /* ---- interaction ---------------------------------------------------- */

    /* The standard button behaviour: hover test, press capture, click. Call it
     * from any control that takes the pointer; the visuals are the caller's.
     *
     * `bounds` is tested against the CURSOR POSITION rather than against a
     * hover flag maintained elsewhere, which is what the PO widgets ended up
     * doing too: a flag derived from event routing blinks off for a frame
     * around clicks, and the blink reads as a white flash on a hover-tinted
     * control. The cursor does not move when it is clicked, so a geometric test
     * cannot blink. */
    InteractionResult interact(UiId id, const UiRect& bounds);

    /* Hover alone, for controls that highlight but take no input (a label
     * reacting to its row, a segment bar previewing a value). */
    bool isHovered(const UiRect& bounds) const;

    /* Per-widget persistent state, created on first use. */
    WidgetState& state(UiId id);

    /* True when any control claimed the cursor this frame — the signal the game
     * needs before treating a click as a click on the world. Valid after the
     * widgets have been submitted, so read it at the end of the frame. */
    bool wantsMouse() const { return hotId_ != 0 || activeId_ != 0; }

    /* The control currently holding the press, 0 for none. Exposed for controls
     * that must know whether the press they are seeing is theirs (a slider
     * continuing a drag that started on its track). */
    UiId activeId() const { return activeId_; }
    void setActiveId(UiId id) { activeId_ = id; }

    std::uint64_t frameIndex() const { return frameIndex_; }

private:
    /* Drops state for widgets that have not been drawn for a while. Without it,
     * a menu that shows a hundred different screens over a session keeps every
     * widget's fade alive forever. The threshold is generous — a control hidden
     * behind a tab for a second should not forget it was hovered. */
    void retireStaleState();

    const TextMetrics* metrics_;
    UiDrawList         drawList_;
    UiInput            input_;

    /* Latched from the input and clamped, so every reader gets a usable number
     * rather than each having to defend against a zero. */
    float scale_ = 1.0f;

    /* The control under the cursor, resolved as widgets are submitted. LAST
     * claim wins, because later widgets draw on top of earlier ones and the
     * thing you can see is the thing you are pointing at. */
    UiId hotId_ = 0;

    /* The control that took the press and keeps it until the button comes up,
     * so a drag that leaves the control's bounds still belongs to it. */
    UiId activeId_ = 0;

    std::uint64_t frameIndex_ = 0;

    std::unordered_map<UiId, WidgetState> states_;
};

}  // namespace cromwell::ui
