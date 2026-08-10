#include "app/picking/UnitPicker.hpp"

#include "core/units/Soldier.hpp"
#include "core/units/UnitRoster.hpp"
#include "core/units/Vehicle.hpp"

namespace xcom {

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

void UnitPicker::visit(const Soldier&) { testBox(0.25f, 0.75f); }

void UnitPicker::visit(const Vehicle&) { testBox(0.1f, 1.9f); }

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
        unit->accept(*this);

        if (pendingHit_ && pendingDistance_ < bestDistance) {
            bestDistance = pendingDistance_;
            best = unit.get();
        }
    }
    return best;
}

}  // namespace xcom
