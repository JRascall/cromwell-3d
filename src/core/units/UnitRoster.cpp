#include "core/units/UnitRoster.hpp"

namespace xcom {

Unit* UnitRoster::add(std::unique_ptr<Unit> unit)
{
    units_.push_back(std::move(unit));
    return units_.back().get();
}

int UnitRoster::indexOf(const Unit* unit) const
{
    for (int i = 0; i < size(); i++)
        if (units_[static_cast<std::size_t>(i)].get() == unit) return i;
    return -1;
}

Unit* UnitRoster::occupantAt(const Cell& cell, const Unit* exclude)
{
    for (const std::unique_ptr<Unit>& unit : units_) {
        if (unit->isDead() || unit.get() == exclude) continue;
        if (unit->occupies(cell)) return unit.get();
    }
    return nullptr;
}

const Unit* UnitRoster::occupantAt(const Cell& cell, const Unit* exclude) const
{
    return const_cast<UnitRoster*>(this)->occupantAt(cell, exclude);
}

const Unit* UnitRoster::lineOfSightBlockerAt(const Cell& cell) const
{
    const Unit* occupant = occupantAt(cell);
    return (occupant && occupant->blocksLineOfSight()) ? occupant : nullptr;
}

}  // namespace xcom
