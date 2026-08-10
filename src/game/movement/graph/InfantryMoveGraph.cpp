#include "game/movement/graph/InfantryMoveGraph.hpp"

#include "game/lattice/Constants.hpp"

#include <cmath>

namespace game {

namespace {

constexpr float kEpsilon = 1e-6f;

/* Ramp ends are absolute heights, so a flight meeting a floor or another
 * flight is an EXACT equality. kEpsilon covers float round-trip only; anything
 * needing real slack uses kWalkStep by name. */
constexpr float kExact = 0.0f;

bool nearly(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance + kEpsilon;
}

const int kDiagonalOffsets[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

}  // namespace

InfantryMoveGraph::InfantryMoveGraph(const World& world)
    : world_(world),
      terrain_(world),
      standability_(world),
      ladders_(world),
      columns_(world)
{
}

bool InfantryMoveGraph::isTraversable(const Cell& cell, const BlockedMask* blocked) const
{
    if (!standability_.isStandable(cell)) return false;
    if (blocked && blocked->isBlocked(world_.lattice().index(cell))) return false;
    return true;
}

/* ---- ramp cells: free, pass-through, ends meet by absolute height ------- */
void InfantryMoveGraph::addRampMoves(const Cell& from, const Tile& tile,
                                     std::vector<Move>& out) const
{
    const Lattice& lattice = world_.lattice();
    const Dir   uphill   = tile.rampDir;
    const Dir   downhill = opposite(uphill);
    const float lowH     = tile.rampBaseHeight;
    const float highH    = tile.rampTopHeight();

    for (int end = 0; end < 2; end++) {
        const bool  goingUp = (end != 0);
        const Dir   d       = goingUp ? uphill : downhill;
        const float endH    = goingUp ? highH : lowH;

        const int nx = from.x + dx(d);
        const int ny = from.y + dy(d);
        if (!lattice.inBounds(nx, ny)) continue;

        columns_.flatSurfaces(nx, ny, surfaces_);
        for (const FlatSurface& s : surfaces_) {
            if (nearly(s.height, endH, kWalkStep) &&
                world_.effectiveEdge(from.x, from.y, s.z, d).cover != Cover::Full) {
                out.push_back({ { nx, ny, s.z }, 0.0f,
                                goingUp ? MoveKind::Ramp : MoveKind::Walk, d });
            }
        }

        columns_.ramps(nx, ny, ramps_);
        for (const RampSurface& r : ramps_) {
            if (r.uphill != uphill) continue;
            const bool chains = goingUp ? nearly(r.lowHeight, highH, kExact)
                                        : nearly(r.highHeight, lowH, kExact);
            if (chains) out.push_back({ { nx, ny, r.z }, 0.0f, MoveKind::Ramp, d });
        }
    }

    /* lateral step onto a parallel flight at the same heights */
    for (Dir p : perpendicular(uphill)) {
        const int nx = from.x + dx(p);
        const int ny = from.y + dy(p);
        if (!lattice.inBounds(nx, ny)) continue;

        columns_.ramps(nx, ny, ramps_);
        for (const RampSurface& r : ramps_) {
            if (r.uphill == uphill &&
                nearly(r.lowHeight, lowH, kExact) && nearly(r.highHeight, highH, kExact) &&
                world_.effectiveEdge(from, p).cover != Cover::Full) {
                out.push_back({ { nx, ny, r.z }, 0.0f, MoveKind::Walk, p });
            }
        }
    }
}

/* ---- portal: standing on a linked tile allows a teleport ---------------- */
void InfantryMoveGraph::addPortalMoves(const Cell& from, const Tile& tile,
                                       std::vector<Move>& out) const
{
    const Lattice& lattice = world_.lattice();
    const int self = lattice.index(from);

    for (int i = 0; i < lattice.cellCount(); i++) {
        if (i == self || world_.at(i).portal != tile.portal) continue;
        const Cell target = lattice.cellAt(i);
        if (standability_.isStandable(target))
            out.push_back({ target, kPortalCost, MoveKind::Portal, std::nullopt });
        break;
    }
}

/* ---- the four cardinal faces ------------------------------------------- */
void InfantryMoveGraph::addLateralMoves(const Cell& from, float myHeight,
                                        std::vector<Move>& out) const
{
    const Lattice& lattice = world_.lattice();

    for (Dir d : kAllDirs) {
        const int nx = from.x + dx(d);
        const int ny = from.y + dy(d);
        if (!lattice.inBounds(nx, ny)) continue;

        const Edge edge = world_.effectiveEdge(from, d);

        /* ladder ascent — independent of the height rules (climbs the face) */
        if (edge.ladder) {
            if (const std::optional<LadderHit> hit = ladders_.targetFrom(from.x, from.y, from.z, d)) {
                const float storeys = hit->rise / kStoreyHeight;
                int levels = static_cast<int>(storeys + 0.5f);
                if (levels < 1) levels = 1;
                out.push_back({ hit->cell,
                                1.0f + kLadderCost * static_cast<float>(levels),
                                MoveKind::Ladder, d });
            }
        }

        /* flat surfaces in the neighbour column, classified purely by dh */
        bool  haveDrop   = false;
        int   dropZ      = 0;
        float dropH      = 0.0f;
        bool  dropIsRamp = false;
        int   dropRampZ  = -1;

        columns_.flatSurfaces(nx, ny, surfaces_);
        for (const FlatSurface& s : surfaces_) {
            const float dh = s.height - myHeight;
            /* wall at the TARGET's band, not only at ours */
            const Edge atTarget = world_.effectiveEdge(from.x, from.y, s.z, d);

            if (std::fabs(dh) <= kWalkStep + kEpsilon) {
                if (edge.cover != Cover::Full && atTarget.cover != Cover::Full) {
                    const bool hopping = (edge.cover == Cover::Half);
                    out.push_back({ { nx, ny, s.z },
                                    1.0f + (hopping ? kClimbCost : 0.0f),
                                    hopping ? MoveKind::Climb : MoveKind::Walk, d });
                }
            } else if (dh > 0.0f && dh <= kMantleMax + kEpsilon) {
                if (edge.cover != Cover::Full && atTarget.cover != Cover::Full)
                    out.push_back({ { nx, ny, s.z }, 1.0f, MoveKind::Mantle, d });
            } else if (dh < 0.0f) {
                if (!haveDrop || s.height > dropH) {
                    haveDrop = true; dropH = s.height; dropZ = s.z;
                    dropIsRamp = false; dropRampZ = -1;
                }
            }
        }

        /* ramp entries: the flight's low end at my height (walk in, heading
         * uphill), or its high end at my height (step onto the top) */
        columns_.ramps(nx, ny, ramps_);
        for (const RampSurface& r : ramps_) {
            if (r.uphill == d && nearly(r.lowHeight, myHeight, kWalkStep) &&
                edge.cover != Cover::Full)
                out.push_back({ { nx, ny, r.z }, 0.0f, MoveKind::Walk, d });

            if (r.uphill == opposite(d) && nearly(r.highHeight, myHeight, kWalkStep) &&
                edge.cover != Cover::Full)
                out.push_back({ { nx, ny, r.z }, 0.0f, MoveKind::Ramp, d });

            /* dropping ONTO a flight midway */
            if (r.midHeight - myHeight < -kWalkStep && (!haveDrop || r.midHeight > dropH)) {
                haveDrop = true; dropH = r.midHeight; dropZ = r.z;
                dropIsRamp = true; dropRampZ = r.z;
            }
        }

        /* one-way drop to the highest surface below — but ONLY down an open
         * shaft: the fall corridor (landing, my height] must contain no floor
         * slab, canopy or solid mass. Without this you could "drop" from a
         * roof THROUGH the neighbouring roof tile into the room beneath. */
        if (haveDrop && edge.cover != Cover::Full) {
            bool fallBlocked = false;

            for (const FlatSurface& s : surfaces_)
                if (s.height > dropH + kEpsilon && s.height <= myHeight + kWalkStep + kEpsilon)
                    fallBlocked = true;

            for (const RampSurface& r : ramps_)
                if (r.lowHeight < myHeight + kWalkStep + kEpsilon && r.highHeight > dropH + kEpsilon &&
                    !(dropIsRamp && r.z == dropRampZ))
                    fallBlocked = true;

            for (int zz = 0; zz < lattice.depth(); zz++) {
                const Tile& column = world_.at(lattice.index(nx, ny, zz));
                const float top = Lattice::cellBaseHeight(zz + 1);
                if (column.canopy  && top > dropH + kEpsilon && top < myHeight - kEpsilon) fallBlocked = true;
                if (column.blocked && top > dropH + kEpsilon && top < myHeight - kEpsilon) fallBlocked = true;
            }

            if (!fallBlocked) {
                /* kDropCost is charged per STOREY fallen, not per z cell — a
                 * one-storey drop is three cells now, and the cost must not
                 * have tripled with the lattice. */
                int storeys = static_cast<int>((myHeight - dropH) / kStoreyHeight + 0.5f);
                if (storeys < 1) storeys = 1;
                out.push_back({ { nx, ny, dropZ },
                                1.0f + static_cast<float>(storeys) * kDropCost +
                                    (edge.cover == Cover::Half ? kClimbCost : 0.0f),
                                MoveKind::Drop, d });
            }
        }
    }
}

/* ---- diagonals: both orthogonal "shoulder" tiles must be walkable at my
 * height, and all four corner edges cover-free (no cutting corners) ------- */
void InfantryMoveGraph::addDiagonalMoves(const Cell& from, float myHeight,
                                         std::vector<Move>& out) const
{
    const Lattice& lattice = world_.lattice();

    for (const auto& offset : kDiagonalOffsets) {
        const int ox = offset[0], oy = offset[1];
        const int nx = from.x + ox, ny = from.y + oy;
        if (!lattice.inBounds(nx, ny)) continue;

        auto findLevel = [&](std::vector<FlatSurface>& scratch, int cx, int cy) -> int {
            columns_.flatSurfaces(cx, cy, scratch);
            for (const FlatSurface& s : scratch)
                if (nearly(s.height, myHeight, kWalkStep)) return s.z;
            return -1;
        };

        const int targetZ = findLevel(surfaces_, nx, ny);
        if (targetZ < 0) continue;
        const int shoulderAZ = findLevel(shoulderA_, from.x + ox, from.y);
        if (shoulderAZ < 0) continue;
        const int shoulderBZ = findLevel(shoulderB_, from.x, from.y + oy);
        if (shoulderBZ < 0) continue;

        const Dir dirX = ox > 0 ? Dir::East : Dir::West;
        const Dir dirY = oy > 0 ? Dir::North : Dir::South;

        if (world_.effectiveEdge(from, dirX).cover != Cover::None) continue;
        if (world_.effectiveEdge(from, dirY).cover != Cover::None) continue;
        if (world_.effectiveEdge(from.x + ox, from.y, shoulderAZ, dirY).cover != Cover::None) continue;
        if (world_.effectiveEdge(from.x, from.y + oy, shoulderBZ, dirX).cover != Cover::None) continue;

        out.push_back({ { nx, ny, targetZ }, kDiagonalCost, MoveKind::Diagonal, std::nullopt });
    }
}

void InfantryMoveGraph::neighbors(const Cell& from,
                                  const BlockedMask* /*blocked*/,
                                  std::vector<Move>& out) const
{
    out.clear();

    const Tile* tile = world_.tryAt(from.x, from.y, from.z);
    if (!tile || tile->blocked || (!tile->hasFloor && !tile->isRamp())) return;

    const float myHeight = terrain_.centerHeight(from);

    if (tile->isRamp()) {
        addRampMoves(from, *tile, out);
        return;                      /* ramps: no diagonals, drops or mantles */
    }

    if (tile->portal) addPortalMoves(from, *tile, out);
    addLateralMoves(from, myHeight, out);
    addDiagonalMoves(from, myHeight, out);
}

void InfantryMoveGraph::continuousNeighbors(const Cell& from, std::vector<Move>& out) const
{
    neighbors(from, nullptr, filterScratch_);
    out.clear();
    for (const Move& move : filterScratch_)
        if (isSurfaceFollowing(move.kind)) out.push_back(move);
}

}  // namespace game
