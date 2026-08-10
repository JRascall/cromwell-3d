#include "game/picking/UnitPicker.hpp"

#include "game/units/roster/UnitRoster.hpp"

namespace game {


void UnitPicker::testBox(float minInset, float maxInset)
{
    BoundingBox box;
    box.min = Vector3{ static_cast<float>(pendingCell_.x) + minInset,
                       pendingBase_,
                       static_cast<float>(pendingCell_.y) + minInset };
    box.max = Vector3{ static_cast<float>(pendingCell_.x) + maxInset,
                       pendingBase_ + 0.95f,
                       static_cast<float>(pendingCell_.y) + maxInset };

    const RayCollision collision = GetRayCollisionBox(pendingRay_, box);
    pendingHit_      = collision.hit;
    pendingDistance_ = collision.distance;
}

Unit* UnitPicker::pick(UnitRoster& roster, const Ray& ray, int maxStorey)
{
    Unit* best = nullptr;
    float bestDistance = 1e30f;

    for (const std::unique_ptr<Unit>& unit : roster) {
        if (unit->isDead()) continue;
        if (Lattice::storeyOfZ(unit->position().z) > maxStorey) continue;

        pendingRay_  = ray;
        pendingCell_ = unit->position();
        pendingBase_ = unit->baseHeight(world_);
        pendingHit_  = false;
        testBox(unit->body().pickMinHeight(), unit->body().pickMaxHeight());

        if (pendingHit_ && pendingDistance_ < bestDistance) {
            bestDistance = pendingDistance_;
            best = unit.get();
        }
    }
    return best;
}

}  // namespace game
