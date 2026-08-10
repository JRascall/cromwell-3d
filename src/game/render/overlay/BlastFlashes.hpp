/* BlastFlashes.hpp — the expanding sphere a grenade leaves behind.
 *
 * SINGLE RESPONSIBILITY: one visual effect — its list, its ageing and its
 * drawing. Nothing else needs to know it exists.
 */
#pragma once

#include "raylib.h"

#include <vector>

namespace game {


class BlastFlashes {
public:
    static constexpr float kLifetime = 0.45f;
    static constexpr int   kMaxFlashes = 16;

    void add(float x, float height, float y);
    void clear() { flashes_.clear(); }

    void update(float deltaSeconds);
    void draw() const;

private:
    struct Flash {
        float x = 0.0f;
        float y = 0.0f;
        float height = 0.0f;
        float age = 0.0f;
    };

    std::vector<Flash> flashes_;
};

}  // namespace game
