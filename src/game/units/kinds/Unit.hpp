/* Unit.hpp — a body on the board.
 *
 * SINGLE RESPONSIBILITY: hold a body's identity and position, and the
 * components that describe what kind of body it is.
 *
 * NO LONGER ABSTRACT, AND THAT IS THE POINT. Unit used to declare fourteen pure
 * virtuals with Soldier and Vehicle overriding all of them — and ten of those
 * overrides were `return false;` or `return true;`. That is a data table
 * written as a class hierarchy: two classes, four files, a vtable and a
 * double-dispatch visitor, to express what five structs of plain fields express
 * directly.
 *
 * A unit is now COMPOSED:
 *
 *   BodyComponent          shape, sight-blocking, how it sits on terrain
 *   MobilityComponent      which move graph, what driving over cover does
 *   CoverComponent         what it does to the cover system
 *   DestructibleComponent  what it leaves behind
 *   PresentationComponent  what it is called and how it is drawn
 *
 * A new body is a new set of VALUES (see UnitFactory), not a new subclass. A
 * drone that is 1x1 like infantry but blocks sight like a hull is a struct
 * literal; under the hierarchy it was a third class re-answering fourteen
 * questions, most of them with a constant.
 *
 * The accessors below are a FACADE over the components, kept deliberately so
 * that the existing call sites reading `unit.footprint()` did not all have to
 * become `unit.body().footprint`. Reach for the component directly when you
 * want several of its fields; use the facade when you want one.
 */
#pragma once

#include "cromwell/entities/Entity.hpp"
#include "game/lattice/Cell.hpp"
#include "game/units/Team.hpp"
#include "game/components/BodyComponent.hpp"
#include "game/components/CoverComponent.hpp"
#include "game/components/DestructibleComponent.hpp"
#include "game/components/MobilityComponent.hpp"
#include "game/components/PresentationComponent.hpp"

#include <memory>
#include <string>
#include <utility>

namespace game {

using namespace cromwell;

class MoveGraph;
class World;

class Unit : public Entity {
public:
    Unit(Cell position, Team team) : team_(team) { setPosition(position); }

    /* ---- where it is ---------------------------------------------------
     * THE CELL IS THE GAME'S COORDINATE; the XYZ underneath it is the
     * engine's, and both stay true. Setting the cell writes the entity's
     * world location to that cell's centre — height is left to whoever knows
     * the terrain, because the lattice alone cannot say how high a floor sits.
     * Read the XYZ with Entity::location(). */
    const Cell& position() const { return position_; }
    void setPosition(const Cell& cell)
    {
        position_ = cell;
        setLocation(Vec3{ static_cast<float>(cell.x) + 0.5f,
                          location().y,
                          static_cast<float>(cell.y) + 0.5f });
    }

    Team team() const { return team_; }

    /* ---- death ----------------------------------------------------------
     * NOT Entity::destroy(). A killed unit stays on the board — it leaves a
     * wreck, it still occupies its cells for a moment, and the roster still
     * holds it — so this is a state of the unit, not removal from the world.
     * It lives here rather than on Entity because most things in a world
     * cannot be killed at all.
     *
     * Belongs in a HealthComponent the moment there is health to track; today
     * there is only the flag. */
    bool isDead()  const { return dead_; }
    bool isAlive() const { return !dead_; }
    void kill() { dead_ = true; }

    /* ---- the components ------------------------------------------------
     * Named accessors over Entity::component<T>(), for the five this game's
     * factory always attaches. Anything optional should be asked for with
     * findComponent<T>() so its absence is a value rather than a crash. */
    const BodyComponent&         body()         const { return component<BodyComponent>(); }
    const MobilityComponent&     mobility()     const { return component<MobilityComponent>(); }
    const CoverComponent&        cover()        const { return component<CoverComponent>(); }
    const DestructibleComponent& destructible() const { return component<DestructibleComponent>(); }
    const PresentationComponent& presentation() const { return component<PresentationComponent>(); }

    /* ---- facade over them ---------------------------------------------- */
    const Footprint& footprint() const { return body().footprint(); }
    bool  blocksLineOfSight()    const { return body().blocksLineOfSight(); }
    float hullHeight()           const { return body().hullHeight(); }
    float baseHeight(const World& world) const { return body().baseHeightAt(world, position_); }

    bool canRestOnRamp()    const { return mobility().canRestOnRamp(); }
    bool crushesHalfCover() const { return mobility().crushesHalfCover(); }
    /* Out of line: the returned unique_ptr's destructor needs MoveGraph
     * complete, and this header only forward-declares it. */
    std::unique_ptr<MoveGraph> createMoveGraph(const World& world) const;

    bool grantsHullCover()   const { return cover().grantsHullCover(); }
    bool showsCoverShields() const { return cover().showsCoverShields(); }
    bool leavesWreckage()    const { return destructible().leavesWreckage(); }

    const std::string& displayName()          const { return presentation().displayName(); }
    const std::string& hudLabel()             const { return presentation().hudLabel(); }
    const std::string& selectionDescription() const { return presentation().selectionDescription(); }

    /* Does this body cover `cell` when anchored where it is? */
    bool occupies(const Cell& cell) const;

private:
    Cell position_;
    Team team_;
    bool dead_ = false;
};

}  // namespace game
