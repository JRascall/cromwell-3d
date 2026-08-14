#include "game/controllers/PlayerController.hpp"

#include "raymath.h"

#include "cromwell/camera/CameraDirector.hpp"
#include "cromwell/camera/Viewport.hpp"
#include "cromwell/debug/DebugDraw.hpp"
#include "cromwell/math/RaylibInterop.hpp"

#include "game/entities/pawns/CameraPawn.hpp"

#include "game/lattice/Lattice.hpp"
#include "game/movement/search/PathReconstructor.hpp"
#include "game/path/PathPreviewBuilder.hpp"
#include "game/picking/TilePicker.hpp"
#include "game/picking/UnitPicker.hpp"
#include "game/rules/DestructionSystem.hpp"
#include "game/rules/HullCrusher.hpp"
#include "game/rules/RestPlacement.hpp"
#include "game/units/kinds/Unit.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace game {

PlayerController::PlayerController(GameState& state, DecalSet& decals)
    : state_(state), decals_(decals) {}

PlayerController::Outcome PlayerController::takeOutcome()
{
    const Outcome taken = outcome_;
    outcome_ = Outcome{};
    return taken;
}

void PlayerController::rebuildDerivedState()
{
    state_.recompute();
    clearPreview();

    /* The reach field changed, so the ribbons built from its border are stale.
     * Rebuilding them is the renderer's job. */
    outcome_.derivedStateChanged = true;
}

void PlayerController::toggleGrenade()
{
    grenadeArmed_ = !grenadeArmed_;
    if (grenadeArmed_) clearPreview();
    else               buildPreviewFor(hovered());
}

void PlayerController::setDecalTool(bool armed, const DevRequests::DecalPlacement& brush,
                                    bool available)
{
    decalArmed_     = armed;
    decalBrush_     = brush;
    decalAvailable_ = available;
}

cromwell::Camera& PlayerController::viewCamera() const
{
    /* The pawn fallback keeps a controller wired before the director — or a
     * test that never made one — picking through the camera it possesses. */
    return director_ != nullptr ? director_->current() : pawn_->camera();
}

Viewport PlayerController::viewViewport() const
{
    const Camera3D rig = viewCamera().toRaylib();
    if (viewArea_) {
        return Viewport{ rig, Vec2{ viewArea_->x, viewArea_->y },
                         Vec2{ viewArea_->width, viewArea_->height } };
    }
    return Viewport::ofWindow(rig);
}

std::string PlayerController::cameraArguments() const
{
    /* THE CAMERA ON SCREEN, which is viewCamera() and not the pawn's rig.
     *
     * This read pawn_->camera(), on the argument that --cam places the PLAYER's
     * camera and copying whatever the screen was switched to would paste a
     * security feed's viewpoint into a flag that positions the pawn. The
     * argument is about the exceptional case and got the ordinary one wrong:
     * the director reports the pawn's rig through current() in normal play
     * anyway, so this only ever differed when the two disagreed — which is
     * exactly when the copied line described a view nobody was looking at.
     *
     * What this button is FOR is turning "it looks wrong here" into a view
     * somebody else can render. That means the view on the screen, whichever
     * camera is producing it. Pasting a feed's pose into the pawn reproduces
     * the picture, which is the whole request.
     *
     * ================== AND THE LENS, NOT JUST THE POSE ======================
     *
     * A pose is where the camera is; it is not what the camera sees. This
     * emitted six numbers and no lens, so pasting it back restored the position
     * exactly and framed it through whatever field of view the pawn's rig
     * happened to carry. Every camera in this game that is not the pawn's has a
     * different one — the security feeds are 50 degrees, the plan view is
     * ORTHOGRAPHIC — so the reproduction was silently wrong for all of them.
     *
     * The symptom is the cruel part: a wrong field of view looks like the
     * camera standing too far forward or too far back. So the reader compares
     * the position, finds it correct to three decimals, and concludes the
     * export is fine.
     *
     * TWO SPELLINGS BECAUSE THERE ARE TWO KINDS OF LENS. Under perspective the
     * number is a vertical angle; under orthographic it is a visible height in
     * world units. Camera.hpp calls conflating them "the fovy trap" and closed
     * it by separating projection() from lens(); emitting one --fov for both
     * would reopen it at the command line. */
    const cromwell::Camera& camera = viewCamera();

    const Vec3 position = camera.position();
    const Vec3 target   = camera.target();

    char buffer[200];
    const int written = std::snprintf(buffer, sizeof(buffer),
                  "--cam %.3f %.3f %.3f %.3f %.3f %.3f",
                  static_cast<double>(position.x),
                  static_cast<double>(position.y),
                  static_cast<double>(position.z),
                  static_cast<double>(target.x),
                  static_cast<double>(target.y),
                  static_cast<double>(target.z));

    if (written > 0 && static_cast<std::size_t>(written) < sizeof(buffer)) {
        if (camera.isOrthographic())
            std::snprintf(buffer + written, sizeof(buffer) - static_cast<std::size_t>(written),
                          " --ortho %.3f", static_cast<double>(camera.lens()));
        else
            std::snprintf(buffer + written, sizeof(buffer) - static_cast<std::size_t>(written),
                          " --fov %.3f", static_cast<double>(camera.lens()));
    }
    return buffer;
}


