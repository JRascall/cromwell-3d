/* RoomPartition.hpp — which cells share an air volume.
 *
 * SINGLE RESPONSIBILITY: flood the lattice's open cells into connected
 * components and report each one's bounds. It places nothing and renders
 * nothing; ReflectionProbeSet turns these components into probes.
 *
 * WHY THIS EXISTS. A single reflection probe captured from the middle of the
 * board leaks: parallax correction re-aims the reflection ray from the probe's
 * centre, and for a wall's interior face that centre is on the far side of the
 * wall, so the cubemap returns geometry the wall itself blocks. The fix both
 * Source 2 and Unreal use is more probes, each bounded to the volume it is
 * allowed to describe — and on a tile lattice the volumes are not authored,
 * they are derivable. That is this file.
 *
 * DERIVED, NOT AUTHORED, and that is the whole argument for doing it here
 * rather than adding a volume entity to the map format. Walls are
 * destructible: blowing one open genuinely merges two rooms, and a partition
 * recomputed from the tiles handles that by being recomputed. An authored
 * volume would have to be invalidated by hand, and would be wrong in exactly
 * the frames anybody is looking.
 *
 * A WINDOW SEPARATES. Edge::window is full cover that happens to be
 * see-through, and for movement and LOS that distinction is the point. For a
 * room it is not: a pane is a solid surface with its own reflection, and the
 * room on either side of it is a different room. Treating a window as open
 * would merge a building's interior with the street through every window in
 * the facade, which is the leak this class exists to stop.
 *
 * SO DOES A DOORWAY, AND CONNECTIVITY ALONE CANNOT SAY SO. This is the part
 * that has to be got right or the whole class does nothing. A pure flood over
 * open cells finds ONE room on the demo map: the building's ground floor has a
 * doorway, the doorway is an open face, and the flood walks straight through
 * it into the street. Every interior merges with the outdoors and the probe
 * set collapses back to the single board-sized probe it was meant to replace.
 *
 * The criterion that does work is ENCLOSURE, not reachability: a cell is
 * indoors when something in its column above it is solid — a floor slab, a
 * canopy, or blocked mass. The flood then refuses to cross between an indoor
 * cell and an outdoor one, so a doorway is a boundary even though it is an
 * opening. Two indoor rooms sharing an interior door still merge, which is a
 * far smaller error than merging a room with the street: they are lit alike
 * and reflect alike, and the wall between them was never the wall that leaked.
 */
#pragma once

#include "game/lattice/Cell.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {


/* One connected volume of open cells, in LATTICE coordinates. Converting to
 * world space is the renderer's job — it is the only side that knows a cell's
 * y is a world z, and turning that around here would put a rendering
 * assumption in core. */
struct RoomVolume {
    /* Inclusive cell bounds. */
    Cell minimum{};
    Cell maximum{};

    /* A cell inside the room, as near its middle as an OPEN cell gets. The
     * centroid of an L-shaped room lands in the wall between its arms, and a
     * probe captured from inside a wall sees six black faces — so this is the
     * open cell nearest that centroid rather than the centroid itself. */
    Cell capture{};

    int cellCount = 0;

    /* Nothing solid anywhere above it in its column — open sky. Exactly one
     * room is normally outdoors; it is the fallback every fragment outside
     * every other room falls back to, and it is the one room whose bounds are
     * deliberately the whole board. */
    bool outdoor = false;
};

class RoomPartition {
public:
    /* Floods `world` immediately. Cheap enough to redo on any edit — one pass
     * over the lattice with a queue, which at 24x24x9 is 5184 cells. */
    explicit RoomPartition(const World& world);

    const std::vector<RoomVolume>& rooms() const { return rooms_; }
    int roomCount() const { return static_cast<int>(rooms_.size()); }

    /* The room owning a cell, or -1 for solid mass and out-of-bounds. */
    int roomOf(int x, int y, int z) const;
    int roomOf(const Cell& c) const { return roomOf(c.x, c.y, c.z); }

    /* Index into rooms() of the outdoor volume, or -1 if the board is sealed
     * (which no real map is, but a test fixture can be). */
    int outdoorRoom() const { return outdoor_; }

private:
    /* Can air pass from `from` to its neighbour across `d`? Full cover of any
     * kind stops it, window included — see the header. */
    bool connectedHorizontally(const Cell& from, Dir d) const;

    /* Can air pass from cell z to z+1 at (x, y)? A floor on the upper cell or
     * a canopy on the lower one seals it. */
    bool connectedVertically(int x, int y, int z) const;

    /* Solid mass belongs to no room. */
    bool isOpen(int x, int y, int z) const;

    /* Is anything above this cell in its column solid? See the header — this
     * is what makes a doorway a room boundary. */
    bool isIndoor(int x, int y, int z) const;

    void classifyEnclosure();
    void chooseCaptureCells();

    const World&            world_;
    std::vector<int>        cellRoom_;   /* flat lattice index -> room, -1 solid */
    std::vector<char>       indoor_;     /* flat lattice index -> has a ceiling  */
    std::vector<RoomVolume> rooms_;
    int                     outdoor_ = -1;
};

}  // namespace game
