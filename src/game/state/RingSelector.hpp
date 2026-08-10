/* RingSelector.hpp — which movement rings are up, and which one is solid.
 *
 * SINGLE RESPONSIBILITY: turn the cursor's position in the cost field into a
 * ring mask and a solid ring.
 *
 * Blue is always up; the amber sprint ring joins it only while the cursor is
 * over a tile costing more than one move. Stepping into the sprint band does
 * not take blue away — it demotes it to a dashed outline.
 */
#pragma once

#include "game/movement/search/ReachField.hpp"
#include "cromwell/ribbon/Ring.hpp"

#include <optional>
#include <string>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

class RingSelector {
public:
    /* auto -> move -> sprint -> both, the TAB debug cycle */
    void cycleOverride() { override_ = (override_ + 1) % 4; }
    int  overrideIndex() const { return override_; }
    void forceBothRings() { override_ = 3; }

    RingMask visibleRings(const ReachField& reach,
                          std::optional<int> hoveredCell,
                          float moveBudget) const;

    /* The ring the cursor is inside draws solid; every other ring dashes. */
    Ring solidRing(const ReachField& reach,
                   std::optional<int> hoveredCell,
                   float moveBudget) const;

    const char* overrideName() const;

private:
    int override_ = 0;   /* 0 = auto (hover-driven), else a RingMask raw value */
};

}  // namespace game
