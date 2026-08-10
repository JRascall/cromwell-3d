/* UnitPicker.hpp — which body did the ray hit?
 *
 * SINGLE RESPONSIBILITY: ray-test unit bounding boxes. Tested before terrain
 * so clicking a soldier selects rather than walks.
 */
#pragma once

#include "raylib.h"

#include "game/world/World.hpp"

namespace game {


class Unit;
class UnitRoster;

class UnitPicker {
public:
    explicit UnitPicker(const World& world) : world_(world) {}

    /* nullptr when the ray misses every living unit at or below `maxStorey`. */
    Unit* pick(UnitRoster& roster, const Ray& ray, int maxStorey);

private:
    /* The box the ray is tested against, as a fraction of a cell. Both numbers
     * come from the unit's BodyComponent - they were two visitor overrides
     * that each called this with a pair of literals. */
    void testBox(float minInset, float maxInset);

    const World& world_;

    /* set before each test */
    Ray   pendingRay_{};
    Cell  pendingCell_{};
    float pendingBase_ = 0.0f;
    float pendingDistance_ = 0.0f;
    bool  pendingHit_ = false;
};

}  // namespace game
