/* UnitRoster.hpp — the set of bodies on the board.
 *
 * SINGLE RESPONSIBILITY: own the units and answer "who is standing here".
 * It builds no masks and computes no cover — those are separate classes.
 *
 * Units are held by unique_ptr because they are polymorphic; the roster owns
 * them, and every other class borrows raw pointers or references.
 */
#pragma once

#include "core/units/Unit.hpp"

#include <memory>
#include <vector>

namespace xcom {

class UnitRoster {
public:
    /* Returns a borrowed pointer to the stored unit. */
    Unit* add(std::unique_ptr<Unit> unit);

    void clear() { units_.clear(); }

    int size() const { return static_cast<int>(units_.size()); }
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
    std::vector<std::unique_ptr<Unit>> units_;
};

}  // namespace xcom