void PlayerController::sampleCameraIntent(const FrameInput& input)
{
    orbiting_   = input.orbiting;
    orbitDelta_ = input.mouseDelta;

    /* x is right, y is forward - screen space, as the input layer reports it.
     * Turning that into metres is the pawn's problem. */
    panInput_ = Vector2{ input.panRight, input.panForward };
    panFast_  = input.panFast;

    /* Accumulated rather than assigned: several wheel events can land in one
     * frame, and the pawn may not poll on the frame they arrive. */
    pendingZoom_ += input.wheel;
}

float PlayerController::consumeZoomDelta()
{
    const float delta = pendingZoom_;
    pendingZoom_ = 0.0f;
    return delta;
}

bool PlayerController::canRestAt(int cellIndex) const
{
    const RestPlacement placement(state_.world(), state_.roster());
    return placement.canRest(state_.selectedUnit(), state_.moveGraph(),
                             state_.blockedMask(),
                             state_.world().lattice().cellAt(cellIndex));
}

void PlayerController::buildPreviewFor(std::optional<int> destination)
{
    preview_.clear();
    route_.clear();
    if (!destination) return;

    const Unit& unit = state_.selectedUnit();
    const int start = state_.world().lattice().index(unit.position());
    if (*destination == start) return;
    if (state_.reach().cost(*destination) > state_.sprintBudget()) return;
    if (!canRestAt(*destination)) return;

    route_ = PathReconstructor::reconstruct(state_.reach(), *destination, start);
    if (route_.size() < 2) return;

    PathPreviewBuilder builder(state_.world());
    builder.build(unit, state_.reach(), route_, preview_);
}

