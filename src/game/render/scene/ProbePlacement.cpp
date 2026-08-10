#include "game/render/scene/ProbePlacement.hpp"

#include "raymath.h"

#include "game/lattice/Constants.hpp"
#include "game/lattice/Lattice.hpp"
#include "game/light/RoomPartition.hpp"

#include <algorithm>
#include <vector>

namespace game {
namespace {

/* A cell's world-space box. THE AXIS SWAP LIVES HERE and nowhere else: the
 * lattice's y is the world's z, because the world is y-up and the lattice is a
 * floor plan. RoomPartition deliberately reports cells rather than world
 * boxes so that this conversion has exactly one home. */
void cellBox(const Cell& minimum, const Cell& maximum, Vector3& outMin, Vector3& outMax)
{
    outMin = Vector3{ static_cast<float>(minimum.x),
                      Lattice::cellBaseHeight(minimum.z),
                      static_cast<float>(minimum.y) };
    outMax = Vector3{ static_cast<float>(maximum.x) + 1.0f,
                      Lattice::cellBaseHeight(maximum.z + 1),
                      static_cast<float>(maximum.y) + 1.0f };
}

float boxVolume(Vector3 minimum, Vector3 maximum)
{
    const Vector3 size = Vector3Subtract(maximum, minimum);
    return std::max(size.x, 0.0f) * std::max(size.y, 0.0f) * std::max(size.z, 0.0f);
}

}  // namespace

void placeProbes(ReflectionProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vector3 worldMinimum, Vector3 worldMaximum)
{
    probes.clear();
    if (!probes.valid()) return;

    probes.setCaptureFar(Vector3Length(Vector3Subtract(worldMaximum, worldMinimum)) + 1.0f);

    /* Rooms are placed largest-first so that if a map has more than the
     * engine's probe ceiling of them, the ones dropped are the smallest — a
     * broom cupboard falling back to its parent volume's reflection is a far
     * better failure than the main hall doing so. */
    std::vector<int> order;
    for (int i = 0; i < rooms.roomCount(); i++) {
        /* A room of one or two cells is a doorway recess or the inside of a
         * stack of cover, not a space with its own environment. Placing a
         * probe there spends a layer to describe six walls. */
        if (rooms.rooms()[static_cast<std::size_t>(i)].cellCount < 4) continue;
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return rooms.rooms()[static_cast<std::size_t>(a)].cellCount >
               rooms.rooms()[static_cast<std::size_t>(b)].cellCount;
    });

    const int ceiling = ReflectionProbeSet::kMaxProbes;
    if (static_cast<int>(order.size()) > ceiling) {
        TraceLog(LOG_WARNING, "PROBES: %d rooms, %d layers - dropping the %d smallest",
                 static_cast<int>(order.size()), ceiling,
                 static_cast<int>(order.size()) - ceiling);
        order.resize(static_cast<std::size_t>(ceiling));
    }

    for (int index : order) {
        const RoomVolume& room = rooms.rooms()[static_cast<std::size_t>(index)];

        ProbeVolume probe;
        probe.interior = !room.outdoor;

        cellBox(room.minimum, room.maximum, probe.parallaxMin, probe.parallaxMax);

        /* THE CAPTURE POINT IS NOT THE BOX CENTRE. It is the middle of an OPEN
         * cell near the middle of the room — RoomPartition picked it, because
         * the centroid of an L-shaped room is inside the wall between its
         * arms, and a capture from inside solid geometry is six black faces
         * that fail silently. */
        probe.capture = Vector3{ static_cast<float>(room.capture.x) + 0.5f,
                                 Lattice::cellBaseHeight(room.capture.z) + kCellHeight * 0.5f,
                                 static_cast<float>(room.capture.y) + 0.5f };

        if (room.outdoor) {
            /* THE OUTDOOR PROBE IS THE FALLBACK, so its influence is the whole
             * board rather than the flooded cells. The flood stops at every
             * building's outer wall, but a fragment ON that wall's exterior
             * face sits fractionally OUTSIDE the open cells — and if no
             * influence volume contained it, the facade would drop to the
             * analytic sky and read as unlit next to its own street.
             *
             * Its parallax box stays the world, which is what the single probe
             * always did and is correct for the one volume that really is
             * board-sized. */
            probe.parallaxMin  = worldMinimum;
            probe.parallaxMax  = worldMaximum;
            probe.influenceMin = worldMinimum;
            probe.influenceMax = worldMaximum;

            /* Lowest priority by construction: it is the biggest box, so any
             * interior containing a fragment wins over it. */
            probe.priority   = boxVolume(worldMinimum, worldMaximum);
            probe.transition = 0.0f;   /* nothing to fade INTO; it is the floor */
        } else {
            /* INFLUENCE IS EXACTLY THE ROOM'S CELLS — no margin, deliberately.
             *
             * Growing the box was tried, to stop an interior wall face landing
             * ambiguously on the boundary it sits astride. It fixes that and
             * causes something worse: a wall is 0.09 thick and centred on the
             * cell boundary, so a margin big enough to capture the interior
             * face also captures the EXTERIOR one, and the room starts
             * claiming fragments on the street. That is the leak this whole
             * change exists to close, arriving from the other direction.
             *
             * The shader disambiguates by stepping along the surface normal
             * before it tests — a surface belongs to the volume it faces. See
             * probeSamplePoint in environment.glsl. With that, exact bounds are
             * both correct and the only thing that stays correct when somebody
             * changes how thick a wall is. */
            probe.influenceMin = probe.parallaxMin;
            probe.influenceMax = probe.parallaxMax;

            probe.priority = boxVolume(probe.influenceMin, probe.influenceMax);

            /* Shorter than the shader's normal step, so a wall face lands at
             * full weight rather than part-blended with the street: the step
             * is 0.25 and the thickest surface puts a face 0.08 off the
             * boundary, leaving 0.17 of clearance for a 0.15 band. What the
             * band is really for is a soldier walking through a doorway, and
             * 0.15 of a tile is enough to make that a crossfade. */
            probe.transition = 0.15f;
        }

        /* PER ROOM, not just a total. "4 rooms" is true of a correct partition
         * and of one that split the street into quarters, and the difference
         * between those is the whole feature. The bounds and the capture point
         * are what make it checkable against the map. */
        TraceLog(LOG_INFO,
                 "PROBES:   [%d] %s %d cells, %.0f..%.0f x %.1f..%.1f x %.0f..%.0f, from (%.1f %.1f %.1f)",
                 probes.probeCount(), room.outdoor ? "outdoor " : "interior",
                 room.cellCount,
                 probe.parallaxMin.x, probe.parallaxMax.x,
                 probe.parallaxMin.y, probe.parallaxMax.y,
                 probe.parallaxMin.z, probe.parallaxMax.z,
                 probe.capture.x, probe.capture.y, probe.capture.z);

        probes.addProbe(probe);
    }

    (void)lattice;   /* placement is entirely in world space; the lattice is
                      * only here so callers cannot forget which one the
                      * partition was flooded from */

    probes.markAllStale();

    TraceLog(LOG_INFO, "PROBES: %d rooms placed (%d interior)", probes.probeCount(),
             static_cast<int>(std::count_if(probes.probes().begin(), probes.probes().end(),
                                            [](const ProbeVolume& p) { return p.interior; })));
}

}  // namespace game
