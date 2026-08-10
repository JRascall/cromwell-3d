/* MoveGraph.hpp — the abstract move graph.
 *
 * SINGLE RESPONSIBILITY: define how a mover's legal steps are enumerated.
 *
 * Pathfinder searches this interface and nothing else, which is what lets one
 * Dijkstra serve both infantry and vehicles — the old code had to pass a
 * `bool tank` down into the search and branch inside it.
 */
#pragma once

#include "core/movement/BlockedMask.hpp"
#include "core/movement/Move.hpp"

#include <vector>

namespace xcom {

class MoveGraph {
public:
    virtual ~MoveGraph() = default;

    /* Appends this cell's legal steps into `out`, which is cleared first.
     * `blocked` may be nullptr for pure terrain. */
    virtual void neighbors(const Cell& from,
                           const BlockedMask* blocked,
                           std::vector<Move>& out) const = 0;

    /* Whether `cell` is a legal place for this mover to exist at all. */
    virtual bool isTraversable(const Cell& cell, const BlockedMask* blocked) const = 0;
};

}  // namespace xcom
