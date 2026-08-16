#include "game/render/scene/StoreyGeometryEmitter.hpp"

#include "game/render/Palette.hpp"
#include "cromwell/geometry/BoxEmitter.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

bool StoreyGeometryEmitter::isCrateLike(const Tile* tile)
{
    if (!tile) return false;
    for (Dir d : kAllDirs)
        if (tile->edge(d).cover != Cover::Half) return false;
    return true;
}

/* The slab's TOP is the tile plane minus artDrop, and it is thickened by the
 * same amount so a dropped tile still overlaps its neighbours' undersides —
 * that overlap is what forms the kerb face instead of leaving a see-through
 * gap. Nothing above the plane, ever: the border rides 4uu over the plane and
 * is therefore over the art by construction. */
void StoreyGeometryEmitter::emitFloor(const Tile& tile, int storey, float cellBase,
                                      float fx, float fy, SurfaceBuffers& out) const
{
    /* The three storey tints are one material and three vertex colours — see
     * SurfaceKind. */
    const SurfaceKind kind = (tile.artTag == Art::Road)  ? SurfaceKind::Road
                           : (tile.artTag == Art::Grass) ? SurfaceKind::Grass
                                                         : SurfaceKind::Floor;
    const Color colour = (tile.artTag == Art::Road)  ? palette::kRoad
                       : (tile.artTag == Art::Grass) ? palette::kGrass
                                                     : palette::kFloor[static_cast<std::size_t>(storey)];

    const float top       = cellBase + tile.floorOffset - tile.artDrop;
    const float thickness = 0.07f + tile.artDrop;
    emitBox(out[kind], fx, top - thickness * 0.5f, fy, 0.995f, thickness, 0.995f, colour);
}

/* Is this flight continued by another one starting where ours stops? Only the
 * TOP flight of a run has to close its own last riser, so the run has to be
 * followed one tile uphill to find out. The flight above may own any z, hence
 * the column scan — same test RampSupport uses to chain flights. */
bool StoreyGeometryEmitter::hasFlightAbove(int x, int y, const Tile& tile) const
{
    const int ax = x + dx(tile.rampDir);
    const int ay = y + dy(tile.rampDir);

    for (int z = 0; z < world_.lattice().depth(); z++) {
        const Tile* ahead = world_.tryAt(ax, ay, z);
        if (ahead && ahead->isRamp() && ahead->rampDir == tile.rampDir &&
            std::fabs(ahead->rampBaseHeight - tile.rampTopHeight()) < 1e-4f)
            return true;
    }
    return false;
}

/* Ramps: DECORATIVE treads. A stair's pitch line touches the NOSINGS, so a
 * tread's top is the plane height at its FRONT (low) edge — with the back edge
 * the plane runs below every tread, the treads bury the border ribbon, and the
 * depth fade chops it into dots. The tile DATA is the plane; these are art.
 *
 * A tread's BOTTOM is a RISER, not the cell base. Resting them on cellBase
 * breaks a multi-tile run wherever a flight's base lands on a z-cell boundary:
 * the first tread is then zero-high, and the riser under it — a sixth of the
 * rise, the full width of the tile — is simply absent, a slot you can see the
 * skybox through. Reaching one riser below its own top puts every tread on the
 * one before it, whether that one belongs to this tile or to the flight below,
 * and costs nothing where the old bottom was already low enough. */
