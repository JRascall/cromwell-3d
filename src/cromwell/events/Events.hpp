/* Events.hpp — every ENGINE event name, declared once.
 *
 * SINGLE RESPONSIBILITY: be the one place a cromwell event name is spelled. A
 * raw string at a call site is a typo waiting to become a silently dead hook,
 * so publishers and subscribers both name the constant.
 *
 * Same discipline as PO's Core/Events/CrierEvents.h, and the same split: this
 * block stays GENRE-AGNOSTIC — input, view, UI, log. Anything that names a
 * soldier, a grenade or a tile belongs in game/events/GameEvents.hpp, which is
 * the game's file and which cromwell must never include.
 *
 * Names are added as systems need them, not up front. Each constant carries
 * its payload contract in a comment — the argument list is positional and the
 * only contract there is.
 */
#pragma once

namespace cromwell::events {

/* --- Pointer / interaction ------------------------------------------------
 * The hovered cell changed, or a cell was clicked. [CellIndex] — an index into
 * whatever spatial structure the game uses. HoverCleared carries nothing. */
inline constexpr const char* kHoverChanged = "input.hover_changed";
inline constexpr const char* kHoverCleared = "input.hover_cleared";
inline constexpr const char* kTileClicked  = "input.tile_clicked";

/* A pure intent event: each listener decides what backing out means in its
 * own context (drop the path preview, disarm the grenade, close the panel).
 * The input layer never knows which one answers. No payload. */
inline constexpr const char* kCancelPressed = "input.cancel_pressed";

/* --- View -----------------------------------------------------------------
 * IsoLevelChanged [Level], LayersChanged (no payload — read ViewSettings),
 * DebugViewChanged [Mode]. */
inline constexpr const char* kViewIsoLevelChanged  = "view.iso_level_changed";
inline constexpr const char* kViewLayersChanged    = "view.layers_changed";
inline constexpr const char* kViewDebugViewChanged = "view.debug_view_changed";

/* --- UI -------------------------------------------------------------------
 * StateChanged [Tag] — the UI state machine moved, where Tag is the lowercase
 * name ("mainmenu", "ingame"). Carried as a STRING rather than the enum so a
 * screen can bind to it without including the machine's header, exactly as
 * PO's UIStateMachine pushes it for UMG.
 *
 * Ready carries nothing and is raised BY a UI layer that has just come up. The
 * state machine answers it by re-publishing the current state — a screen that
 * loads late would otherwise sit on a state change it was not alive to hear. */
inline constexpr const char* kUIStateChanged = "ui.state_changed";
inline constexpr const char* kUIReady        = "ui.ready";

/* --- Log / status ---------------------------------------------------------
 * [Message]. StatusChanged is the HUD's one-line status; the log family is for
 * anything that wants recording rather than showing. */
inline constexpr const char* kStatusChanged = "ui.status_changed";
inline constexpr const char* kLogInfo       = "log.info";
inline constexpr const char* kLogWarning    = "log.warning";
inline constexpr const char* kLogError      = "log.error";

}  // namespace cromwell::events
