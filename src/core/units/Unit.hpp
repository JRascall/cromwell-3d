/* Unit.hpp — the abstract body.
 *
 * SINGLE RESPONSIBILITY: hold a body's identity and position, and declare the
 * questions whose answers differ between kinds of body.
 *
 * Every one of these virtuals replaces a `kind == XC_KIND_TANK` branch that
 * used to be scattered through movement, LOS, cover, picking and rendering.
 *
 * Occupancy rules, XCOM-style:
 *  - ENEMY-occupied cells are walls: no traversal, no stopping.
 *  - FRIENDLY-occupied cells are pass-through: paths may cross them, but no
 *    unit may END a move overlapping another unit's footprint.
 * Multi-tile units also BLOCK LOS (their hull is terrain) and grant full
 * cover to adjacent infantry; 1x1 units are transparent to sight — you shoot
 * past squadmates, as in XCOM.
 */
#pragma once

#include "core/lattice/Cell.hpp"
#include "core/movement/MoveGraph.hpp"
#include "core/units/Footprint.hpp"
#include "core/units/Team.hpp"
#include "core/units/UnitVisitor.hpp"

#include <memory>
#include <string>

namespace xcom {

class World;

class Unit {
public:
    Unit(Cell position, Team team) : position_(position), team_(team) {}
    virtual ~Unit() = default;

    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;

    /* ---- identity and state, common to every body --------------------- */
    const Cell& position() const { return position_; }
    void setPosition(const Cell& cell) { position_ = cell; }

    Team team() const { return team_; }
    bool isDead() const { return dead_; }
    void kill() { dead_ = true; }
    bool isAlive() const { return !dead_; }

    /* ---- what differs by kind of body --------------------------------- */
    virtual const Footprint& footprint() const = 0;

    /* Only BIG units block sight — 1x1 bodies are transparent. */
    virtual bool blocksLineOfSight() const = 0;

    /* How tall the hull stands, for LOS. Meaningless for transparent units. */
    virtual float hullHeight() const = 0;

    /* Infantry may not STOP on stairs (pass-through only, which the zero
     * stair cost guarantees is never forced); vehicles cannot use them at all. */
    virtual bool canRestOnRamp() const = 0;

    /* Where this body's base sits in world units. */
    virtual float baseHeight(const World& world) const = 0;

    /* Does driving over destructible half cover flatten it? */
    virtual bool crushesHalfCover() const = 0;

    /* Should the cover shields be drawn for this body? A hull carries armour,
     * not cover, so shields would misreport it. */
    virtual bool showsCoverShields() const = 0;

    /* Does standing beside this body grant infantry full cover? XCOM treats a
     * big unit as mobile high cover. */
    virtual bool grantsHullCover() const = 0;

    /* Does its death stamp a wreck into the terrain? A wreck becomes half
     * cover, so cover, LOS and pathing all pick it up with no special case. */
    virtual bool leavesWreckage() const = 0;

    /* The graph this body moves on. Returned by value so a unit never has to
     * hold a world reference, and so vehicles get anchor space for free. */
    virtual std::unique_ptr<MoveGraph> createMoveGraph(const World& world) const = 0;

    /* For the HUD and status lines. */
    virtual std::string displayName() const = 0;
    virtual std::string hudLabel() const = 0;
    virtual std::string selectionDescription() const = 0;

    /* Double dispatch, so renderers and path builders can specialise without
     * Unit knowing they exist. See UnitVisitor. */
    virtual void accept(UnitVisitor& visitor) const = 0;

    /* Does this body cover `cell` when anchored where it is? */
    bool occupies(const Cell& cell) const;

private:
    Cell position_;
    Team team_;
    bool dead_ = false;
};

}  // namespace xcom