void StoreyGeometryEmitter::emitRampTreads(const Tile& tile, int x, int y, float cellBase,
                                           float fx, float fy, SurfaceBuffers& out) const
{
    MeshVertexBuffer& buffer = out[SurfaceKind::Ramp];
    const Dir   uphill     = tile.rampDir;
    const float riser      = tile.rampRise / kRampArtSteps;
    const float treadWidth = 1.0f / kRampArtSteps;
    const bool  northSouth = (uphill == Dir::North || uphill == Dir::South);

    /* `along` runs 0 at the downhill edge to 1 at the uphill edge. */
    const auto slab = [&](float along, float bottom, float top) {
        float px = fx, py = fy;
        if (uphill == Dir::North) py = static_cast<float>(y) + along;
        if (uphill == Dir::South) py = static_cast<float>(y) + 1.0f - along;
        if (uphill == Dir::East)  px = static_cast<float>(x) + along;
        if (uphill == Dir::West)  px = static_cast<float>(x) + 1.0f - along;

        const float height = top - bottom;
        if (northSouth)
            emitBox(buffer, px, bottom + height * 0.5f, py,
                    0.98f, height, treadWidth, palette::kRamp);
        else
            emitBox(buffer, px, bottom + height * 0.5f, py,
                    treadWidth, height, 0.98f, palette::kRamp);
    };

    for (int step = 0; step < kRampArtSteps; step++) {
        const float topHeight =
            tile.rampBaseHeight + (static_cast<float>(step) / kRampArtSteps) * tile.rampRise;
        const float bottom = (cellBase < topHeight - riser) ? cellBase : topHeight - riser;
        slab((static_cast<float>(step) + 0.5f) / kRampArtSteps, bottom, topHeight);
    }

    /* The last tread stops one riser below the landing, as a real stair does —
     * but a landing is a 0.07-thick slab, so on the top flight that riser has
     * nothing behind it. Close it from under the landing's lip, where its top
     * is buried by the slab instead of standing proud of the tile plane; a
     * riser inside our OWN last sliver would rise above the pitch line there
     * and bury the border ribbon. */
    if (!hasFlightAbove(x, y, tile)) {
        const float top = tile.rampTopHeight();
        slab(1.0f + treadWidth * 0.5f, top - riser, top - 0.005f);
    }
}

void StoreyGeometryEmitter::emitBlockedMass(int x, int y, int z, float cellBase,
                                            float fx, float fy, SurfaceBuffers& out) const
{
    const std::optional<float> top = mass_.topHeight(x, y, z);
    if (!top) return;

    float height = *top - cellBase - 0.01f;
    if (height < 0.05f) height = 0.05f;
    emitBox(out[SurfaceKind::Block], fx, cellBase + height * 0.5f, fy,
            0.99f, height, 0.99f, palette::kBlock);
}

void StoreyGeometryEmitter::emitCrate(const Tile& tile, float cellBase,
                                      float fx, float fy, SurfaceBuffers& out) const
{
    emitBox(out[SurfaceKind::Cover], fx, cellBase + tile.floorOffset - tile.artDrop + 0.22f, fy,
            0.72f, 0.44f, 0.72f, palette::kHalf);
}

void StoreyGeometryEmitter::emitCanopy(int z, float fx, float fy, SurfaceBuffers& out) const
{
    emitBox(out[SurfaceKind::Canopy], fx, Lattice::cellBaseHeight(z + 1) - 0.025f, fy,
            0.995f, 0.05f, 0.995f, palette::kCanopy);
}

void StoreyGeometryEmitter::emitPortal(const Tile& tile, float cellBase,
                                       float fx, float fy, SurfaceBuffers& out) const
{
    emitBox(out[SurfaceKind::Portal], fx, cellBase + tile.floorOffset - tile.artDrop + 0.05f, fy,
            0.62f, 0.03f, 0.62f, palette::kPortal);
}

/* Ladders — climbable face; base and landing height are DERIVED, never
 * authored. Drawn once per storey: setLadderWall mirrors the flag onto every
 * cell of the run, so without the storey-base gate the rails would be built
 * three times over. */
