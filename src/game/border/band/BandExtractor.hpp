/* BandExtractor.hpp — walk a band's boundary into closed loops.
 *
 * SINGLE RESPONSIBILITY: emit one boundary edge per tile face with no
 * CONNECTED in-band neighbour, and chain those edges into loops using
 * BandConnectivity's corner rule.
 *
 * Every directed boundary edge has exactly one successor, so each walk is a
 * cycle; the visited set also guards against a malformed band.
 */
#pragma once

#include "game/border/band/Band.hpp"
#include "game/border/band/BandConnectivity.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {


class BandExtractor {
public:
    explicit BandExtractor(const World& world)
        : world_(world), connectivity_(world) {}

    void extract(const Band& band, LoopSet& out);

private:
    const World&     world_;
    BandConnectivity connectivity_;

    /* instance scratch — the C original used a file-scope array */
    std::vector<unsigned char> visited_;
};

}  // namespace game
