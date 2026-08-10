/* Move.hpp — one edge of the derived move graph.
 *
 * SINGLE RESPONSIBILITY: describe a single legal step.
 *
 * CONNECTIVITY IS GENERATED FROM HEIGHT DELTAS, not from cell adjacency:
 * |dh| <= kWalkStep walks, <= kMantleMax mantles, lower surfaces are one-way
 * drops — whatever z cell the surfaces happen to be stored in. Ramps connect
 * where their absolute end heights meet a surface, which is exact equality
 * rather than a tolerance.
 */
#pragma once

#include "game/lattice/Cell.hpp"
#include "game/lattice/Direction.hpp"

#include <optional>

namespace game {


enum class MoveKind : int {
    Walk = 0, Climb, Ramp, Drop, Diagonal, Ladder, Portal, Mantle
};

/* Surface-following kinds only. Drops, diagonals, ladders, portals and
 * mantles are transitions between SEPARATE surfaces — which is what gives a
 * roof patch or a portal's far side its own independent border loop. */
inline constexpr bool isSurfaceFollowing(MoveKind kind)
{
    switch (kind) {
        case MoveKind::Drop:
        case MoveKind::Diagonal:
        case MoveKind::Ladder:
        case MoveKind::Portal:
        case MoveKind::Mantle:
            return false;
        default:
            return true;
    }
}

struct Move {
    Cell               target;
    float              cost = 0.0f;
    MoveKind           kind = MoveKind::Walk;
    std::optional<Dir> dir;      /* absent for diagonals and portals */
};

/* A column can offer a surface per cell plus ramps, and every direction can
 * reach several of them, so the fan-out is bounded but not tiny. */
inline constexpr int kMaxMovesPerCell = 64;

}  // namespace game