void StoreyGeometryEmitter::emitLadders(const Tile& tile, int x, int y, int z, float cellBase,
                                        float fx, float fy, SurfaceBuffers& out) const
{
    MeshVertexBuffer& buffer = out[SurfaceKind::Ladder];

    for (Dir d : kAllDirs) {
        /* canonical side only: N/E always, S/W at the grid border */
        if ((d == Dir::South && y != 0) || (d == Dir::West && x != 0)) continue;
        if (!tile.edge(d).ladder) continue;

        const int nx = x + dx(d);
        const int ny = y + dy(d);

        float sideSign = -1.0f;                          /* hangs toward the climber */
        std::optional<LadderHit> hit = ladders_.targetFrom(x, y, z, d);
        if (!hit) {
            hit = ladders_.targetFrom(nx, ny, z, opposite(d));
            if (!hit) continue;
            sideSign = 1.0f;
        }

        const float span = Lattice::cellBaseHeight(hit->cell.z) - cellBase;
        if (span <= 0.0f) continue;

        const float edgeX = (d == Dir::East) ? static_cast<float>(x) + 1.0f
                          : (d == Dir::West) ? static_cast<float>(x) : fx;
        const float edgeY = (d == Dir::North) ? static_cast<float>(y) + 1.0f
                          : (d == Dir::South) ? static_cast<float>(y) : fy;
        const float offsetX = static_cast<float>(dx(d)) * 0.07f * sideSign;
        const float offsetY = static_cast<float>(dy(d)) * 0.07f * sideSign;

        const bool  northSouth = (d == Dir::North || d == Dir::South);
        const float alongX = northSouth ? 1.0f : 0.0f;
        const float alongY = northSouth ? 0.0f : 1.0f;

        for (int rail = 0; rail < 2; rail++) {
            const float offset = (rail == 0) ? -0.27f : 0.27f;
            emitBox(buffer,
                    edgeX + offsetX + alongX * offset,
                    cellBase + (span + 0.25f) * 0.5f,
                    edgeY + offsetY + alongY * offset,
                    alongX != 0.0f ? 0.06f : 0.05f,
                    span + 0.25f,
                    alongY != 0.0f ? 0.06f : 0.05f,
                    palette::kLadder);
        }
        for (float rung = cellBase + 0.22f; rung < cellBase + span + 0.1f; rung += 0.24f) {
            emitBox(buffer, edgeX + offsetX, rung, edgeY + offsetY,
                    alongX != 0.0f ? 0.6f : 0.05f, 0.05f,
                    alongY != 0.0f ? 0.6f : 0.05f,
                    palette::kLadder);
        }
    }
}

/* The lattice's compass mapped onto the engine's world axes. A cell's y is a
 * world z — that conversion lives on this side of the fence because cromwell
 * has no opinion about which way north is. */
namespace {

/* HOW MUCH OF A CUT WALL IS LEFT STANDING.
 *
 * A wall that vanishes entirely takes its floor plan with it: the room reads
 * as an open platform and you lose where the doorways were, which is exactly
 * the information the cutaway was opened to look at. XCOM leaves a low kerb for
 * this reason, and it costs nothing here because the stub is simply emitted
 * into the uncuttable bucket — the cut then removes the wall ABOVE it rather
 * than the wall.
 *
 * THE NUMBER IS BOUNDED FROM ABOVE BY GAMEPLAY, NOT BY TASTE. Half cover
 * stands at 0.42 (see the half branch below), and a kerb approaching that
 * height would read as cover a unit could take — a stub is scenery and cover is
 * a promise, so they must not be confusable at a glance. 0.18 is comfortably
 * under half that: about 17uu on this lattice's 96uu tile, a kerb rather than a
 * parapet. Raise it and check it against a sandbag line before keeping it. */
constexpr float kCutStubHeight = 0.18f;

SurfaceFacing facingOfDir(Dir d)
{
    switch (d) {
        case Dir::North: return SurfaceFacing::PlusZ;
        case Dir::South: return SurfaceFacing::MinusZ;
        case Dir::East:  return SurfaceFacing::PlusX;
        case Dir::West:  return SurfaceFacing::MinusX;
    }
    return SurfaceFacing::None;
}

}  // namespace

