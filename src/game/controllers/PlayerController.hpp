/* PlayerController.hpp — what the player's input MEANS.
 *
 * SINGLE RESPONSIBILITY: turn a frame of device input into changes to the
 * game's state — where the camera looks, which cell is hovered, what the path
 * preview is, which unit is selected, where a grenade went off, where a decal
 * is about to be stuck.
 *
 * IT OWNS NO GPU STATE AND CANNOT DRAW. Several of these actions invalidate
 * something the renderer built — blowing a wall open makes the static mesh, the
 * light bake and the reflection probes all describe a world that no longer
 * exists. Rather than reach into the renderer to fix them, this records what
 * changed in an Outcome and lets Application apply it. That is what keeps the
 * dependency one-way: input knows about the world, the renderer knows about the
 * world, and neither knows about the other.
 *
 * Split out of Application, which was 1615 lines doing this, the rendering and
 * the frame loop at once.
 */
#pragma once

#include "raylib.h"
#include "cromwell/decal/Decal.hpp"
#include "cromwell/decal/DecalSet.hpp"
#include "cromwell/input/FrameInput.hpp"
#include "cromwell/input/HoverTracker.hpp"
#include "cromwell/input/PointerDrag.hpp"
#include "cromwell/ribbon/RibbonRenderer.hpp"
#include "game/lattice/Cell.hpp"
#include "game/movement/search/PathPoint.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/picking/SurfacePicker.hpp"
#include "game/render/dev/DevView.hpp"
#include "game/render/scene/CutawayView.hpp"
#include "game/state/GameState.hpp"
#include "game/state/RingSelector.hpp"

#include <optional>
#include <string>
#include <vector>

namespace game {

using namespace cromwell;

class CameraPawn;
}  // namespace game

namespace cromwell {
class Camera;
class CameraDirector;
class Viewport;
}

namespace game {

class PlayerController {
public:
    /* What this frame's input invalidated. Application drains it after
     * stepping the controller and tells the renderer; nothing here touches a
     * GPU resource itself.
     *
     * Flags rather than direct calls because one action can invalidate several
     * things at once — a grenade changes the geometry, the lighting AND the
     * reach field — and a caller that has to remember all three in the right
     * order is a caller that will eventually forget one. */
    struct Outcome {
        /* The reach field was recomputed, so the movement ribbons are stale. */
        bool derivedStateChanged = false;

        /* The world's DATA changed: static mesh, probes and bake are all wrong. */
        bool worldGeometryChanged = false;

        /* Where to re-bake the static sun, and how far out. Only the area a
         * change could have affected — a full re-bake of an unchanged board is
         * seconds of work to produce an identical result. */
        std::optional<Cell> rebakeCentre;
        float               rebakeRadius = 0.0f;
    };

    PlayerController(GameState& state, DecalSet& decals);

    /* ---- per frame ----------------------------------------------------- */
    /* Parses this frame's device state into the held intent above. */
    void sampleCameraIntent(const FrameInput& input);
    void updatePointer(const FrameInput& input);
    void stepAnimation(float deltaSeconds);

    /* Recomputes reach and clears the path preview. Sets derivedStateChanged. */
    void rebuildDerivedState();

    void detonateAt(const Cell& cell);

    /* Consumes the accumulated outcome and resets it, so a flag is acted on
     * exactly once. */
    Outcome takeOutcome();

    /* ---- possession -----------------------------------------------------
     * The pawn this controller drives. Set once at startup. The controller
     * needs it back because the possessed pawn's camera is the DEFAULT view
     * camera — the same reason Unreal's PlayerController can reach its pawn. */
    void possess(CameraPawn& pawn) { pawn_ = &pawn; }

    /* ---- the view, READ but not decided ---------------------------------
     * WHAT THE SCREEN SHOWS IS NOT THIS CLASS'S CALL. A controller interprets
     * input; it may ask for a cut the way it asks for a move order, but the
     * choice of camera is presentation state and lives with the
     * CameraDirector (cromwell/camera/CameraDirector.hpp). It briefly lived
     * here, and that was wrong: a kill-cam or a scripted cut is not an input
     * concern, and a controller that owned the view would have made it one.
     *
     * The controller still has to AGREE with the view — a pick from a camera
     * the player is not looking through clicks on things they cannot see —
     * so it watches the director and builds its rays from current(). */
    void setDirector(const cromwell::CameraDirector& director) { director_ = &director; }

    /* The camera the screen is showing: the director's choice when one is
     * wired, the possessed pawn's camera otherwise. What every pick and every
     * camera-derived answer in this class reads. */
    cromwell::Camera& viewCamera() const;

    /* WHERE THE VIEW'S PICTURE LANDS ON SCREEN — the whole window normally,
     * pane 0's rectangle when a splitscreen layout is tiling it. Picks
     * project through this rectangle (Viewport.hpp: "the screen is not always
     * the window" — this is that day arriving), and a cursor OUTSIDE it is
     * pointing at some other pane's picture, which is not this player's
     * world: it hovers and clicks nothing. Application states it each frame,
     * because Application owns the layout; nullopt means the whole window. */
    void setViewArea(std::optional<Rectangle> areaPx) { viewArea_ = areaPx; }

