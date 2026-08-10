#include "cromwell/entities/Entity.hpp"

namespace cromwell {

Entity::~Entity()
{
    /* Every component gets the chance to undo what it registered elsewhere —
     * an event subscription, a handle in a spatial index — before its memory
     * goes. Done here rather than in each component's own destructor because a
     * destructor cannot safely reach back into a half-destroyed owner, and
     * onDetach can. */
    for (auto& entry : components_) entry.second->onDetach();
}

}  // namespace cromwell
