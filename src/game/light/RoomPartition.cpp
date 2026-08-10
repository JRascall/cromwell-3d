#include "game/light/RoomPartition.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace game {


RoomPartition::RoomPartition(const World& world)
    : world_(world)
{
    const Lattice& lattice = world_.lattice();
    cellRoom_.assign(static_cast<std::size_t>(lattice.cellCount()), -1);

    /* Before the flood, because the flood reads it on every step. */
    classifyEnclosure();

    /* An explicit stack rather than recursion: a room can be the whole board,
     * and 5184 frames of stack to flood an empty map is a crash waiting for
     * the first map bigger than the default. */
    std::vector<Cell> stack;

    for (int z = 0; z < lattice.depth(); z++) {
        for (int y = 0; y < lattice.height(); y++) {
            for (int x = 0; x < lattice.width(); x++) {
                if (!isOpen(x, y, z)) continue;
                if (cellRoom_[static_cast<std::size_t>(lattice.index(x, y, z))] >= 0) continue;

                const int room = static_cast<int>(rooms_.size());
                RoomVolume volume;
                volume.minimum = Cell{ x, y, z };
                volume.maximum = Cell{ x, y, z };

                stack.clear();
                stack.push_back(Cell{ x, y, z });
                cellRoom_[static_cast<std::size_t>(lattice.index(x, y, z))] = room;

                while (!stack.empty()) {
                    const Cell current = stack.back();
                    stack.pop_back();

                    volume.cellCount++;
                    volume.minimum.x = std::min(volume.minimum.x, current.x);
                    volume.minimum.y = std::min(volume.minimum.y, current.y);
                    volume.minimum.z = std::min(volume.minimum.z, current.z);
                    volume.maximum.x = std::max(volume.maximum.x, current.x);
                    volume.maximum.y = std::max(volume.maximum.y, current.y);
                    volume.maximum.z = std::max(volume.maximum.z, current.z);

                    /* Open sky above means outdoors. The whole component
                     * shares one enclosure class — the flood refuses to cross
                     * between them — so this is really being read off the seed
                     * cell, and testing every cell only makes that robust to a
                     * future change in what "indoor" means. */
                    if (!isIndoor(current.x, current.y, current.z))
                        volume.outdoor = true;

                    const bool indoorHere = isIndoor(current.x, current.y, current.z);

                    const auto visit = [&](int nx, int ny, int nz) {
                        if (!lattice.isValid(nx, ny, nz)) return;
                        if (!isOpen(nx, ny, nz)) return;

                        /* THE DOORWAY STOPS HERE. An open face between an
                         * indoor cell and an outdoor one is a doorway, an
                         * archway or a blown-out wall, and none of them make
                         * the street and the room one reflection environment.
                         * Without this test the demo map floods to a single
                         * room through one doorway. */
                        if (isIndoor(nx, ny, nz) != indoorHere) return;

                        int& owner = cellRoom_[static_cast<std::size_t>(
                            lattice.index(nx, ny, nz))];
                        if (owner >= 0) return;

                        owner = room;
                        stack.push_back(Cell{ nx, ny, nz });
                    };

                    for (Dir d : kAllDirs)
                        if (connectedHorizontally(current, d))
                            visit(current.x + dx(d), current.y + dy(d), current.z);

                    if (connectedVertically(current.x, current.y, current.z))
                        visit(current.x, current.y, current.z + 1);
                    if (current.z > 0 &&
                        connectedVertically(current.x, current.y, current.z - 1))
                        visit(current.x, current.y, current.z - 1);
                }

                rooms_.push_back(volume);
            }
        }
    }

    /* THE LARGEST outdoor component, not the first. A sealed courtyard reaches
     * the top layer and is therefore "outdoor" by the border test above, and
     * so is the street; the street is the one that should own the fallback
     * bounds, and on any real map it is much the bigger of the two. */
    int best = -1;
    for (int i = 0; i < static_cast<int>(rooms_.size()); i++) {
        if (!rooms_[static_cast<std::size_t>(i)].outdoor) continue;
        if (best < 0 || rooms_[static_cast<std::size_t>(i)].cellCount >
                        rooms_[static_cast<std::size_t>(best)].cellCount)
            best = i;
    }
    outdoor_ = best;

    chooseCaptureCells();
}