SurfaceFacing StoreyGeometryEmitter::outwardFacing(int x, int y, int z, Dir d) const
{
    const std::optional<Dir> outward = outwardWallDirection(rooms_, x, y, z, d);
    return outward ? facingOfDir(*outward) : SurfaceFacing::None;
}

void StoreyGeometryEmitter::emitEdges(const Tile& tile, int x, int y, int z, float cellBase,
                                      bool storeyBase, float fx, float fy,
                                      SurfaceBuffers& out) const
{
    for (Dir d : kAllDirs) {
        /* canonical side only: N/E always, S/W at the grid border */
        if ((d == Dir::South && y != 0) || (d == Dir::West && x != 0)) continue;

        const Edge& edge = tile.edge(d);
        if (edge.cover == Cover::None) continue;

        const Tile* neighbour = world_.tryAt(x + dx(d), y + dy(d), z);
        const bool  half = (edge.cover == Cover::Half);
        if (half && (isCrateLike(&tile) || isCrateLike(neighbour))) continue;

        const float length    = half ? 0.9f : 1.04f;
        const float thickness = half ? 0.16f : 0.09f;

        const float edgeX = (d == Dir::East) ? static_cast<float>(x) + 1.0f
                          : (d == Dir::West) ? static_cast<float>(x) : fx;
        const float edgeY = (d == Dir::North) ? static_cast<float>(y) + 1.0f
                          : (d == Dir::South) ? static_cast<float>(y) : fy;
        const bool northSouth = (d == Dir::North || d == Dir::South);

        float baseOffset = tile.floorOffset - tile.artDrop;
        if (neighbour) {
            const float neighbourBase = neighbour->floorOffset - neighbour->artDrop;
            if (neighbourBase < baseOffset) baseOffset = neighbourBase;
        }

        /* Which bucket this edge's full-height geometry goes in. Computed once
         * per edge rather than per box, so a window's parapet, glass and
         * header cannot land in different buckets and be cut apart. */
        const SurfaceFacing facing = outwardFacing(x, y, z, d);

        const auto wallBox = [&](float height, float bottom, SurfaceKind kind,
                                 Color colour, float thick, SurfaceFacing bucket) {
            if (height <= 0.001f) return;
            if (northSouth)
                emitBox(out(kind, bucket), edgeX, cellBase + bottom + height * 0.5f, edgeY,
                        length, height, thick, colour);
            else
                emitBox(out(kind, bucket), edgeX, cellBase + bottom + height * 0.5f, edgeY,
                        thick, height, length, colour);
        };

        /* The same wall, split so a cut leaves its kerb behind — see
         * kCutStubHeight. Two boxes rather than one, and only for the piece
         * that sits on the ground: a full wall is emitted as one slice per
         * cell, so stubbing every slice would leave a ledge floating at each
         * cell boundary up the height of the building. Walls that face nowhere
         * are never cut and are emitted whole, which is also what keeps the
         * interior partitions from growing a seam they would never show. */
        const auto wallWithKerb = [&](float height, float bottom, SurfaceKind kind,
                                      Color colour, float thick) {
            if (facing == SurfaceFacing::None || !storeyBase) {
                wallBox(height, bottom, kind, colour, thick, facing);
                return;
            }

            const float kerb = std::min(kCutStubHeight, height);
            wallBox(kerb, bottom, kind, colour, thick, SurfaceFacing::None);
            wallBox(height - kerb, bottom + kerb, kind, colour, thick, facing);
        };

        if (edge.window && !half) {
            /* parapet, glass band, header — matches the LOS rule, which passes
             * rays only in [sill, head] OF THE STOREY. Built once, on the
             * storey's base cell. */
            if (!storeyBase) continue;
            const float sill  = kWindowSill * kStoreyHeight;
            const float glass = (kWindowHead - kWindowSill) * kStoreyHeight;
            const float head  = (1.0f - kWindowHead) * kStoreyHeight;
            /* THE GLASS STAYS THINNER THAN THE WALL, and must.
             *
             * Making it flush was tried, to remove a 0.018-unit reveal finer
             * than a shadow texel. It removed nothing — the flat geometry view
             * showed the pane covering its opening either way — and it broke
             * something: at the full thickness the pane's side faces land
             * coplanar with the perpendicular wall's, because `length` is 1.04
             * and adjacent edges therefore overlap at every corner. Coplanar
             * faces z-fight. Being inset is what keeps the pane clear of its
             * neighbours' surfaces. */
            /* Only the parapet grows a kerb — it is the piece standing on the
             * floor. The glass and the header are cut whole. */
            wallWithKerb(sill, baseOffset, SurfaceKind::Wall, palette::kWall, thickness);
            wallBox(glass, baseOffset + sill,         SurfaceKind::Window, palette::kWindow, thickness * 0.6f, facing);
            wallBox(head,  baseOffset + sill + glass, SurfaceKind::Wall,   palette::kWall,   thickness,        facing);
        } else if (half) {
            /* HALF COVER IS NEVER CUT. A sandbag line is waist high — it hides
             * nothing from a camera looking down at it, so removing it would
             * buy no visibility at all, and it would cost the player the one
             * piece of tactical information most worth reading off the board.
             * It goes in the uncuttable bucket deliberately, not by omission. */
            wallBox(0.42f - baseOffset, baseOffset, SurfaceKind::Cover, palette::kHalf, thickness,
                    SurfaceFacing::None);
        } else {
            /* full walls: ONE kCellHeight slice per cell — three stacked reach
             * the ceiling, matching the three edge records setWall wrote */
            const float bottom = storeyBase ? baseOffset : 0.0f;
            wallWithKerb(kCellHeight - bottom, bottom, SurfaceKind::Wall, palette::kWall, thickness);
        }
    }
}

