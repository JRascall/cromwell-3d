/* InputHandler.hpp — read the devices, produce named actions.
 *
 * SINGLE RESPONSIBILITY: sample IInput, IClock and ISurface into a FrameInput.
 * It maps keys to named actions and nothing more; what an action DOES is
 * Application's call.
 *
 * ================== WHY IT TAKES INTERFACES NOW ===========================
 *
 * This file used to call IsKeyPressed and GetFrameTime directly — thirty-six
 * raylib calls, which was every keyboard binding the game has in one place. It
 * now takes the platform's services instead, which does three things:
 *
 *   - it is the single largest block of raylib in the engine's non-render half,
 *     and it is gone;
 *   - the bindings become testable, because a stub IInput can assert that F9
 *     starts a capture without a window existing;
 *   - a console port gets the bindings for free and only has to decide what a
 *     gamepad maps to, rather than reimplementing the mapping.
 *
 * WHAT IT DELIBERATELY DOES NOT DO YET: read a gamepad. The bindings here are
 * keyboard and mouse because that is what this game has; adding a pad means
 * adding cases below, not changing anything above. IInput already reports pads
 * (see IInput.hpp on why that was there before anything used it).
 */
#pragma once

#include "cromwell/input/FrameInput.hpp"

#include <optional>

namespace cromwell {

class IClock;
class IInput;
class ISurface;

class InputHandler {
public:
    /* `forcedMouse` pins the cursor for reproducible screenshots.
     *
     * IT OVERRIDES THE REPORTED POSITION RATHER THAN WARPING THE REAL CURSOR,
     * which is both a smaller side effect and the only version that works at
     * all on a platform with no cursor to warp. Everything downstream reads
     * this struct, so the two are equivalent where a cursor exists. */
    FrameInput sample(const IInput& input, const IClock& clock, ISurface& surface,
                      std::optional<int> forcedMouseX,
                      std::optional<int> forcedMouseY) const;
};

}  // namespace cromwell
