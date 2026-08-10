/* RampValidator.hpp — is this rise a legal staircase?
 *
 * SINGLE RESPONSIBILITY: judge a ramp's slope against XCOM's band and explain
 * a rejection. It writes nothing and touches no world — MapAuthor decides what
 * to do with the verdict, and the caller decides where the text goes.
 *
 * A staircase authored out of band is a map bug, and silently clamping it
 * would move the surface out from under the border ribbon and the LOS mass
 * built to match it. So rejection is loud and the tile is left alone.
 */
#pragma once

#include <string>

namespace game {


class RampValidator {
public:
    struct Result {
        bool        valid = false;
        std::string diagnostic;   /* empty when valid */

        explicit operator bool() const { return valid; }
    };

    /* `x`, `y` appear only in the diagnostic text. */
    static Result validate(int x, int y, float rise);

    /* The slope in degrees a rise across one tile represents. */
    static float slopeDegrees(float rise);
};

}  // namespace game
