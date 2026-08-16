/* StoreyGeometryEmitter.hpp — turn one storey of tile data into triangles.
 *
 * SINGLE RESPONSIBILITY: decide what the world LOOKS like. It reads tile data
 * and writes vertices; it owns no GPU resource and draws nothing.
 *
 * The scene is derived entirely from tile data — edit the data, re-emit, and
 * the world reflects it.
 *
 * Every box goes into the buffer for its SurfaceKind rather than into one
 * shared stream, so the batching a textured material set needs falls out of
 * the same single walk over the tiles.
 */
#pragma once

#include "game/light/RoomPartition.hpp"
#include "game/query/BlockedMass.hpp"
#include "game/query/LadderQuery.hpp"
#include "game/world/World.hpp"
#include "cromwell/geometry/SurfaceBuffers.hpp"

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

class StoreyGeometryEmitter {
public:
    /* Floods the room partition up front. That is one pass over the lattice
     * per rebuild, which happens on a grenade rather than on a frame — cold
     * code, and the alternative is asking the same question per wall. */
    explicit StoreyGeometryEmitter(const World& world)
        : world_(world), mass_(world), ladders_(world), rooms_(world) {}

    void emit(int storey, SurfaceBuffers& out) const;

    /* ONE RECTANGLE OF TILES OF ONE STOREY, half-open in x and y.
     *
     * WHY A REGION EXISTS AT ALL: the device renderer registers the static
     * world as renderables the engine culls, and a culler needs something to
     * cull. One mesh per (storey, kind, facing) for a whole map has bounds the
     * size of the map, so it is either wholly visible or wholly not — which is
     * to say never not. Chunking is what turns those bounds into something a
     * frustum can reject, and it pays a second time: editing the world
     * re-uploads one chunk instead of every storey.
     *
     * IT IS A CLEAN PARTITION AND THAT IS NOT LUCK. Walls are stored
     * CANONICALLY — the slab between two cells lives on the northern or eastern
     * one's edge, see emitEdges — so every tile emits its own geometry and
     * nobody else's. The union of the regions is therefore exactly the whole
     * storey, with no seam to reconcile and no triangle emitted twice. If that
     * ever stops being true, this splits chunks along shared geometry and the
     * symptom is a flicker at chunk boundaries.
     *
     * NEIGHBOURS ARE STILL READ, which is a different thing from emitted: the
     * outward-facing test and the blocked-mass test both look at the cells
     * around a tile. Those reads are unclipped, so a chunk edge looks the same
     * as it would in a whole-storey emit. */
    void emit(int storey, int minX, int minY, int maxX, int maxY, SurfaceBuffers& out) const;

private:
    void emitFloor(const Tile& tile, int storey, float cellBase,
                   float fx, float fy, SurfaceBuffers& out) const;
    void emitRampTreads(const Tile& tile, int x, int y, float cellBase,
                        float fx, float fy, SurfaceBuffers& out) const;
    bool hasFlightAbove(int x, int y, const Tile& tile) const;
    void emitBlockedMass(int x, int y, int z, float cellBase,
                         float fx, float fy, SurfaceBuffers& out) const;
    void emitCrate(const Tile& tile, float cellBase,
                   float fx, float fy, SurfaceBuffers& out) const;
    void emitCanopy(int z, float fx, float fy, SurfaceBuffers& out) const;
    void emitPortal(const Tile& tile, float cellBase,
                    float fx, float fy, SurfaceBuffers& out) const;
    void emitLadders(const Tile& tile, int x, int y, int z, float cellBase,
                     float fx, float fy, SurfaceBuffers& out) const;
    void emitEdges(const Tile& tile, int x, int y, int z, float cellBase,
                   bool storeyBase, float fx, float fy, SurfaceBuffers& out) const;

    /* A tile whose four faces are ALL half cover reads as a free-standing
     * crate rather than four separate railings. */
    static bool isCrateLike(const Tile* tile);

    /* WHICH WAY THIS WALL FACES AWAY FROM THE ROOM IT ENCLOSES, which is the
     * one question a cutaway needs answered and the one the geometry cannot
     * answer for itself.
     *
     * Walls are stored CANONICALLY — the slab between two cells lives on the
     * northern or eastern one's edge (see emitEdges) — so a slab's Dir gives
     * its axis and nothing more. Whether that north-edge slab is a room's
     * north wall or the next room's south wall depends entirely on which side
     * the enclosed air is, and answering that is outwardWallDirection's job:
     * see RoomPartition.hpp, which also explains why the test is a comparison
     * rather than a classification.
     *
     * All this adds is the lattice's compass to the engine's world axes. In
     * every case the rule cannot decide, the wall stays up — which is the
     * failure worth having, since a wall that fails to open is a nuisance and
     * a wall that vanishes is a hole in the world. */
    SurfaceFacing outwardFacing(int x, int y, int z, Dir d) const;

    const World&  world_;
    BlockedMass   mass_;
    LadderQuery   ladders_;
    RoomPartition rooms_;
};

}  // namespace game