void StoreyGeometryEmitter::emit(int storey, SurfaceBuffers& out) const
{
    const Lattice& lattice = world_.lattice();
    emit(storey, 0, 0, lattice.width(), lattice.height(), out);
}

void StoreyGeometryEmitter::emit(int storey, int minX, int minY, int maxX, int maxY,
                                 SurfaceBuffers& out) const
{
    const Lattice& lattice = world_.lattice();

    /* CLAMPED RATHER THAN ASSERTED, because the caller is a chunk grid whose
     * last chunk on each axis legitimately overhangs a map that is not a whole
     * number of chunks wide. Making that the caller's problem would put the
     * same min() in every caller. */
    const int x0 = std::max(minX, 0);
    const int y0 = std::max(minY, 0);
    const int x1 = std::min(maxX, lattice.width());
    const int y1 = std::min(maxY, lattice.height());

    for (int i = 0; i < kCellsPerStorey; i++) {
        const int   z = Lattice::storeyBaseZ(storey) + i;
        const bool  storeyBase = (i == 0);
        const float cellBase = Lattice::cellBaseHeight(z);

        for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const Tile& tile = world_.at(lattice.index(x, y, z));
            const float fx = static_cast<float>(x) + 0.5f;
            const float fy = static_cast<float>(y) + 0.5f;

            if (tile.hasFloor && !tile.blocked) emitFloor(tile, storey, cellBase, fx, fy, out);
            if (tile.isRamp())  emitRampTreads(tile, x, y, cellBase, fx, fy, out);
            if (tile.blocked)   emitBlockedMass(x, y, z, cellBase, fx, fy, out);
            if (!tile.blocked && isCrateLike(&tile)) emitCrate(tile, cellBase, fx, fy, out);
            if (tile.canopy)    emitCanopy(z, fx, fy, out);
            if (tile.portal && !tile.blocked && tile.hasFloor)
                emitPortal(tile, cellBase, fx, fy, out);

            if (storeyBase) emitLadders(tile, x, y, z, cellBase, fx, fy, out);
            emitEdges(tile, x, y, z, cellBase, storeyBase, fx, fy, out);
        }
    }
}

}  // namespace game