    /* ---- continuous camera intent, POLLED BY THE PAWN -------------------
     * Held state, true for as long as the key is down. The controller never
     * moves a camera; it says what the player is asking for, and the pawn
     * decides what that means in world units. */
    Vector2 panInput()   const { return panInput_; }
    bool    panFast()    const { return panFast_; }
    bool    isOrbiting() const { return orbiting_; }
    Vector2 orbitDelta() const { return orbitDelta_; }

    /* Returns the accumulated wheel movement and clears it. Consume-once: a
     * notch must be applied exactly once, so reading is taking. */
    float consumeZoomDelta();

    /* ---- what the renderer and the HUD read ---------------------------- */
    std::optional<int>            hovered()    const { return hover_.target(); }
    const std::vector<PathPoint>& preview()    const { return preview_; }
    const MoveAnimator&           animator()   const { return animator_; }
    const RingSelector&           rings()      const { return rings_; }
    RingSelector&                 rings()            { return rings_; }
    bool                          grenadeArmed() const { return grenadeArmed_; }
    bool                          cursorOnSurface() const { return cursorSurface_.has_value(); }
    const std::optional<Decal>&   decalPreview() const { return decalPreview_; }
    const std::string&            status()     const { return status_; }

    void setStatus(std::string text) { status_ = std::move(text); }
    void setGrenadeArmed(bool armed) { grenadeArmed_ = armed; if (!armed) clearPreview(); }
    void toggleGrenade();

    /* The decal tool's live state, mirrored from the dev panel every frame.
     * `available` is the renderer's answer to "is the decal pass even up" —
     * the controller cannot ask a DecalRenderer it does not own. */
    void setDecalTool(bool armed, const DevRequests::DecalPlacement& brush, bool available);

    /* The current camera as a ready-to-paste `--cam px py pz tx ty tz`. */
    std::string cameraArguments() const;

    RibbonPassSettings ribbonSettings(bool softCutaway) const;

    /* HOW MUCH OF THE WORLD THE CAMERA SHOULD SHOW, this frame.
     *
     * The policy lives here and not in the renderer because both halves of it
     * are the controller's: the storey comes from the mode and the selection,
     * and the facings come from where the camera is pointing. The renderer is
     * handed the answer, which is what stops a pass from deciding for itself —
     * the fault that made the sun's shadows move with the iso level. */
    CutawayView cutawayView() const;

    /* Whether the selected unit could legally END a move on this cell.
     * Public because the overlays and the HUD both ask it. */
    bool canRestAt(int cellIndex) const;

private:
    void buildPreviewFor(std::optional<int> destination);
    void updateDecalPreview();
    void commitDecalPreview();
    void handleClick();
    void clearPreview() { preview_.clear(); route_.clear(); }

    GameState& state_;
    DecalSet&  decals_;

    CameraPawn* pawn_ = nullptr;

    /* Where "what is the screen showing" is answered. Borrowed, read-only —
     * see setDirector. */
    const cromwell::CameraDirector* director_ = nullptr;

    /* The view's on-screen rectangle, when it is not the whole window. */
    std::optional<Rectangle> viewArea_;

    /* The viewport every pick projects through: the view camera, at the view
     * area. Built per use — a Viewport is a value; see Viewport.hpp. */
    cromwell::Viewport viewViewport() const;

    /* Held camera intent, rebuilt from FrameInput every frame. */
    Vector2 panInput_{};
    Vector2 orbitDelta_{};
    float   pendingZoom_ = 0.0f;
    bool    panFast_  = false;
    bool    orbiting_ = false;

    RingSelector rings_;
    MoveAnimator animator_;

    /* WHICH CELL THE CURSOR IS OVER, held by the engine's tracker rather than
     * as a bare optional. It is the same value it always was — `hovered()` still
     * hands one back — but a seam flicker no longer reads as an exit and an
     * enter, and a tooltip that wants "held still for 400 ms" now has somewhere
     * to ask. See cromwell/input/HoverTracker.hpp. */
    HoverTracker<int>      hover_;

    std::vector<PathPoint> preview_;
    std::vector<int>       route_;

    /* Click versus drag for the world pointer. A click is what orders a move or
     * selects a unit; the drag half is unused here so far and is what a marquee
     * selection would read. */
    PointerDrag            click_;

    /* What the cursor is over, as geometry rather than as a tile — see
     * SurfacePicker for why that is a separate question from hover_. */
    std::optional<SurfaceHit> cursorSurface_;

    /* The decal tool's armed state and its live brush, mirrored from the panel
     * every frame (present-means-armed; see DevRequests::decalBrush). */
    bool                        decalArmed_ = false;
    bool                        decalAvailable_ = false;
    DevRequests::DecalPlacement decalBrush_;

    /* The ghost under the cursor. A REAL Decal, submitted by the real pass
     * after the committed ones so it reads on top — the only thing separating
     * it from a placement is that it lives here instead of in decals_, and is
     * rebuilt from scratch every frame. */
    std::optional<Decal> decalPreview_;

    bool        grenadeArmed_ = false;
    std::string status_;
    Outcome     outcome_;
};

}  // namespace game
