/* MapAuthor.hpp — the write side of the world.
 *
 * SINGLE RESPONSIBILITY: mutate tile data. It reads only what it must to
 * write correctly, and answers no gameplay question.
 *
 * Two vocabularies, deliberately:
 *   - per CELL  (setEdge/setLadder)      — the 64uu lattice records
 *   - per STOREY (setWall/setSolid/...)  — how a map author actually thinks
 * Map authoring thinks in building floors; the lattice stores 64uu cells.
 * Full cover fills the whole storey (XCOM high cover spans past one cell);
 * half cover occupies just the surface cell (low cover is 64uu = one cell).
 */
#pragma once

#include "core/lattice/Cover.hpp"
#include "core/lattice/Direction.hpp"
#include "core/world/World.hpp"

#include <iosfwd>

namespace xcom {

class MapAuthor {
public:
    /* `diagnostics` receives ramp rejection messages; defaults to std::cerr.
     * Injected so tests can capture it instead of polluting the console. */
    explicit MapAuthor(World& world, std::ostream* diagnostics = nullptr);

    World& world() { return world_; }

    /* ---- per CELL ------------------------------------------------------ */
    void setEdge(int x, int y, int z, Dir d, Cover cover,
                 bool destructible, bool window);
    void clearEdge(int x, int y, int z, Dir d);
    void setLadder(int x, int y, int z, Dir d);

    /* ---- per STOREY ---------------------------------------------------- */
    void setWall(int x, int y, int storey, Dir d, Cover cover,
                 bool destructible, bool window);
    void clearWall(int x, int y, int storey, Dir d);
    void setLadderWall(int x, int y, int storey, Dir d);
    void setSolid(int x, int y, int storey);          /* fills the storey */

    /* ---- floors and ramps ---------------------------------------------- */
    int  setFloorAt(int x, int y, float height);      /* returns z, -1 if outside */
    void clearFloorAt(int x, int y, float height);

    /* Author a ramp, enforcing XCOM's slope band. Returns the owning z cell,
     * or -1 with a diagnostic — see RampValidator for why it never clamps. */
    int setRamp(int x, int y, Dir uphill, float baseHeight, float rise);

private:
    World&        world_;
    std::ostream* diagnostics_;
};

}  // namespace xcom
