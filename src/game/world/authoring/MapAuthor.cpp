#include "game/world/authoring/MapAuthor.hpp"

#include "game/world/ramp/RampValidator.hpp"

#include <iostream>

namespace game {


MapAuthor::MapAuthor(World& world, std::ostream* diagnostics)
    : world_(world), diagnostics_(diagnostics ? diagnostics : &std::cerr)
{
}

/* ----------------------------------------------------------- per cell */
void MapAuthor::setEdge(int x, int y, int z, Dir d, Cover cover,
                        bool destructible, bool window)
{
    Tile* tile = world_.tryAt(x, y, z);
    if (!tile) return;

    Edge& near = tile->edge(d);
    near.cover        = cover;
    near.destructible = destructible;
    near.window       = window;

    /* the same physical face is recorded on the neighbour too */
    if (Tile* neighbour = world_.tryAt(x + dx(d), y + dy(d), z)) {
        Edge& far = neighbour->edge(opposite(d));
        far.cover        = cover;
        far.destructible = destructible;
        far.window       = window;
    }
}

void MapAuthor::clearEdge(int x, int y, int z, Dir d)
{
    setEdge(x, y, z, d, Cover::None, false, false);
}

void MapAuthor::setLadder(int x, int y, int z, Dir d)
{
    Tile* tile = world_.tryAt(x, y, z);
    if (!tile) return;
    tile->edge(d).ladder = true;
    if (Tile* neighbour = world_.tryAt(x + dx(d), y + dy(d), z))
        neighbour->edge(opposite(d)).ladder = true;
}

/* --------------------------------------------------------- per storey */
void MapAuthor::setWall(int x, int y, int storey, Dir d, Cover cover,
                        bool destructible, bool window)
{
    const int baseZ = Lattice::storeyBaseZ(storey);
    const int cells = (cover == Cover::Full) ? kCellsPerStorey : 1;
    for (int i = 0; i < cells; i++)
        setEdge(x, y, baseZ + i, d, cover, destructible, window);
}

void MapAuthor::clearWall(int x, int y, int storey, Dir d)
{
    const int baseZ = Lattice::storeyBaseZ(storey);
    for (int i = 0; i < kCellsPerStorey; i++) clearEdge(x, y, baseZ + i, d);
}

void MapAuthor::setLadderWall(int x, int y, int storey, Dir d)
{
    const int baseZ = Lattice::storeyBaseZ(storey);
    for (int i = 0; i < kCellsPerStorey; i++) setLadder(x, y, baseZ + i, d);
}

void MapAuthor::setSolid(int x, int y, int storey)
{
    const int baseZ = Lattice::storeyBaseZ(storey);
    for (int i = 0; i < kCellsPerStorey; i++)
        if (Tile* tile = world_.tryAt(x, y, baseZ + i)) tile->blocked = true;
}

/* ------------------------------------------------------ floors, ramps */
int MapAuthor::setFloorAt(int x, int y, float height)
{
    float offset = 0.0f;
    const int z = world_.lattice().cellOfHeight(height, &offset);
    Tile* tile = world_.tryAt(x, y, z);
    if (!tile) return -1;
    tile->hasFloor    = true;
    tile->floorOffset = offset;
    return z;
}

void MapAuthor::clearFloorAt(int x, int y, float height)
{
    const int z = world_.lattice().cellOfHeight(height);
    Tile* tile = world_.tryAt(x, y, z);
    if (!tile) return;
    tile->hasFloor    = false;
    tile->floorOffset = 0.0f;
}

int MapAuthor::setRamp(int x, int y, Dir uphill, float baseHeight, float rise)
{
    const RampValidator::Result verdict = RampValidator::validate(x, y, rise);
    if (!verdict) {
        *diagnostics_ << verdict.diagnostic;
        return -1;
    }

    const int z = world_.lattice().cellOfHeight(baseHeight);
    Tile* tile = world_.tryAt(x, y, z);
    if (!tile) {
        *diagnostics_ << "MapAuthor::setRamp(" << x << "," << y << "): baseHeight "
                      << baseHeight << " is outside the grid\n";
        return -1;
    }

    tile->rampDir        = uphill;
    tile->rampBaseHeight = baseHeight;
    tile->rampRise       = rise;
    tile->hasFloor       = false;
    return z;
}

}  // namespace game
