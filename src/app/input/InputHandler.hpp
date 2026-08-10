/* InputHandler.hpp — read the keyboard and mouse.
 *
 * SINGLE RESPONSIBILITY: sample the devices into a FrameInput. It maps keys to
 * named actions and nothing more; what an action DOES is Application's call.
 */
#pragma once

#include "app/input/FrameInput.hpp"

#include <optional>

namespace xcom {

class InputHandler {
public:
    /* `forcedMouse` pins the cursor for reproducible screenshots. */
    FrameInput sample(std::optional<int> forcedMouseX,
                      std::optional<int> forcedMouseY) const;
};

}  // namespace xcom
