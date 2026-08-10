/* UnitPicker.hpp — which body did the ray hit?
 *
 * SINGLE RESPONSIBILITY: ray-test unit bounding boxes. Tested before terrain
 * so clicking a soldier selects rather than walks.
 */
#pragma once

#include "raylib.h"

#include "core/units/UnitVisitor.hpp"
#include "core/world/World.hpp"

namespace xcom {

class Unit;
class UnitRoster;

class UnitPicker : public UnitVisitor {
public:
    explicit UnitPicker(const World& world) : world_(world) {}

    /* nullptr when the ray misses every living unit at or below `maxStorey`. */
    Unit* pick(UnitRoster& roster, const Ray& ray, int maxStorey);

    void visit(const Soldier& soldier) override;
    void visit(const Vehicle& vehicle) override;

private:
    void testBox(float minInset, float maxInset);

    const World& world_;

    /* set before each accept() */
    Ray   pendingRay_{};
    Cell  pendingCell_{};
    float pendingBase_ = 0.0f;
    float pendingDistance_ = 0.0f;
    bool  pendingHit_ = false;
};

}  // namespace xcom
