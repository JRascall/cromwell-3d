#include "game/units/roster/UnitRoster.hpp"

namespace game {

void UnitRoster::tick(float deltaSeconds)
{
    for (std::unique_ptr<Unit>& unit : units_) unit->tick(deltaSeconds);
}


void UnitRoster::bindLattice(const Lattice& lattice)
{
    lattice_ = lattice;
    occupancy_.bind(lattice);
    rebuildOccupancy();
}

Unit* UnitRoster::add(std::unique_ptr<Unit> unit)
{
    units_.push_back(std::move(unit));
    Unit* added = units_.back().get();

    /* The back-pointer is what lets a unit keep the index honest as it moves
     * and dies, rather than the roster having to be told. Indices are stable:
     * units are only ever appended, and clear() takes the whole vector. */
    const int bodyId = size() - 1;
    added->attachToRoster(this, bodyId);

    if (occupancy_.isBound() && added->isAlive())
        stamp(*added, added->position(), bodyId);

    return added;
}

void UnitRoster::clear()
{
    units_.clear();
    if (occupancy_.isBound()) occupancy_.clear();
}

int UnitRoster::indexOf(const Unit* unit) const
{
    for (int i = 0; i < size(); i++)
        if (units_[static_cast<std::size_t>(i)].get() == unit) return i;
    return -1;
}

/* ---- the index ---------------------------------------------------------- */

void UnitRoster::stamp(const Unit& unit, const Cell& anchor, int bodyId)
{
    for (const Offset& o : unit.footprint().offsets()) {
        const Cell cell{ anchor.x + o.dx, anchor.y + o.dy, anchor.z };
        if (lattice_.isValid(cell)) occupancy_.place(lattice_.index(cell), bodyId);
    }
}

void UnitRoster::unstamp(const Unit& unit, const Cell& anchor, int bodyId)
{
    for (const Offset& o : unit.footprint().offsets()) {
        const Cell cell{ anchor.x + o.dx, anchor.y + o.dy, anchor.z };
        if (lattice_.isValid(cell)) occupancy_.erase(lattice_.index(cell), bodyId);
    }
}

void UnitRoster::rebuildOccupancy()
{
    occupancy_.clear();
    for (int i = 0; i < size(); i++) {
        const Unit& unit = at(i);
        if (unit.isAlive()) stamp(unit, unit.position(), i);
    }
}

void UnitRoster::onBodyMoved(const Unit& unit, const Cell& from)
{
    if (!occupancy_.isBound() || unit.bodyId_ < 0) return;

    /* Clear THEN write, and the erase is id-matched, so a move whose old and
     * new footprints overlap does not blank the cells it is keeping. */
    unstamp(unit, from, unit.bodyId_);
    if (unit.isAlive()) stamp(unit, unit.position(), unit.bodyId_);
}

void UnitRoster::onBodyKilled(const Unit& unit)
{
    if (!occupancy_.isBound() || unit.bodyId_ < 0) return;

    /* A dead body still stands on the board — it leaves a wreck and the roster
     * keeps it — but occupantAt has always skipped the dead, so the index it
     * backs must skip them too. */
    unstamp(unit, unit.position(), unit.bodyId_);
}

/* ---- queries ------------------------------------------------------------ */

Unit* UnitRoster::scanForOccupant(const Cell& cell, const Unit* exclude) const
{
    for (const std::unique_ptr<Unit>& unit : units_) {
        if (unit->isDead() || unit.get() == exclude) continue;
        if (unit->occupies(cell)) return unit.get();
    }
    return nullptr;
}

Unit* UnitRoster::occupantAt(const Cell& cell, const Unit* exclude)
{
    if (!occupancy_.isBound()) return scanForOccupant(cell, exclude);
    if (!lattice_.isValid(cell)) return nullptr;

    const int bodyId = occupancy_.at(lattice_.index(cell));
    if (bodyId == OccupancyGrid::kEmpty) return nullptr;

    Unit* occupant = units_[static_cast<std::size_t>(bodyId)].get();
    return occupant == exclude ? nullptr : occupant;
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

}  // namespace game