void PlayerController::updatePointer(const FrameInput& input)
{
    /* THROUGH Viewport RATHER THAN raylib's GetScreenToWorldRay, which reads the
     * window size out of globals and so cannot be told about a viewport that is
     * not the whole window. Nothing here needs that yet; using it anyway means
     * the day something does — a tactical inset, a split screen — is a
     * constructor argument rather than a hunt for every place a ray is built.
     * See cromwell/camera/Viewport.hpp. */
    /* viewViewport, NOT the window: picks come from the camera the screen is
     * SHOWING, at the rectangle its picture actually occupies — the whole
     * window normally, pane 0 under a split. A cursor outside that rectangle
     * is over some other pane's picture, and picking the world through it
     * would select things this player is not pointing at; everything below
     * treats it as the miss it is. */
    const Viewport viewport = viewViewport();
    const Vec2 cursor = input.mousePosition;
    const bool inView = viewport.contains(cursor);

    const cromwell::Ray ray = viewport.rayThrough(cursor);
    const TilePicker picker(state_.world());
    const std::optional<int> hit =
        inView ? picker.pick(ray, state_.isoLevel()) : std::nullopt;

    /* THE SURFACE, as opposed to the tile — kept alongside rather than derived
     * from `hover_`, because they are genuinely different answers. TilePicker
     * reports the standable tile a soldier could walk to and ignores walls
     * entirely; this reports the geometry under the cursor, wall faces
     * included, as a point and a normal. Only the decal tool reads it, and only
     * while the dev panel is up, but it is refreshed every frame regardless:
     * a picker that runs only when a button is pressed cannot grey that button
     * out beforehand, which is the whole difference between "pointing at
     * nothing" and "the tool is broken". */
    cursorSurface_ = inView ? SurfacePicker(state_.world()).pick(ray, state_.isoLevel())
                            : std::nullopt;

    /* WHAT THE DECAL TOOL IS ABOUT TO DO, drawn. A decal is a projector, so its
     * ORIENTATION is half of the placement and the ghost quad alone cannot show
     * which way it faces — a mark on a wall and a mark on the floor beneath it
     * look identical from above until one of them is wrong.
     *
     * Only while the tool is armed, so it costs nothing in play, and it doubles
     * as the standing proof that the trace layer and the debug renderer are
     * both alive. See cromwell/debug/DebugDraw.hpp. */
    if (decalArmed_ && cursorSurface_) {
        debugNormal(fromRaylib(cursorSurface_->point), fromRaylib(cursorSurface_->normal),
                    0.4f, debugColour::cyan());
    }

    /* After the pick, so the ghost is on the surface the cursor is over THIS
     * frame — a preview one frame behind the mouse reads as lag on the tool. */
    updateDecalPreview();

    /* THE TRACKER, RATHER THAN `hit != hovered_`. A ray marched against tile
     * geometry misses for a frame when the cursor sits on a seam between two
     * cells, and the naive compare turns that into an exit and an enter — which
     * clears the path preview and rebuilds it, sixty times a second, while the
     * player holds still. The tracker holds the cell across a gap that short.
     * See cromwell/input/HoverTracker.hpp. */
    const HoverChange change = hover_.update(hit, input.deltaSeconds);
    if (change.changed) {
        if (grenadeArmed_) { preview_.clear(); route_.clear(); }
        else buildPreviewFor(hover_.target());
    }

    /* A CLICK IS A FAILED DRAG, and the six pixels of slop that separate them
     * used to be a literal and a Vector2Distance here. It is the same rule the
     * marquee selection will need, so it moved to the engine rather than being
     * written twice with two different thresholds. */
    const DragResult gesture = click_.update(cursor, input.leftPressed,
                                             input.leftDown, input.leftReleased);
    if (gesture.clicked) handleClick(cursor);
}

/* ---- the dev decal tool --------------------------------------------------
 * ARM, PREVIEW, COMMIT. The panel supplies the brush and owns the armed flag;
 * this supplies the world. Nothing in the panel knows what a camera or a mouse
 * ray is, which is the same split every other DevRequest uses.
 *
 * THE PREVIEW IS A REAL DECAL, drawn by the real pass with the real settings —
 * not an outline, not a wireframe box, not a flat quad. That is the entire
 * point: what is hard to predict about a projected decal is exactly the part a
 * cheaper preview would not show you. Whether it wraps cleanly over the kerb
 * it is straddling, whether the angle fade has eaten the half of it that fell
 * on the wall behind, whether it is stretching where the surface turns away —
 * a box outline answers none of those, and they are the only questions worth
 * asking before committing. So the ghost goes through the same projector, the
 * same DBuffer and the same blend as the thing it is predicting, and the only
 * difference between it and a placement is which list it lives in. */
