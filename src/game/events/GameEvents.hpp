/* GameEvents.hpp — every event name this GAME raises, declared once.
 *
 * SINGLE RESPONSIBILITY: be the one place a tactical-layer event name is
 * spelled. The engine's half lives in cromwell/events/Events.hpp and is
 * genre-agnostic; everything here names something only an XCOM-like has.
 *
 * The split is the whole point: cromwell cannot include this file, so no
 * engine system can ever come to depend on there being such a thing as a
 * soldier or a grenade. Same discipline as PO's CrierEvents.h.
 */
#pragma once

namespace game::events {

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

}  // namespace game::events
