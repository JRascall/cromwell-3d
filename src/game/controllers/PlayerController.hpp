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
#include "cromwell/ribbon/RibbonRenderer.hpp"
#include "game/lattice/Cell.hpp"
#include "game/movement/search/PathPoint.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/picking/SurfacePicker.hpp"
#include "game/render/dev/DevView.hpp"
#include "game/state/GameState.hpp"
#include "game/state/RingSelector.hpp"

#include <optional>
#include <string>
#include <vector>

namespace game {

using namespace cromwell;

class CameraPawn;

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
     * needs it back for one thing only - the picking ray starts at the
     * possessed camera - which is the same reason Unreal's PlayerController
     * can reach its pawn. */
    void possess(CameraPawn& pawn) { pawn_ = &pawn; }

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
    std::optional<int>            hovered()    const { return hovered_; }
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

    /* Held camera intent, rebuilt from FrameInput every frame. */
    Vector2 panInput_{};
    Vector2 orbitDelta_{};
    float   pendingZoom_ = 0.0f;
    bool    panFast_  = false;
    bool    orbiting_ = false;

    RingSelector rings_;
    MoveAnimator animator_;

    std::optional<int>     hovered_;
    std::vector<PathPoint> preview_;
    std::vector<int>       route_;
    Vector2                pressedAt_{};

    /* What the cursor is over, as geometry rather than as a tile — see
     * SurfacePicker for why that is a separate question from hovered_. */
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