void PlayerController::updateDecalPreview()
{
    decalPreview_.reset();

    if (!decalArmed_ || !cursorSurface_ || !decalAvailable_) return;
    if (decalBrush_.material < 0 ||
        decalBrush_.material >= static_cast<int>(decals_.materialCount()))
        return;

    /* HOW DEEP THE BOX GOES, and it means two different things depending on the
     * wrap setting — which is why it is chosen here rather than fixed.
     *
     * WRAPPING: depth IS the wrap budget. The unwrap carries the texture by the
     * distance travelled from the placement plane, and the box caps that
     * distance, so a mark meant to climb the wall it is thrown against needs a
     * box tall enough to reach up it. Scaled with the decal's own size, because
     * a bigger mark should run further before it runs out of paper.
     *
     * NOT WRAPPING: depth is only reach, and the two surfaces want different
     * amounts. On the ground a decal should still reach over the kerb or crate
     * standing on it. On a wall there is nothing to reach: the wall is 0.09
     * thick and the only thing a deeper box could find is the room behind or the
     * street in front, neither of which it has any business inking. */
    const float depth = decalBrush_.wrap
                      ? std::max(1.0f, decalBrush_.size)
                      : (cursorSurface_->vertical
                             ? 0.6f
                             : std::max(1.0f, decalBrush_.size * 0.75f));

    Decal decal = Decal::onSurface(cursorSurface_->point, cursorSurface_->normal,
                                   decalBrush_.rotation * DEG2RAD,
                                   Vector2{ decalBrush_.size, decalBrush_.size }, depth);

    decal.material  = static_cast<DecalMaterialId>(decalBrush_.material);
    decal.opacity   = decalBrush_.opacity;
    decal.roughness = decalBrush_.roughness;
    decal.emissive  = decalBrush_.emissive;
    decal.wrap      = decalBrush_.wrap;

    decalPreview_ = decal;
}

void PlayerController::commitDecalPreview()
{
    if (!decalPreview_) return;

    Decal decal = *decalPreview_;

    /* Newest on top, which is what placing another one is expected to mean.
     * Taken from the count rather than a counter of its own so it survives a
     * "clear all" without the order restarting halfway up the stack. */
    decal.sortOrder = static_cast<int>(decals_.count());
    decals_.add(decal);

    /* STAYS ARMED. Placing a second mark next to the first is the common case —
     * comparing two sizes, or laying a row along a wall — and disarming after
     * every commit would mean a trip back to the panel between each one. The
     * button is the way out. */
}

void PlayerController::handleClick(Vec2 cursor)
{
    /* BEFORE THE GRENADE AND BEFORE SELECTION. While a placement tool is armed
     * the click belongs to it and to nothing else — a click that both stuck a
     * decal to a wall and ordered the squad across the map would be the worst of
     * both. Same reason the grenade check sits ahead of selection. */
    if (decalArmed_) {
        commitDecalPreview();
        return;
    }

    if (grenadeArmed_) {
        if (hovered()) detonateAt(state_.world().lattice().cellAt(*hovered()));
        return;
    }

    /* Same viewport, same rule as the hover: a click outside the view's own
     * rectangle landed on another pane's picture and orders nothing. */
    const Viewport viewport = viewViewport();
    const Vec2& click = cursor;
    if (!viewport.contains(click)) return;

    const cromwell::Ray ray = viewport.rayThrough(click);
    UnitPicker unitPicker(state_.world());
    Unit* picked = unitPicker.pick(state_.roster(), ray, state_.isoLevel());

    if (picked && picked->team() == Team::Player && !picked->isDead()) {
        state_.selectUnit(picked);
        rebuildDerivedState();
        status_ = state_.selectedUnit().selectionDescription();
        return;
    }

    if (hovered() && preview_.size() >= 2) animator_.start(*hovered());
}

