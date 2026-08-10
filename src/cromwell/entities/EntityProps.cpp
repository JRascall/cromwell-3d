#include "cromwell/entities/EntityProps.hpp"

#include "cromwell/services/Services.hpp"
#include "cromwell/settings/SettingKeys.hpp"
#include "cromwell/settings/Settings.hpp"

namespace cromwell {

namespace {

/* 100ms - ten thinks a second, faster than a player can perceive a decision
 * being made and slow enough to be nearly free. A game raises it for cheaper
 * AI or lowers it for sharper reactions, once, at startup. */
constexpr float kFallbackThinkInterval = 0.1f;

}  // namespace

float EntityProps::defaultThinkInterval()
{
    /* Survives having no settings at all: a test, a tool, or a game that never
     * registers a bag gets the fallback and runs. */
    if (const Settings* config = Services::tryGet<Settings>(kGameSettings))
        return config->floatOr(settings::kThinkInterval, kFallbackThinkInterval);
    return kFallbackThinkInterval;
}

}  // namespace cromwell
