#include "game/movement/search/Pathfinder.hpp"

namespace game {


void Pathfinder::search(const MoveGraph& graph,
                        const Cell& start,
                        float maxCost,
                        const BlockedMask* blocked,
                        ReachField& out) const
{
    out.reset(lattice_.cellCount());
    if (!lattice_.isValid(start)) return;

    /* Stale entries mean the queue can hold more than one node per cell; the
     * fan-out bounds it well below any real limit, and std::priority_queue
     * grows rather than dropping nodes the way the old fixed heap did. */
    std::priority_queue<Entry> frontier;

    const int startIndex = lattice_.index(start);
    out.setStart(startIndex);
    frontier.push({ 0.0f, startIndex });

    while (!frontier.empty()) {
        const Entry current = frontier.top();
        frontier.pop();

        if (current.cost > out.cost(current.cell) + 1e-6f) continue;   /* stale */

        graph.neighbors(lattice_.cellAt(current.cell), blocked, neighbours_);

        for (const Move& move : neighbours_) {
            const int next = lattice_.index(move.target);
            if (blocked && blocked->isBlocked(next)) continue;

            const float nextCost = current.cost + move.cost;
            if (nextCost > maxCost + 1e-6f) continue;
            if (nextCost < out.cost(next) - 1e-6f) {
                out.setArrival(next, nextCost, current.cell, move.kind);
                frontier.push({ nextCost, next });
            }
        }
    }
}

}  // namespace game
