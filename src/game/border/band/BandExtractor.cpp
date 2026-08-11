#include "game/border/band/BandExtractor.hpp"

#include <algorithm>
#include <cmath>

namespace game {


/* Append cycle_[first .. first+count) to `out` as one loop. `closed` is the
 * caller's to decide: a whole cycle earns it by geometry, a run left over
 * between two suppressed edges never can. */
void BandExtractor::emitLoop(int first, int count, bool closed, LoopSet& out) const
{
    if (count <= 0) return;

    const Lattice& lattice = world_.lattice();

    Loop loop;
    loop.first  = out.edgeCount();
    loop.count  = count;
    loop.closed = closed;
    loop.minZ   = lattice.depth();

    for (int i = 0; i < count; i++) {
        const EdgeId edge = cycle_[static_cast<std::size_t>(first + i)];
        out.appendEdge(edge);
        loop.minZ = std::min(loop.minZ, lattice.cellAt(edge.cell()).z);
    }

    out.appendLoop(loop);
}

void BandExtractor::emit(const Band* suppress, LoopSet& out)
{
    const Lattice& lattice = world_.lattice();
    const int count = static_cast<int>(cycle_.size());
    if (count <= 0) return;

    const auto masked = [&](int i) {
        return suppress != nullptr &&
               suppress->contains(cycle_[static_cast<std::size_t>(i)].cell());
    };

    int firstMasked = -1;
    for (int i = 0; i < count && firstMasked < 0; i++)
        if (masked(i)) firstMasked = i;

    if (firstMasked < 0) {
        /* Nothing shared — the ordinary case, and the only one the unmasked
         * overload ever takes. Closed when the last edge's far corner is the
         * first's near one. */
        const EdgeId firstEdge = cycle_.front();
        const EdgeId lastEdge  = cycle_.back();

        const Cell firstCell = lattice.cellAt(firstEdge.cell());
        const Cell lastCell  = lattice.cellAt(lastEdge.cell());

        const EdgeSegment firstSegment = edgeCorners(firstCell.x, firstCell.y, firstEdge.dir());
        const EdgeSegment lastSegment  = edgeCorners(lastCell.x,  lastCell.y,  lastEdge.dir());

        const bool closed = std::fabs(lastSegment.toX - firstSegment.fromX) < 1e-5f &&
                            std::fabs(lastSegment.toY - firstSegment.fromY) < 1e-5f;

        emitLoop(0, count, closed, out);
        return;
    }

    /* CUT THE CIRCLE AT A SUPPRESSED EDGE FIRST. A cycle has no beginning, so
     * a run of survivors can straddle the point the walk happened to start at
     * and would be emitted as two strips with a seam in the middle of a
     * straight line. Rotating a dropped edge to the front makes every run
     * contiguous, and index 0 is then known-suppressed so the scan can skip
     * it. cycle_ is scratch; nothing downstream reads it again. */
    std::rotate(cycle_.begin(), cycle_.begin() + firstMasked, cycle_.end());

    /* Maximal runs of survivors. A run of ONE edge is kept: a single grid
     * line of amber standing out past the blue is exactly what this is for,
     * and two points are enough for a strip. */
    int runStart = -1;
    for (int i = 1; i < count; i++) {
        if (masked(i)) {
            if (runStart >= 0) emitLoop(runStart, i - runStart, false, out);
            runStart = -1;
            continue;
        }
        if (runStart < 0) runStart = i;
    }
    if (runStart >= 0) emitLoop(runStart, count - runStart, false, out);
}

void BandExtractor::extract(const Band& band, const Band* suppress, LoopSet& out)
{
    const Lattice& lattice = world_.lattice();

    connectivity_.rebuild(band);
    visited_.assign(static_cast<std::size_t>(lattice.cellCount() * kDirCount), 0);
    out.clear();

    for (int start = 0; start < lattice.cellCount(); start++) {
        if (!band.contains(start)) continue;

        for (Dir startDir : kAllDirs) {
            if (!connectivity_.isBoundary(start, startDir)) continue;

            const EdgeId startEdge(start, startDir);
            if (visited_[static_cast<std::size_t>(startEdge.raw())]) continue;

            /* THE WALK IS UNCHANGED, and it walks suppressed edges too: the
             * corner rule chains one boundary edge to the next, so skipping an
             * edge here would lose the thread rather than shorten the line.
             * Suppression is applied when the cycle is emitted. */
            cycle_.clear();
            int cell = start;
            Dir d    = startDir;

            for (;;) {
                const EdgeId edge(cell, d);
                if (visited_[static_cast<std::size_t>(edge.raw())]) break;
                visited_[static_cast<std::size_t>(edge.raw())] = 1;
                cycle_.push_back(edge);

                int nextCell = 0;
                Dir nextDir  = d;
                if (!connectivity_.successor(cell, d, nextCell, nextDir)) break;
                cell = nextCell;
                d    = nextDir;
            }

            emit(suppress, out);
        }
    }
}

}  // namespace game
