#include "game/state/RingSelector.hpp"

namespace game {


RingMask RingSelector::visibleRings(const ReachField& reach,
                                    std::optional<int> hoveredCell,
                                    float moveBudget) const
{
    if (override_ == 1) return RingMask(Ring::Move);
    if (override_ == 2) return RingMask(Ring::Sprint);
    if (override_ == 3) return RingMask::both();

    if (hoveredCell) {
        const float cost = reach.cost(*hoveredCell);
        if (cost > moveBudget + 1e-6f && cost <= moveBudget * 2.0f + 1e-6f)
            return RingMask::both();
    }
    return RingMask(Ring::Move);
}

Ring RingSelector::solidRing(const ReachField& reach,
                             std::optional<int> hoveredCell,
                             float moveBudget) const
{
    const RingMask visible = visibleRings(reach, hoveredCell, moveBudget);
    /* Amber only ever reaches the screen because the cursor is out past the
     * move budget, so whenever it is up it is the relevant one. */
    return visible.contains(Ring::Sprint) ? Ring::Sprint : Ring::Move;
}

const char* RingSelector::overrideName() const
{
    static const char* kNames[4] = { "auto", "move", "sprint", "both" };
    return kNames[override_];
}

}  // namespace game