bool RoomPartition::isOpen(int x, int y, int z) const
{
    const Tile* tile = world_.tryAt(x, y, z);
    return tile != nullptr && !tile->blocked;
}

bool RoomPartition::connectedHorizontally(const Cell& from, Dir d) const
{
    const Edge edge = world_.effectiveEdge(from.x, from.y, from.z, d);

    /* Full cover seals, half cover does not — a waist-high barrier divides no
     * room, and treating it as one would shatter an open-plan floor into a
     * probe per desk. Windows are full cover; see the header. */
    return edge.cover != Cover::Full;
}

bool RoomPartition::connectedVertically(int x, int y, int z) const
{
    const Tile* lower = world_.tryAt(x, y, z);
    const Tile* upper = world_.tryAt(x, y, z + 1);
    if (lower == nullptr || upper == nullptr) return false;

    /* Two separate seals and either one is enough: the upper cell's floor slab
     * and the lower cell's roof plane. They are authored independently — a
     * canopy is a roof with no floor above it — so both have to be asked. */
    if (upper->hasFloor) return false;
    if (lower->canopy) return false;

    /* A STAIRWELL MUST NOT MERGE TWO STOREYS, and without this it does.
     *
     * A staircase is authored as a HOLE in the floor slab — the cells over the
     * flight simply have no floor — so the two tests above both pass and the
     * flood walks straight up the stairs. The ground floor and the storey
     * above become one room, its parallax box spans both, and a ground-floor
     * wall starts reflecting first-floor geometry: the original leak, arrived
     * through the stairs instead of through the wall.
     *
     * So an INDOOR volume stops at its storey boundary whether or not there is
     * a slab there. That is not a fudge — this lattice is explicitly
     * storey-based (three z cells per building floor, see Constants.hpp), and
     * "one probe per storey per building" is the granularity that makes the
     * parallax box mean something. An open double-height atrium is the case it
     * gets wrong, and it gets it wrong by splitting one volume into two
     * plausible ones rather than by leaking between them.
     *
     * OUTDOORS IS EXEMPT and must be. The street is one volume all the way up;
     * cutting it per storey would slice open sky into three stacked slabs and
     * hand a rooftop a different reflection from the pavement below it. */
    const bool crossesStorey = Lattice::storeyOfZ(z) != Lattice::storeyOfZ(z + 1);
    if (crossesStorey && isIndoor(x, y, z)) return false;

    return true;
}

bool RoomPartition::isIndoor(int x, int y, int z) const
{
    const Lattice& lattice = world_.lattice();
    if (!lattice.isValid(x, y, z)) return false;
    return indoor_[static_cast<std::size_t>(lattice.index(x, y, z))] != 0;
}

void RoomPartition::classifyEnclosure()
{
    const Lattice& lattice = world_.lattice();
    indoor_.assign(static_cast<std::size_t>(lattice.cellCount()), 0);

    /* ONE TOP-DOWN SWEEP PER COLUMN, which is why this is a pass of its own
     * rather than a predicate: asking "is anything above me solid" per cell
     * would be O(depth) per query inside a flood that queries constantly.
     *
     * `ceiling` carries "something at a higher z in this column is solid"
     * downward. A cell's OWN canopy counts for the cell itself, because a
     * canopy is a roof plane at the TOP of its cell — the cell is under it. */
    for (int y = 0; y < lattice.height(); y++) {
        for (int x = 0; x < lattice.width(); x++) {
            bool ceiling = false;

            for (int z = lattice.depth() - 1; z >= 0; z--) {
                const Tile& tile = world_.at(Cell{ x, y, z });

                indoor_[static_cast<std::size_t>(lattice.index(x, y, z))] =
                    (ceiling || tile.canopy) ? 1 : 0;

                /* A floor is at the BOTTOM of its cell, so it becomes a
                 * ceiling for everything BELOW — which is why the flag is
                 * updated after this cell has already been classified. */
                if (tile.hasFloor || tile.canopy || tile.blocked) ceiling = true;
            }
        }
    }
}

