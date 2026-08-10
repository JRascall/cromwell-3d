#include "core/world/RampValidator.hpp"

#include "core/lattice/Constants.hpp"

#include <cmath>
#include <cstdio>

namespace xcom {

float RampValidator::slopeDegrees(float rise)
{
    return std::atan2(rise, kTileSize) * 57.2957795f;
}

RampValidator::Result RampValidator::validate(int x, int y, float rise)
{
    /* snprintf rather than iostreams so the diagnostics keep the exact
     * %.3f / %.1f shapes the C build produced. */
    char buffer[256];
    const float deg = slopeDegrees(rise);

    if (!(rise > 0.0f)) {
        std::snprintf(buffer, sizeof(buffer),
                      "MapAuthor::setRamp(%d,%d): rise must be positive (got %g)\n",
                      x, y, static_cast<double>(rise));
        return { false, buffer };
    }
    if (rise > kRampMaxRise + 1e-6f) {
        std::snprintf(buffer, sizeof(buffer),
                      "MapAuthor::setRamp(%d,%d): rise %.3f over a %.0f tile = %.1f deg, steeper "
                      "than the 45 deg ramp limit. Split the flight - max rise per tile is %.3f.\n",
                      x, y, static_cast<double>(rise), static_cast<double>(kTileSize),
                      static_cast<double>(deg), static_cast<double>(kRampMaxRise));
        return { false, buffer };
    }
    if (rise < kRampMinRise - 1e-6f) {
        std::snprintf(buffer, sizeof(buffer),
                      "MapAuthor::setRamp(%d,%d): rise %.3f = %.1f deg, shallower than 10 deg. "
                      "That is micro-relief - use floorOffset, not a ramp.\n",
                      x, y, static_cast<double>(rise), static_cast<double>(deg));
        return { false, buffer };
    }
    return { true, {} };
}

}  // namespace xcom
