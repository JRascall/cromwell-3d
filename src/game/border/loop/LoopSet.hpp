/* LoopSet.hpp — the boundary loops of one band.
 *
 * SINGLE RESPONSIBILITY: store loops of directed edges and their metadata.
 * Edges are concatenated into one vector; each Loop names its slice.
 */
#pragma once

#include "game/border/EdgeId.hpp"

#include <vector>

namespace game {


struct Loop {
    int  first = 0;        /* index into LoopSet::edges() */
    int  count = 0;
    int  minZ  = 0;        /* lowest z cell in the loop   */
    bool closed = false;
};

class LoopSet {
public:
    void clear()
    {
        edges_.clear();
        loops_.clear();
    }

    const std::vector<EdgeId>& edges() const { return edges_; }
    const std::vector<Loop>&   loops() const { return loops_; }

    int loopCount() const { return static_cast<int>(loops_.size()); }
    int edgeCount() const { return static_cast<int>(edges_.size()); }

    const Loop& loop(int index) const { return loops_[static_cast<std::size_t>(index)]; }

    EdgeId edgeAt(const Loop& loop, int offset) const
    {
        return edges_[static_cast<std::size_t>(loop.first + offset)];
    }

    int closedLoopCount() const;

    /* ---- writing, used by BandExtractor ------------------------------- */
    void appendEdge(EdgeId edge) { edges_.push_back(edge); }
    void appendLoop(const Loop& loop) { loops_.push_back(loop); }

private:
    std::vector<EdgeId> edges_;
    std::vector<Loop>   loops_;
};

}  // namespace game
