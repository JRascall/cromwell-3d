#include "game/entities/pawns/CameraPawn.hpp"

#include "game/controllers/PlayerController.hpp"
#include "game/lattice/Lattice.hpp"
#include "game/world/World.hpp"

namespace game {

void CameraPawn::tick(float deltaSeconds, PlayerController& controller, const World& world)
{
    /* Orbit first: it changes where "forward" points, and a pan applied against
     * last frame's heading reads as the rig sliding sideways out from under the
     * drag. */
    if (controller.isOrbiting()) rig_.orbit(controller.orbitDelta());

    const Vector2 pan = controller.panInput();
    rig_.pan(pan.y, pan.x, deltaSeconds, controller.panFast(),
             world.lattice().width(), world.lattice().height());

    /* Consumed, not read — see the header. */
    rig_.zoom(controller.consumeZoomDelta());
}

}  // namespace game
