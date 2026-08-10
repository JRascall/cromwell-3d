/* SettingKeys.hpp — the settings cromwell itself reads, and where it lives.
 *
 * SINGLE RESPONSIBILITY: name the engine's own keys, and the service key the
 * engine looks its settings up under.
 *
 * SHORT LIST ON PURPOSE. Everything here is something the ENGINE consults, so
 * every addition is the engine learning about one more thing a game might want
 * to configure. A game's own settings share the same bag but are named in the
 * game's own header - cromwell must not learn what a grenade is by way of a
 * settings key.
 */
#pragma once

#include "cromwell/services/ServiceKey.hpp"

namespace cromwell {

/* Where the engine looks. A game registers its bag under this key at startup;
 * if it never does, every read falls back to its default and the engine runs
 * on its own numbers. */
inline constexpr ServiceKey kGameSettings{ "cromwell.settings.game" };

/* The player's own overrides, kept apart so that "reset to defaults" is a
 * matter of dropping one bag rather than remembering which values were
 * touched. Nothing in the engine reads it yet; the key is here so that the two
 * are named in one place. */
inline constexpr ServiceKey kUserSettings{ "cromwell.settings.user" };

namespace settings {

/* Seconds between think() calls, for every entity and component that does not
 * set its own. See Component.hpp. */
inline constexpr const char* kThinkInterval = "entity.think_interval";

}  // namespace settings

}  // namespace cromwell