void RoomPartition::chooseCaptureCells()
{
    const Lattice& lattice = world_.lattice();

    /* Two passes: accumulate each room's centroid, then find the open cell
     * nearest it. The nearest-open search is what keeps an L-shaped room's
     * probe out of the wall between its arms — a capture from inside solid
     * geometry is six black faces, and it fails silently. */
    std::vector<long long> sumX(rooms_.size(), 0);
    std::vector<long long> sumY(rooms_.size(), 0);
    std::vector<long long> sumZ(rooms_.size(), 0);

    for (int index = 0; index < lattice.cellCount(); index++) {
        const int room = cellRoom_[static_cast<std::size_t>(index)];
        if (room < 0) continue;

        const Cell cell = lattice.cellAt(index);
        sumX[static_cast<std::size_t>(room)] += cell.x;
        sumY[static_cast<std::size_t>(room)] += cell.y;
        sumZ[static_cast<std::size_t>(room)] += cell.z;
    }

    std::vector<Cell> centroid(rooms_.size());
    std::vector<long long> best(rooms_.size(), std::numeric_limits<long long>::max());

    for (std::size_t i = 0; i < rooms_.size(); i++) {
        const long long count = std::max(1, rooms_[i].cellCount);
        centroid[i] = Cell{ static_cast<int>(sumX[i] / count),
                            static_cast<int>(sumY[i] / count),
                            static_cast<int>(sumZ[i] / count) };
        /* Seeded with the room's own minimum so a room always has a capture
         * cell, even if the search below somehow matches nothing. */
        rooms_[i].capture = rooms_[i].minimum;
    }

    /* WHICH COLUMNS A BUILDING STANDS ON, in plan.
     *
     * THE OUTDOOR PROBE MUST NOT STAND INSIDE ONE, and left to the plain
     * nearest-the-centroid rule it does. On the demo map the outdoor volume's
     * centroid is the middle of the board, the nearest open cell to it is on
     * the building's own unroofed upper storey, and the fallback probe ends up
     * standing on a balcony. From there it can see straight into the storey
     * below through its openings — and because the outdoor probe's parallax
     * box is the whole world, EVERY exterior wall re-aims its reflection ray
     * from that point. The facade then reflects the building's interior, which
     * is the original leak arriving from the outside in.
     *
     * A column is barred if any INTERIOR room covers it, so the outdoor probe
     * is pushed out onto open ground where its view is the street and the
     * outside of the buildings — which is what a fallback probe is for. */
    std::vector<char> barred(static_cast<std::size_t>(lattice.width()) *
                             static_cast<std::size_t>(lattice.height()), 0);
    for (const RoomVolume& room : rooms_) {
        if (room.outdoor) continue;
        for (int y = room.minimum.y; y <= room.maximum.y; y++)
            for (int x = room.minimum.x; x <= room.maximum.x; x++)
                if (lattice.inBounds(x, y))
                    barred[static_cast<std::size_t>(y) *
                           static_cast<std::size_t>(lattice.width()) +
                           static_cast<std::size_t>(x)] = 1;
    }

    for (int index = 0; index < lattice.cellCount(); index++) {
        const int room = cellRoom_[static_cast<std::size_t>(index)];
        if (room < 0) continue;

        const std::size_t r = static_cast<std::size_t>(room);
        const Cell cell = lattice.cellAt(index);

        Cell target = centroid[r];

        if (rooms_[r].outdoor) {
            if (barred[static_cast<std::size_t>(cell.y) *
                       static_cast<std::size_t>(lattice.width()) +
                       static_cast<std::size_t>(cell.x)]) continue;

            /* AIMED AT STANDING HEIGHT, not at the volume's centroid. The
             * outdoor volume is mostly empty sky, so its centroid floats well
             * above the roofline — and a fallback probe up there reflects
             * rooftops onto ground-level facades. One cell up from the ground
             * is where the street actually is. */
            target.z = 1;
        }

        /* Manhattan, and the vertical term is weighted: a cell is 96uu across
         * and 64uu tall, but more to the point a probe wants to sit at eye
         * level in the middle of a floor rather than drift up through a
         * stairwell to the geometric centre of a two-storey volume. */
        const long long distance = std::abs(cell.x - target.x) +
                                   std::abs(cell.y - target.y) +
                                   2LL * std::abs(cell.z - target.z);

        if (distance < best[r]) {
            best[r] = distance;
            rooms_[r].capture = cell;
        }
    }
}

int RoomPartition::roomOf(int x, int y, int z) const
{
    const Lattice& lattice = world_.lattice();
    if (!lattice.isValid(x, y, z)) return -1;
    return cellRoom_[static_cast<std::size_t>(lattice.index(x, y, z))];
}

}  // namespace game
