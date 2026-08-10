/* RestPlacement.hpp — may this unit END its move here?
 *
 * SINGLE RESPONSIBILITY: the end-of-move rule, which is NOT the same as the
 * traversal rule.
 *
 * Infantry may not stop on stairs (pass-through only, which the zero stair
 * cost guarantees is never forced) nor on another body. A hull needs its whole
 * 2x2 anchor to be valid, which its move graph already answers.
 */
#pragma once

#include "core/movement/BlockedMask.hpp"
#include "core/movement/MoveGraph.hpp"
#include "core/units/UnitRoster.hpp"
#include "core/world/World.hpp"

namespace xcom {

class RestPlacement {
public:
    RestPlacement(const World& world, const UnitRoster& roster)
        : world_(world), roster_(roster) {}

    bool canRest(const Unit& unit, const MoveGraph& graph,
                 const BlockedMask& blocked, const Cell& cell) const;

private:
    const World&      world_;
    const UnitRoster& roster_;
};

}  // namespace xcom
