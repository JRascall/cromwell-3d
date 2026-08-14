#include "game/render/scene/ProbePlacement.hpp"

#include "cromwell/diag/Logger.hpp"
#include "game/lattice/Constants.hpp"
#include "game/lattice/Lattice.hpp"
#include "game/light/RoomPartition.hpp"

#include <algorithm>
#include <cmath>

namespace game {
namespace {

/* A cell's world-space box. THE AXIS SWAP LIVES HERE and nowhere else: the
 * lattice's y is the world's z, because the world is y-up and the lattice is a
 * floor plan. RoomPartition deliberately reports cells rather than world
 * boxes so that this conversion has exactly one home. */
void cellBox(const Cell& minimum, const Cell& maximum, Vec3& outMin, Vec3& outMax)
{
    outMin = Vec3{ static_cast<float>(minimum.x),
                   Lattice::cellBaseHeight(minimum.z),
                   static_cast<float>(minimum.y) };
    outMax = Vec3{ static_cast<float>(maximum.x) + 1.0f,
                   Lattice::cellBaseHeight(maximum.z + 1),
                   static_cast<float>(maximum.y) + 1.0f };
}

Vector3 toRaylib(Vec3 value) { return Vector3{ value.x, value.y, value.z }; }
Vec3 toVec3(Vector3 value)   { return Vec3{ value.x, value.y, value.z }; }

/* AN OPEN OUTDOOR CELL NEAR A POINT, or none.
 *
 * A capture taken from inside solid geometry is six black faces that fail
 * silently, so a block's capture point cannot simply be its centre — the middle
 * of a street block is as likely to be inside a building as not. RoomPartition
 * already solved this for rooms by choosing an open cell; this does the same for
 * an arbitrary target, spiralling outward over the floor plan until it finds a
 * cell the flood assigned to the OUTDOORS.
 *
 * BY RING, so the first hit is the nearest. That distance — capture point to the
 * surfaces it will be reflected in — is the whole quantity this system's
 * accuracy depends on, so it is worth searching in the order that minimises it
 * rather than scanning the block and taking whatever turns up first. */
bool openOutdoorCellNear(const RoomPartition& rooms, const Lattice& lattice,
                         int targetX, int targetY, int storey, Cell& out)
{
    const int outdoor = rooms.outdoorRoom();
    if (outdoor < 0) return false;

    const int reach = std::max(lattice.width(), lattice.height());
    for (int ring = 0; ring <= reach; ring++) {
        for (int dy = -ring; dy <= ring; dy++) {
            for (int dx = -ring; dx <= ring; dx++) {
                /* The ring's PERIMETER only — its interior belonged to a
                 * smaller ring and has already been tested. Without this the
                 * search is quadratic in the ring rather than linear, and it
                 * returns the same answer. */
                if (ring > 0 && std::abs(dx) != ring && std::abs(dy) != ring) continue;

                const int x = targetX + dx;
                const int y = targetY + dy;
                if (x < 0 || y < 0 || x >= lattice.width() || y >= lattice.height()) continue;
                if (rooms.roomOf(x, y, storey) != outdoor) continue;

                out = Cell{ x, y, storey };
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::vector<DeviceProbeSet::Volume> placeProbeVolumes(
    const RoomPartition& rooms, const Lattice& lattice,
    Vec3 worldMinimum, Vec3 worldMaximum)
{
    std::vector<DeviceProbeSet::Volume> placed;

    /* Rooms are placed largest-first so that if a map has more than the
     * engine's probe ceiling of them, the ones dropped are the smallest — a
     * broom cupboard falling back to its parent volume's reflection is a far
     * better failure than the main hall doing so.
     *
     * THE OUTDOOR ROOM IS NOT IN THIS LIST. It becomes a grid of blocks plus
     * one board-sized fallback further down — see the note there. */
    std::vector<int> order;
    for (int i = 0; i < rooms.roomCount(); i++) {
        const RoomVolume& room = rooms.rooms()[static_cast<std::size_t>(i)];
        if (room.outdoor) continue;

        /* A room of one or two cells is a doorway recess or the inside of a
         * stack of cover, not a space with its own environment. Placing a
         * probe there spends a layer to describe six walls. */
        if (room.cellCount < 4) continue;
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return rooms.rooms()[static_cast<std::size_t>(a)].cellCount >
               rooms.rooms()[static_cast<std::size_t>(b)].cellCount;
    });

    /* THE TWO CEILINGS ARE THE SAME NUMBER and are asserted to be rather than
     * assumed. Both sets carry shader volume arrays of their own kMaxProbes,
     * and a placement capped at the larger of the two would silently drop its
     * tail into whichever set is smaller — in the set that reads them, not
     * here, where the log line says how many were placed. */
    static_assert(DeviceProbeSet::kMaxProbes == ReflectionProbeSet::kMaxProbes,
                  "one placement feeds both sets; their ceilings must agree");

    /* RESERVED: one layer for the board-sized fallback, and at least four for
     * the exterior grid. An interior list long enough to eat the outdoors would
     * leave every facade on the analytic sky, which is a worse failure than a
     * small room sharing its neighbour's reflection. */
    constexpr int kFallbackLayers = 1;
    constexpr int kMinimumBlocks  = 4;

    /* THE DEFAULT PRIORITY LADDER, and it is only a default: priority is a
     * designer's field now, and a map format carrying authored volumes sets it
     * directly. These three are what a generated map needs to behave the way
     * the derived-from-volume rule used to, without the rule.
     *
     * A room beats the street it stands on; the street beats the board-sized
     * fallback under all of it. Spaced by whole numbers so an authored volume
     * can be slotted between two of them. */
    constexpr float kInteriorPriority = 20.0f;
    constexpr float kExteriorPriority = 10.0f;
    constexpr float kFallbackPriority = 0.0f;

    /* HOW FAR EXTERIOR BLOCKS REACH INTO EACH OTHER. One tile: wide enough that
     * the crossfade is not a line, narrow enough that a fragment is never
     * meaningfully inside three blocks at once — selectProbes tracks a winner
     * and one runner-up, so a third overlapping volume is simply not in the
     * answer. That is the real constraint on this number. */
    constexpr float kBlockOverlap = 1.0f;

    const int ceiling = DeviceProbeSet::kMaxProbes - kFallbackLayers - kMinimumBlocks;
    if (static_cast<int>(order.size()) > ceiling) {
        LOGGER.warn("probes: {} rooms, {} layers - dropping the {} smallest",
                    static_cast<int>(order.size()), ceiling,
                    static_cast<int>(order.size()) - ceiling);
        order.resize(static_cast<std::size_t>(ceiling));
    }

    for (int index : order) {
        const RoomVolume& room = rooms.rooms()[static_cast<std::size_t>(index)];

        DeviceProbeSet::Volume probe;
        probe.interior = true;

        cellBox(room.minimum, room.maximum, probe.parallaxMin, probe.parallaxMax);

        /* THE CAPTURE POINT IS NOT THE BOX CENTRE. It is the middle of an OPEN
         * cell near the middle of the room — RoomPartition picked it, because
         * the centroid of an L-shaped room is inside the wall between its
         * arms, and a capture from inside solid geometry is six black faces
         * that fail silently. */
        probe.capture = Vec3{ static_cast<float>(room.capture.x) + 0.5f,
                              Lattice::cellBaseHeight(room.capture.z) + kCellHeight * 0.5f,
                              static_cast<float>(room.capture.y) + 0.5f };

        {
            /* INFLUENCE IS EXACTLY THE ROOM'S CELLS — no margin, deliberately.
             *
             * Growing the box was tried, to stop an interior wall face landing
             * ambiguously on the boundary it sits astride. It fixes that and
             * causes something worse: a wall is 0.09 thick and centred on the
             * cell boundary, so a margin big enough to capture the interior
             * face also captures the EXTERIOR one, and the room starts
             * claiming fragments on the street. That is the leak the whole
             * per-room design exists to close, arriving from the other
             * direction.
             *
             * The shader disambiguates by stepping along the surface normal
             * before it tests — a surface belongs to the volume it faces. See
             * probeSamplePoint, in common/environment.glsl and rhi/probes.glsl
             * alike. With that, exact bounds are both correct and the only
             * thing that stays correct when somebody changes wall thickness. */
            probe.influenceMin = probe.parallaxMin;
            probe.influenceMax = probe.parallaxMax;

            /* ABOVE THE STREET. A room and the block outside it both contain
             * a fragment on the room's own floor, and the room must win. High
             * wins now, so this is a number rather than the old implicit
             * "smaller box" — see DeviceProbeSet::Volume::priority. */
            probe.priority = kInteriorPriority;

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
        LOGGER.info("probes:   [{}] {} {} cells, {:.0f}..{:.0f} x {:.1f}..{:.1f} x "
                    "{:.0f}..{:.0f}, from ({:.1f} {:.1f} {:.1f})",
                    static_cast<int>(placed.size()), "interior",
                    room.cellCount,
                    probe.parallaxMin.x, probe.parallaxMax.x,
                    probe.parallaxMin.y, probe.parallaxMax.y,
                    probe.parallaxMin.z, probe.parallaxMax.z,
                    probe.capture.x, probe.capture.y, probe.capture.z);

        placed.push_back(probe);
    }

    /* ===================== THE OUTDOORS, AS BLOCKS ==========================
     *
     * A cubemap is an environment at INFINITY by definition, and parallax
     * correction is what repairs that — only as well as the box it is given.
     * Every technique "defines an approximation of the geometry surrounding the
     * local cubemap, and the simpler the approximation, the more efficient the
     * algorithm will be AT THE PRICE OF ACCURACY" (Lagarde). Unity's Box
     * Projection says to size the box to the room; Source takes a bounding-box
     * brush an artist draws round the local geometry; Source 2 ships
     * parallax-corrected cubemaps as standard.
     *
     * THE OUTDOORS WAS ONE PROBE WITH A BOARD-SIZED BOX, which is the
     * degenerate case of that approximation. A reflection ray off a window ran
     * twenty tiles to the box edge and was re-aimed from a capture point ten
     * tiles away, so geometry behind the camera arrived in the pane on the
     * WRONG SIDE — reported, reproduced, and fixed by moving the capture onto
     * the viewpoint.
     *
     * TURNING CORRECTION OFF WAS TRIED FIRST AND IS NOT THE ANSWER. That is the
     * infinity the correction exists to repair, and the measurement says so:
     * moving the capture point 3.7 tiles with correction off changed the pane by
     * a mean of 2.44/255, because with no correction the capture point alters
     * only what is IN the cubemap, never where the lookup points.
     *
     * So the street becomes blocks, each with a box its own size and a capture
     * inside it. See study/realtime_reflections.md section 2.4. */
    const int spare = DeviceProbeSet::kMaxProbes - static_cast<int>(placed.size())
                    - kFallbackLayers;

    /* THE LARGEST SQUARE GRID THAT FITS THE LAYERS LEFT. Derived rather than
     * tuned: the accuracy of a block probe is set by how far a surface can sit
     * from its capture point, so the right number of blocks is simply as many
     * as there is room for. */
    int side = 1;
    while ((side + 1) * (side + 1) <= spare) side++;

    /* THE STOREY THE STREET IS ON, taken from the outdoor room's own capture
     * cell — so a map whose ground floor is not storey zero needs nothing
     * changed here. */
    const int outdoorIndex = rooms.outdoorRoom();
    const int storey = outdoorIndex >= 0
        ? rooms.rooms()[static_cast<std::size_t>(outdoorIndex)].capture.z : 0;

    const Vec3 span = worldMaximum - worldMinimum;

    for (int bz = 0; bz < side; bz++) {
        for (int bx = 0; bx < side; bx++) {
            const float lowX  = static_cast<float>(bx) / static_cast<float>(side);
            const float highX = static_cast<float>(bx + 1) / static_cast<float>(side);
            const float lowZ  = static_cast<float>(bz) / static_cast<float>(side);
            const float highZ = static_cast<float>(bz + 1) / static_cast<float>(side);

            DeviceProbeSet::Volume block;
            block.interior = false;

            /* HARD BOUNDS IN X AND Z, THE WORLD'S FULL RANGE IN Y. A street is
             * open to the sky, so a box that capped the height would re-aim
             * every upward ray into a ceiling that is not there. */
            block.parallaxMin = Vec3{ worldMinimum.x + span.x * lowX, worldMinimum.y,
                                      worldMinimum.z + span.z * lowZ };
            block.parallaxMax = Vec3{ worldMinimum.x + span.x * highX, worldMaximum.y,
                                      worldMinimum.z + span.z * highZ };

            /* INFLUENCE OVERLAPS ITS NEIGHBOURS BY kBlockOverlap on every side,
             * while the parallax box above stays hard at the block bounds. That
             * is the influence/parallax split doing exactly what it is for.
             *
             * THE BLOCKS USED NOT TO OVERLAP, because selectProbes ranked by box
             * VOLUME and two equal blocks tied — the tie fell to array order
             * rather than to the box the fragment sat deeper inside, so an
             * overlap picked a neighbour arbitrarily. Ranking is now explicit
             * priority first and depth-inside second, which is what makes an
             * overlap blend. Seams used to crossfade through the board-sized
             * fallback; now they crossfade into the neighbouring block, which is
             * a reflection of the right street rather than of the whole map.
             *
             * The transition matches the overlap so weight reaches 1 exactly at
             * the block's own bound: inside the overlap both neighbours are
             * below 1 and the shader splits them proportionally. */
            block.influenceMin = block.parallaxMin - Vec3{ kBlockOverlap, 0.0f, kBlockOverlap };
            block.influenceMax = block.parallaxMax + Vec3{ kBlockOverlap, 0.0f, kBlockOverlap };
            block.priority     = kExteriorPriority;
            block.transition   = kBlockOverlap;

            const Vec3 centre = (block.parallaxMin + block.parallaxMax) * 0.5f;

            Cell open{};
            if (!openOutdoorCellNear(rooms, lattice, static_cast<int>(centre.x),
                                     static_cast<int>(centre.z), storey, open))
                continue;   /* solid all the way out — nothing to capture from */

            block.capture = Vec3{ static_cast<float>(open.x) + 0.5f,
                                  Lattice::cellBaseHeight(open.z) + kCellHeight * 0.5f,
                                  static_cast<float>(open.y) + 0.5f };

            LOGGER.info("probes:   [{}] exterior {:.0f}..{:.0f} x {:.0f}..{:.0f}, "
                        "from ({:.1f} {:.1f} {:.1f})",
                        static_cast<int>(placed.size()),
                        block.parallaxMin.x, block.parallaxMax.x,
                        block.parallaxMin.z, block.parallaxMax.z,
                        block.capture.x, block.capture.y, block.capture.z);
            placed.push_back(block);
        }
    }

    /* AND THE FALLBACK UNDER ALL OF THEM. A fragment outside every block — a
     * seam between two, anything past the world bounds — lands here rather than
     * dropping to the analytic sky, which on a facade reads as a wall that is
     * not lit like its own street.
     *
     * NO CORRECTION ON THIS ONE, and it is the single place infinity is the
     * honest answer: its box is the whole board and stands in for no geometry at
     * all, so correcting against it would preserve the original bug in the one
     * probe that cannot avoid it. */
    if (outdoorIndex >= 0 && static_cast<int>(placed.size()) < DeviceProbeSet::kMaxProbes) {
        const RoomVolume& outside = rooms.rooms()[static_cast<std::size_t>(outdoorIndex)];

        DeviceProbeSet::Volume fallback;
        fallback.interior     = false;
        fallback.parallax     = false;
        fallback.parallaxMin  = worldMinimum;
        fallback.parallaxMax  = worldMaximum;
        fallback.influenceMin = worldMinimum;
        fallback.influenceMax = worldMaximum;
        /* UNDER EVERYTHING. Lowest priority, so it only answers where no block
         * and no room does. */
        fallback.priority     = kFallbackPriority;
        fallback.transition   = 0.0f;   /* nothing to fade INTO; it is the floor */
        fallback.capture      = Vec3{ static_cast<float>(outside.capture.x) + 0.5f,
                                      Lattice::cellBaseHeight(outside.capture.z)
                                          + kCellHeight * 0.5f,
                                      static_cast<float>(outside.capture.y) + 0.5f };
        placed.push_back(fallback);
    }

    LOGGER.info("probes: {} placed ({} interior)", static_cast<int>(placed.size()),
                static_cast<int>(std::count_if(placed.begin(), placed.end(),
                    [](const DeviceProbeSet::Volume& p) { return p.interior; })));

    return placed;
}

void placeProbes(DeviceProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vec3 worldMinimum, Vec3 worldMaximum)
{
    probes.clear();
    if (!probes.valid()) return;

    /* THE BOARD'S DIAGONAL, WITH A TILE OF SLACK. The scene has to fit inside
     * the capture's far plane or reflections clip against nothing — and the far
     * corner of the board seen from the opposite corner is as far as anything
     * on it can be. */
    probes.withCaptureFar((worldMaximum - worldMinimum).length() + 1.0f);

    for (const DeviceProbeSet::Volume& volume :
         placeProbeVolumes(rooms, lattice, worldMinimum, worldMaximum))
        probes.addProbe(volume);

    probes.markAllStale();
}

void placeProbes(ReflectionProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vector3 worldMinimum, Vector3 worldMaximum)
{
    probes.clear();
    if (!probes.valid()) return;

    const Vec3 minimum = toVec3(worldMinimum);
    const Vec3 maximum = toVec3(worldMaximum);

    probes.setCaptureFar((maximum - minimum).length() + 1.0f);

    /* THE ONE PLACE raylib's Vector3 RE-ENTERS, and it is four assignments at
     * the boundary rather than a second copy of the placement rules above. */
    for (const DeviceProbeSet::Volume& volume :
         placeProbeVolumes(rooms, lattice, minimum, maximum)) {
        ProbeVolume converted;
        converted.capture      = toRaylib(volume.capture);
        converted.parallaxMin  = toRaylib(volume.parallaxMin);
        converted.parallaxMax  = toRaylib(volume.parallaxMax);
        converted.influenceMin = toRaylib(volume.influenceMin);
        converted.influenceMax = toRaylib(volume.influenceMax);
        converted.transition   = volume.transition;
        converted.priority     = volume.priority;
        converted.interior     = volume.interior;
        converted.parallax     = volume.parallax;

        probes.addProbe(converted);
    }

    probes.markAllStale();
}

}  // namespace game
