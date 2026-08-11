/* TilePicker.hpp — what is under the mouse ray?
 *
 * SINGLE RESPONSIBILITY: march the ray through the lattice and report the
 * first surface it CROSSES from above.
 *
 * No pickable-mesh list to keep in sync: the tile data is the only thing
 * consulted, so what you click is by construction what movement and LOS agree
 * is there.
 */
#pragma once

#include "cromwell/math/Ray.hpp"

#include "raylib.h"

#include "game/query/BlockedMass.hpp"
#include "game/query/Terrain.hpp"
#include "game/world/World.hpp"

#include <optional>

namespace game {


class TilePicker {
public:
    explicit TilePicker(const World& world)
        : world_(world), terrain_(world), mass_(world) {}

    /* The first standable surface or solid mass under the ray, honouring the
     * floor isolation ceiling. */
    std::optional<int> pick(const cromwell::Ray& ray, int maxStorey) const;

private:
    static constexpr float kStep = 0.03f;
    static constexpr float kMaxDistance = 140.0f;

    const World& world_;
    Terrain      terrain_;
    BlockedMass  mass_;
};

}  // namespace game
