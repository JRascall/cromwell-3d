/* Pathfinder.hpp — uniform-cost search over any MoveGraph.
 *
 * SINGLE RESPONSIBILITY: run Dijkstra. It has no opinion about what a legal
 * step is — that is the graph's job — which is why infantry and vehicles now
 * share one search instead of passing a `bool tank` down into it.
 *
 * Expansion is capped at maxCost so a search never floods the whole map.
 */
#pragma once

#include "core/lattice/Lattice.hpp"
#include "core/movement/BlockedMask.hpp"
#include "core/movement/MoveGraph.hpp"
#include "core/movement/ReachField.hpp"

#include <queue>
#include <vector>

namespace xcom {

class Pathfinder {
public:
    explicit Pathfinder(const Lattice& lattice) : lattice_(lattice) {}

    /* `blocked` may be nullptr for pure terrain. */
    void search(const MoveGraph& graph,
                const Cell& start,
                float maxCost,
                const BlockedMask* blocked,
                ReachField& out) const;

private:
    struct Entry {
        float cost;
        int   cell;
        /* std::priority_queue is a MAX-heap, so the comparison is inverted. */
        bool operator<(const Entry& other) const { return cost > other.cost; }
    };

    Lattice lattice_;

    mutable std::vector<Move> neighbours_;
};

}  // namespace xcom
