/* UnitRoster.hpp — the set of bodies on the board.
 *
 * SINGLE RESPONSIBILITY: own the units and answer "who is standing here".
 * It builds no masks and computes no cover — those are separate classes.
 *
 * Units are held by unique_ptr because they are polymorphic; the roster owns
 * them, and every other class borrows raw pointers or references.
 */
#pragma once

#include "game/movement/occupancy/OccupancyGrid.hpp"
#include "game/units/kinds/Unit.hpp"

#include <memory>
#include <vector>

namespace game {


class UnitRoster {
public:
    /* ---- the occupancy index -------------------------------------------
     * Binding a lattice turns "who is standing here" from a walk over every
     * unit into one array read. UNBOUND ROSTERS STILL WORK — they fall back to
     * the scan, which is what a small headless test wants and what every
     * caller got before this existed. Anything holding a World should bind:
     * see GameState's constructor.
     *
     * Rebuilding from scratch, so it is safe to call on a populated roster. */
    void bindLattice(const Lattice& lattice);

    /* Returns a borrowed pointer to the stored unit. */
    Unit* add(std::unique_ptr<Unit> unit);

    void clear();

    int size() const { return static_cast<int>(units_.size()); }

    /* Passes the frame to every unit, which passes it to whichever of its
     * components asked for it — every frame for tick, on their own interval
     * for think. This is the ONLY thing that drives the entity update cycle,
     * so a component that stops updating is either not in this roster or did
     * not say canEverTick/canEverThink. */
    void tick(float deltaSeconds);
    bool empty() const { return units_.empty(); }

    Unit&       at(int index)       { return *units_[static_cast<std::size_t>(index)]; }
    const Unit& at(int index) const { return *units_[static_cast<std::size_t>(index)]; }

    /* -1 when the unit is not in this roster. */
    int indexOf(const Unit* unit) const;

    /* The living occupant of a cell, or nullptr. `exclude` (may be nullptr)
     * is skipped — a unit never blocks itself. */
    Unit*       occupantAt(const Cell& cell, const Unit* exclude = nullptr);
    const Unit* occupantAt(const Cell& cell, const Unit* exclude = nullptr) const;

    /* Only BIG units block sight. Returns nullptr for 1x1 units and empty
     * cells. */
    const Unit* lineOfSightBlockerAt(const Cell& cell) const;

    /* range-for over living and dead alike; callers filter. */
    auto begin() { return units_.begin(); }
    auto end()   { return units_.end(); }
    auto begin() const { return units_.begin(); }
    auto end()   const { return units_.end(); }

private:
    /* Driven by Unit, which calls these as its own state changes — a roster
     * that had to be told separately would drift the first time somebody moved
     * a unit and forgot. Private plus a friend, so the compiler enforces that
     * rather than a comment asking nicely. */
    friend class Unit;

    void onBodyMoved(const Unit& unit, const Cell& from);
    void onBodyKilled(const Unit& unit);

    /* Writes/clears one body's footprint in the grid. */
    void stamp(const Unit& unit, const Cell& anchor, int bodyId);
    void unstamp(const Unit& unit, const Cell& anchor, int bodyId);

    /* Fills the grid from scratch. */
    void rebuildOccupancy();

    /* The scan this class used to be. Kept because an unbound roster is still
     * legal, and because it is the reference the grid is checked against. */
    Unit* scanForOccupant(const Cell& cell, const Unit* exclude) const;

    std::vector<std::unique_ptr<Unit>> units_;

    Lattice       lattice_;
    OccupancyGrid occupancy_;
};

}  // namespace game
