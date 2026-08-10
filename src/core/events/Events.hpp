/* Events.hpp — every event name, declared once.
 *
 * SINGLE RESPONSIBILITY: be the one place an event name is spelled. A raw
 * string at a call site is a typo waiting to become a silently dead hook, so
 * publishers and subscribers both name the constant.
 *
 * Same discipline as PO's Core/Events/CrierEvents.h: the core block stays
 * genre-agnostic (input, view, log), and the game block below it holds the
 * tactical-layer names. Each constant carries its payload contract in a
 * comment — the argument list is positional and the only contract there is.
 *
 * Names are added as systems need them, not up front. Everything here names a
 * seam that already exists in the app; a seam that has not been wired to the
 * bus yet is marked so.
 */
#pragma once

namespace xcom::events {

/* --- Pointer / interaction ------------------------------------------------
 * The hovered cell changed, or a cell was clicked. [CellIndex] — the lattice
 * index, matching Lattice::index(x, y, z). HoverCleared carries nothing. */
inline constexpr const char* kHoverChanged = "input.hover_changed";
inline constexpr const char* kHoverCleared = "input.hover_cleared";
inline constexpr const char* kTileClicked  = "input.tile_clicked";

/* A pure intent event: each listener decides what backing out means in its
 * own context (drop the path preview, disarm the grenade, close the panel).
 * The input layer never knows which one answers. No payload. */
inline constexpr const char* kCancelPressed = "input.cancel_pressed";

/* --- Selection ------------------------------------------------------------
 * [UnitIndex, Name] — the roster index and the unit's display name, so a
 * listener reacts without holding a GameState pointer. Deselected carries the
 * index of the unit that lost selection. */
inline constexpr const char* kUnitSelected   = "unit.selected";
inline constexpr const char* kUnitDeselected = "unit.deselected";

/* --- Movement -------------------------------------------------------------
 * PreviewBuilt [CellIndex, Cost] — a path to the hovered cell was costed.
 * Started/Finished [UnitIndex, FromCell, ToCell]; Step [UnitIndex, CellIndex]
 * as each waypoint is reached. */
inline constexpr const char* kMovePreviewBuilt = "move.preview_built";
inline constexpr const char* kMoveStarted      = "move.started";
inline constexpr const char* kMoveStep         = "move.step";
inline constexpr const char* kMoveFinished     = "move.finished";

/* Pipeline (not call): the move cost of one edge, folded through every hook
 * that modifies it. Argument 0 is the running cost, then [UnitIndex,
 * FromCell, ToCell]. Nothing folds into it yet — the seam is here so terrain
 * and status effects have somewhere to land that isn't Pathfinder. */
inline constexpr const char* kMoveCostPipeline = "move.cost";

/* --- World ----------------------------------------------------------------
 * Detonated [CellIndex, Radius] — a grenade went off. GeometryChanged carries
 * nothing and means the static mesh, the light bake and the reach field all
 * describe a world that no longer exists. Reset is a full rebuild. */
inline constexpr const char* kWorldDetonated       = "world.detonated";
inline constexpr const char* kWorldGeometryChanged = "world.geometry_changed";
inline constexpr const char* kWorldReset           = "world.reset";

/* --- View -----------------------------------------------------------------
 * IsoLevelChanged [Level], LayersChanged (no payload — read ViewLayers),
 * DebugViewChanged [Mode]. */
inline constexpr const char* kViewIsoLevelChanged  = "view.iso_level_changed";
inline constexpr const char* kViewLayersChanged    = "view.layers_changed";
inline constexpr const char* kViewDebugViewChanged = "view.debug_view_changed";

/* --- Log / status ---------------------------------------------------------
 * [Message]. StatusChanged is the HUD's one-line status; the log family is for
 * anything that wants recording rather than showing. */
inline constexpr const char* kStatusChanged = "ui.status_changed";
inline constexpr const char* kLogInfo       = "log.info";
inline constexpr const char* kLogWarning    = "log.warning";
inline constexpr const char* kLogError      = "log.error";

}  // namespace xcom::events
