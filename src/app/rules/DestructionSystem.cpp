#include "app/rules/DestructionSystem.hpp"

#include "core/world/MapAuthor.hpp"
#include "core/world/RampSupport.hpp"

#include <cmath>

namespace xcom {
namespace {

float planarDistance(int ax, int ay, int bx, int by)
{
    const float dx = static_cast<float>(ax - bx);
    const float dy = static_cast<float>(ay - by);
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

int DestructionSystem::destroyTerrain(const Cell& centre)
{
    const Lattice& lattice = world_.lattice();
    MapAuthor author(world_);
    int edits = 0;

    for (int y = centre.y - 2; y <= centre.y + 2; y++)
    for (int x = centre.x - 2; x <= centre.x + 2; x++) {
        if (!lattice.inBounds(x, y)) continue;
        if (planarDistance(x, y, centre.x, centre.y) > kBlastRadius) continue;

        Tile& tile = world_.at(lattice.index(x, y, centre.z));

        for (Dir d : kAllDirs) {
            if (tile.edge(d).cover != Cover::None && tile.edge(d).destructible) {
                author.clearEdge(x, y, centre.z, d);
                edits++;
            }
        }
        if (tile.blocked && tile.blockedDestructible) { tile.blocked = false; edits++; }
        if (tile.hasFloor && tile.floorDestructible) {
            tile.hasFloor = false;
            tile.floorDestructible = false;
            edits++;
        }
        if (tile.isRamp()) { tile.rampRise = 0.0f; edits++; }
        if (tile.canopy)   { tile.canopy = false; edits++; }
    }

    /* structural pass: stairs left without base, chain or landing collapse,
     * and the fixpoint means destroying the bottom cascades up the whole run */
    edits += RampSupport(world_).collapseUnsupported();
    return edits;
}

void DestructionSystem::stampWreck(const Unit& vehicle)
{
    MapAuthor author(world_);
    for (const Cell& cell : vehicle.footprint().cellsAt(vehicle.position())) {
        for (Dir d : kAllDirs)
            if (world_.effectiveEdge(cell, d).cover == Cover::None)
                author.setEdge(cell.x, cell.y, cell.z, d, Cover::Half, true, false);
    }
}

int DestructionSystem::killUnits(const Cell& centre)
{
    int killed = 0;

    for (const std::unique_ptr<Unit>& unit : roster_) {
        if (unit->isDead() || unit->position().z != centre.z) continue;

        bool hit = false;
        for (const Cell& cell : unit->footprint().cellsAt(unit->position()))
            if (planarDistance(cell.x, cell.y, centre.x, centre.y) <= kBlastRadius) hit = true;
        if (!hit) continue;

        if (unit->leavesWreckage()) {
            unit->kill();
            killed++;
            stampWreck(*unit);
        } else if (unit->team() == Team::Enemy) {
            unit->kill();
            killed++;
        }
    }
    return killed;
}

BlastReport DestructionSystem::detonate(const Cell& centre)
{
    BlastReport report;
    report.dataEdits   = destroyTerrain(centre);
    report.unitsKilled = killUnits(centre);
    return report;
}

}  // namespace xcom
