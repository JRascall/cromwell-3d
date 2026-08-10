#include "game/border/band/BandExtractor.hpp"

#include <cmath>

namespace game {


void BandExtractor::extract(const Band& band, LoopSet& out)
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

            const int first = out.edgeCount();
            int  cell = start;
            Dir  d    = startDir;
            int  minZ = lattice.depth();

            for (;;) {
                const EdgeId edge(cell, d);
                if (visited_[static_cast<std::size_t>(edge.raw())]) break;
                visited_[static_cast<std::size_t>(edge.raw())] = 1;
                out.appendEdge(edge);

                minZ = std::min(minZ, lattice.cellAt(cell).z);

                int nextCell = 0;
                Dir nextDir  = d;
                if (!connectivity_.successor(cell, d, nextCell, nextDir)) break;
                cell = nextCell;
                d    = nextDir;
            }

            const int count = out.edgeCount() - first;
            if (count <= 0) continue;

            /* closed when the last edge's far corner is the first's near one */
            const EdgeId firstEdge = out.edges()[static_cast<std::size_t>(first)];
            const EdgeId lastEdge  = out.edges()[static_cast<std::size_t>(first + count - 1)];

            const Cell firstCell = lattice.cellAt(firstEdge.cell());
            const Cell lastCell  = lattice.cellAt(lastEdge.cell());

            const EdgeSegment firstSegment = edgeCorners(firstCell.x, firstCell.y, firstEdge.dir());
            const EdgeSegment lastSegment  = edgeCorners(lastCell.x,  lastCell.y,  lastEdge.dir());

            Loop loop;
            loop.first  = first;
            loop.count  = count;
            loop.minZ   = minZ;
            loop.closed = std::fabs(lastSegment.toX - firstSegment.fromX) < 1e-5f &&
                          std::fabs(lastSegment.toY - firstSegment.fromY) < 1e-5f;
            out.appendLoop(loop);
        }
    }
}

}  // namespace game
