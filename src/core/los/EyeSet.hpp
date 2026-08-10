/* EyeSet.hpp — where a unit looks from.
 *
 * SINGLE RESPONSIBILITY: produce the eye positions for a body — its centre
 * eye plus every step-out peek PeekFinder offers.
 */
#pragma once

#include "core/los/PeekFinder.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

#include <vector>

namespace xcom {

struct Eye {
    float x = 0.0f;
    float y = 0.0f;
    float height = 0.0f;
    bool  isPeek = false;
};

class EyeSet {
public:
    explicit EyeSet(const World& world)
        : world_(world), terrain_(world), peeks_(world) {}

    /* The centre eye comes FIRST, so a caller that stops at the first
     * successful cast naturally reports direct sight over peek sight. */
    std::vector<Eye> eyesFor(const Cell& cell) const;

private:
    const World& world_;
    Terrain      terrain_;
    PeekFinder   peeks_;
};

}  // namespace xcom