/* ------------------------------------------------------------ animation */
void PlayerController::stepAnimation(float deltaSeconds)
{
    animator_.advance(deltaSeconds, preview_);
    if (!animator_.isFinished(preview_)) return;

    Unit& unit = state_.selectedUnit();

    int crushed = 0;
    if (unit.crushesHalfCover())
        crushed = HullCrusher(state_.world()).crushAlong(route_);

    unit.setPosition(state_.world().lattice().cellAt(animator_.destinationCell()));
    animator_.stop();
    if (crushed) outcome_.worldGeometryChanged = true;

    char buffer[192];
    if (crushed) {
        std::snprintf(buffer, sizeof(buffer),
                      "%s moved to (%d,%d) storey %d  - crushed %d half-cover edge(s)",
                      unit.displayName().c_str(), unit.position().x, unit.position().y,
                      Lattice::storeyOfZ(unit.position().z) + 1, crushed);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s moved to (%d,%d) storey %d",
                      unit.displayName().c_str(), unit.position().x, unit.position().y,
                      Lattice::storeyOfZ(unit.position().z) + 1);
    }
    status_ = buffer;

    rebuildDerivedState();
}

/* ---- grenade: edit the DATA first, then everything re-derives ---------- */
void PlayerController::detonateAt(const Cell& cell)
{
    DestructionSystem destruction(state_.world(), state_.roster());
    const BlastReport report = destruction.detonate(cell);

    /* selection falls back to the soldier if the tank just died */
    if (state_.selectedUnit().isDead()) state_.selectIndex(0);

    /* The blast FLASH is a render effect and is not ours to add. It falls out
     * of outcome_.rebakeCentre below, which is the same cell. */

    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "grenade (%d,%d) storey %d - %d data edits, %d unit(s) killed%s",
                  cell.x, cell.y, Lattice::storeyOfZ(cell.z) + 1,
                  report.dataEdits, report.unitsKilled,
                  report.dataEdits ? "" : "  [nothing destructible in radius]");
    status_ = buffer;

    grenadeArmed_ = false;
    /* The renderer is not ours to touch. Record what the blast invalidated
     * and let Application hand it over. */
    outcome_.worldGeometryChanged = true;
    outcome_.rebakeCentre = cell;
    outcome_.rebakeRadius = DestructionSystem::kBlastRadius;
    rebuildDerivedState();
}

/* --------------------------------------------------------------- drawing */
RibbonPassSettings PlayerController::ribbonSettings(bool softCutaway) const
{
    RibbonPassSettings settings;
    settings.camera       = viewCamera().toRaylib();
    settings.visibleRings = rings_.visibleRings(state_.reach(), hovered(), state_.moveBudget());
    settings.solidRing    = rings_.solidRing(state_.reach(), hovered(), state_.moveBudget());
    settings.hideHeight   = softCutaway
        ? Lattice::storeyBaseHeight(state_.isoLevel()) + kStoreyHeight
        : kHideHeightOff;
    settings.maxStorey    = state_.isoLevel();
    return settings;
}

CutawayView PlayerController::cutawayView() const
{
    CutawayView cutaway;
    cutaway.maxStorey = state_.isoLevel();

    /* THE FACING CUT IS OFF WHILE INSPECTING. Manual mode is for looking at a
     * specific storey — usually to check what is on it — and walls that come
     * and go as the camera turns are exactly the wrong behaviour for that.
     * Dynamic is the playing mode, where seeing into the building beats seeing
     * the building. */
    if (state_.cutawayMode() == CutawayMode::Dynamic) {
        /* The cutaway is a CAMERA affordance — walls come off for the camera
         * actually looking, so it follows the view target like the picks do. */
        const Vec3 look = viewCamera().target() - viewCamera().position();
        cutaway.facings = facingsVisibleFrom(look.x, look.z);
    }
    return cutaway;
}


}  // namespace game
