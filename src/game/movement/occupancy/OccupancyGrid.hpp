/* OccupancyGrid.hpp — which body is standing in each cell.
 *
 * SINGLE RESPONSIBILITY: map a cell index to the id of the body occupying it,
 * or to nothing. It does not own the bodies and cannot tell you what one is.
 *
 * WHY THIS EXISTS. "Who is standing here?" used to be answered by asking every
 * body in turn. That is fine at four of them and quadratic-feeling at two
 * hundred, because the question is asked from inside the ray caster's per-step
 * loop — so the cost is (ray steps x bodies) and BOTH grow as a game gets
 * bigger. Measured on the demo map, the roll call was fifteen times the cost of
 * the ray casting it was attached to.
 *
 * A grid answers it in one read, and — the part that matters — the answer costs
 * the same whether there are four bodies or four hundred.
 *
 * THIS IS THE SPATIAL INDEX AN RTS WOULD HAVE TO BUILD, and it is nearly free
 * here because this world is already a lattice. An RTS buckets floating-point
 * positions into invented cells to get this; a tile game's coordinates ARE the
 * bucket, so all that is missing is somewhere to write the contents. There is
 * no hash and no tree beneath this - one int per cell, about 20KB for the demo
 * map, small enough to sit in cache while a ray walks it.
 *
 * IDS ARE BODY IDS, NOT UNIT INDICES, and that distinction is deliberate rather
 * than pedantic. A crate, a door and a knocked-over lamppost occupy cells
 * exactly the way a soldier does — BodyComponent already says as much — and
 * they will want to be in this grid without being made into units to qualify.
 * Today the only registry of bodies is UnitRoster, so the ids happen to be its
 * indices; when props arrive they join the same grid under the same id space
 * rather than getting a parallel one.
 *
 * ONE BODY PER CELL. Two living bodies never share a cell: paths may cross a
 * squadmate but never end on one, so logical positions are only ever written
 * where nothing stands. Bodies mid-animation do not move their logical cell.
 */
#pragma once

#include "game/lattice/Lattice.hpp"

#include <vector>

namespace game {


class OccupancyGrid {
public:
    static constexpr int kEmpty = -1;

    /* Sizes the grid to the lattice and empties it. Until this is called the
     * grid is UNBOUND and every query says "empty" — callers test isBound()
     * and fall back, rather than silently reading an answer that is only
     * accidentally right. */
    void bind(const Lattice& lattice);

    bool isBound() const { return !slots_.empty(); }

    void clear();

    /* kEmpty when nothing is there. Unchecked: callers hold a valid index. */
    int at(int cellIndex) const { return slots_[static_cast<std::size_t>(cellIndex)]; }

    void place(int cellIndex, int bodyId)
    {
        slots_[static_cast<std::size_t>(cellIndex)] = bodyId;
    }

    /* Clears the slot only if `bodyId` is what is actually in it. A body
     * leaving a cell something else has since claimed must not erase the new
     * occupant — which is exactly what happens if a move is applied as
     * (erase old, place new) and the two overlap. */
    void erase(int cellIndex, int bodyId)
    {
        int& slot = slots_[static_cast<std::size_t>(cellIndex)];
        if (slot == bodyId) slot = kEmpty;
    }

private:
    std::vector<int> slots_;
};

}  // namespace game
