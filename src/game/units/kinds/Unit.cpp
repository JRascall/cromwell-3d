#include "game/units/kinds/Unit.hpp"

#include "game/movement/graph/MoveGraph.hpp"
#include "game/units/roster/UnitRoster.hpp"

namespace game {

void Unit::setPosition(const Cell& cell)
{
    const Cell from = position_;

    position_ = cell;
    setLocation(Vec3{ static_cast<float>(cell.x) + 0.5f,
                      location().y,
                      static_cast<float>(cell.y) + 0.5f });

    if (roster_) roster_->onBodyMoved(*this, from);
}

void Unit::kill()
{
    if (dead_) return;
    dead_ = true;
    if (roster_) roster_->onBodyKilled(*this);
}

void Unit::onComponentsChanged()
{
    /* Re-resolves ALL of them rather than just the one that changed. Five hash
     * lookups on a cold path — a unit is built once — in exchange for a rebind
     * that cannot go stale or half-apply. */
    body_         = findComponent<BodyComponent>();
    mobility_     = findComponent<MobilityComponent>();
    cover_        = findComponent<CoverComponent>();
    destructible_ = findComponent<DestructibleComponent>();
    presentation_ = findComponent<PresentationComponent>();
}

std::unique_ptr<MoveGraph> Unit::createMoveGraph(const World& world) const
{
    return mobility().createGraph(world);
}


bool Unit::occupies(const Cell& cell) const
{
    if (cell.z != position_.z) return false;
    for (const Offset& o : footprint().offsets())
        if (position_.x + o.dx == cell.x && position_.y + o.dy == cell.y) return true;
    return false;
}

}  // namespace game
