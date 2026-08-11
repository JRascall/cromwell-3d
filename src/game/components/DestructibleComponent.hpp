/* DestructibleComponent.hpp — what an entity leaves behind when it is destroyed.
 *
 * SINGLE RESPONSIBILITY: one question, deliberately. It is its own component
 * rather than a field on the body because the next thing to carry it will not
 * be a unit at all — a destructible prop or a wreck has no footprint rules, no
 * mobility and no cover behaviour, but it does leave debris.
 */
#pragma once

#include "cromwell/entities/Component.hpp"

namespace game {

using namespace cromwell;

class DestructibleComponent : public Component {
public:
    /* Does its destruction stamp a wreck into the terrain? A wreck becomes half
     * cover, so cover, LOS and pathing all pick it up with no special case. */
    bool leavesWreckage() const { return leavesWreckage_; }
    DestructibleComponent& withLeavesWreckage(bool leaves)
    {
        leavesWreckage_ = leaves;
        return *this;
    }

private:
    bool leavesWreckage_ = false;
};

}  // namespace game
