/* GameState.hpp — the simulation's live state.
 *
 * SINGLE RESPONSIBILITY: hold the world, the roster and the selection, and
 * recompute the derived fields (reachability, visibility) when they change.
 *
 * It owns nothing that touches the GPU. Application turns this state into
 * pictures; GameState never knows it was drawn.
 */
#pragma once

#include "game/border/band/Band.hpp"
#include "game/border/band/BandBuilder.hpp"
#include "game/los/VisibilityField.hpp"
#include "game/movement/occupancy/BlockedMask.hpp"
#include "game/movement/search/Pathfinder.hpp"
#include "game/movement/search/PathPoint.hpp"
#include "game/movement/search/ReachField.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

#include <memory>
#include <vector>

namespace game {

/* WHERE THE CUTAWAY'S STOREY COMES FROM.
 *
 * Dynamic is the playing mode and the default: the cut follows the selected
 * unit's storey, so selecting a soldier on the first floor opens the first
 * floor without the player asking. That is XCOM's behaviour, and it is right
 * for playing because the storey you care about is always the one your unit
 * is on.
 *
 * Manual is the INSPECTING mode, and it exists because Dynamic cannot express
 * "show me the roof while my squad is in the basement". Pressing a storey key
 * is an explicit statement that the automatic answer is not the one wanted, so
 * it latches until the player hands control back.
 *
 * THE MODE DECIDES WHO WRITES isoLevel_, NOT WHAT IT MEANS. Everything
 * downstream — picking, the overlays, the renderer — reads the same field
 * whichever mode is live, so nothing outside Application has to know a mode
 * exists. */
enum class CutawayMode { Dynamic, Manual };

class GameState {
public:
    GameState();

    /* Rebuilds the demo map and roster from scratch. */
    void reset();

    World&            world()       { return world_; }
    const World&      world() const { return world_; }
    UnitRoster&       roster()       { return roster_; }
    const UnitRoster& roster() const { return roster_; }

    Unit&       selectedUnit()       { return roster_.at(selectedIndex_); }
    const Unit& selectedUnit() const { return roster_.at(selectedIndex_); }
    int  selectedIndex() const { return selectedIndex_; }
    void selectIndex(int index);
    void selectUnit(const Unit* unit);

    float moveBudget() const { return moveBudget_; }
    void  setMoveBudget(float budget) { moveBudget_ = budget; }
    float sprintBudget() const { return moveBudget_ * 2.0f; }

    int  isoLevel() const { return isoLevel_; }
    void setIsoLevel(int level) { isoLevel_ = level; }

    CutawayMode cutawayMode() const { return cutawayMode_; }
    void setCutawayMode(CutawayMode mode) { cutawayMode_ = mode; }

    bool losMode() const { return losMode_; }
    void setLosMode(bool on) { losMode_ = on; }

    const ReachField&      reach() const { return reach_; }
    const BlockedMask&     blockedMask() const { return blockedMask_; }
    const VisibilityField& visibility() const { return visibility_; }
    const MoveGraph&       moveGraph() const { return *moveGraph_; }

    /* Rebuilds the blocked mask, the reachability field and (when the LOS
     * overlay is on) the visibility field for the selected unit. */
    void recompute();

    /* The displayed band for a cost cap — anchors expanded to footprints. */
    void buildBand(float costCap, Band& out) const;

    /* Where a hover plate should sit above this cell for the selected unit. */
    float hoverPlateHeight(const Cell& cell) const;

private:
    World      world_;
    UnitRoster roster_;

    std::unique_ptr<MoveGraph> moveGraph_;
    Pathfinder                 pathfinder_;
    BandBuilder                bandBuilder_;

    ReachField      reach_;
    BlockedMask     blockedMask_;
    VisibilityField visibility_;

    int   selectedIndex_ = 0;
    float moveBudget_ = 6.0f;
    int   isoLevel_ = kDefaultStoreyCount - 1;
    CutawayMode cutawayMode_ = CutawayMode::Dynamic;
    bool  losMode_ = false;
};

}  // namespace game
