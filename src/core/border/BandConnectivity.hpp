/* BandConnectivity.hpp — which in-band cells count as joined?
 *
 * SINGLE RESPONSIBILITY: build and serve the link table the contour walk
 * needs, including the corner-successor rule.
 *
 * MEMBERSHIP, NOT CONNECTIVITY — walls are deliberately ignored. A wall
 * standing between two reachable tiles sits INSIDE the band, and the border
 * draws nothing there. Testing connectivity instead made the line run down
 * both faces of every interior wall and hairpin around its tip, which is not
 * what XCOM does.
 *
 * HEIGHT is what separates, not walls: a roof directly above a reachable floor
 * must not merge into one blob, so a neighbour only counts if its surface sits
 * within one step. Heights are sampled at the MIDPOINT OF THE SHARED FACE
 * rather than at tile centres — identical for flat tiles, and the only
 * defensible point for a sloped one. That is what lets a flight link to the
 * ground at its low edge and to the landing at its high edge, while its SIDE
 * faces — sampled mid-slope, half a rise above the floor beside them — stay
 * boundaries, so the line runs up the flight's sides.
 *
 * The table is INSTANCE state. The C original kept it in a file-scope array,
 * which meant two bands could never be extracted at once.
 */
#pragma once

#include "core/border/Band.hpp"
#include "core/lattice/Direction.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

#include <array>
#include <vector>

namespace xcom {

class BandConnectivity {
public:
    explicit BandConnectivity(const World& world) : world_(world), terrain_(world) {}

    /* Recomputes the link table for `band`, which must outlive the calls
     * below. */
    void rebuild(const Band& band);

    /* The in-band cell across face `d`, or -1. */
    int linkedNeighbour(int cell, Dir d) const;

    /* A face with no CONNECTED in-band neighbour — one boundary edge. */
    bool isBoundary(int cell, Dir d) const;

    /* THE CORNER RULE. At the far corner of edge (cell, d) four tiles meet;
     * their edges starting at that corner are tried in rotational order —
     * hardest right turn first, then straight, then left, then the
     * 180-degree retrace — and the first that is itself a boundary edge wins.
     * Hugging right keeps the interior on the left all the way round.
     *
     *   A = forward + outward (diagonally across the corner) -> right turn
     *   B = forward                                          -> straight on
     *   T = ourselves                                        -> left turn
     *   D = the far side of our own face                     -> the retrace
     *       that traces a SLIT (see acrossSlit).
     *
     * Returns false only for a malformed band. */
    bool successor(int cell, Dir d, int& outCell, Dir& outDir) const;

private:
    /* The in-band cell in the neighbouring column nearest in height, within
     * `tolerance`, or -1. */
    int nearestInBand(int cell, Dir d, float tolerance) const;

    /* The partner on the far side of a SLIT — same search, ignoring the link
     * tolerance.
     *
     * A slit is a boundary with walkable surface on BOTH sides: the side face
     * of a ramp has ground beside it and the ramp above it, both in the band
     * and both part of one connected region, separated only by height.
     * Tracing it needs a fourth candidate, because the three ordinary turns
     * all sit on the near side.
     *
     * This does NOT resurrect wall-hugging. Under a membership test a wall
     * between two in-band tiles is not a boundary at all, so no slit exists
     * there. Slits come from height alone, which is why the bound is the
     * steepest legal ramp: a flight can differ from its neighbour by at most
     * one tile of rise, and anything larger is a storey change that must stay
     * a separate loop. */
    int acrossSlit(int cell, Dir d) const;

    const World& world_;
    Terrain      terrain_;
    const Band*  band_ = nullptr;

    std::vector<std::array<int, kDirCount>> links_;
};

}  // namespace xcom
