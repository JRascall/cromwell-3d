#include "core/world/World.hpp"

namespace xcom {

World::World(const Lattice& lattice)
    : lattice_(lattice),
      tiles_(static_cast<std::size_t>(lattice.cellCount()))
{
}

void World::clear()
{
    tiles_.assign(tiles_.size(), Tile{});
}

Tile* World::tryAt(int x, int y, int z)
{
    if (!lattice_.isValid(x, y, z)) return nullptr;
    return &at(lattice_.index(x, y, z));
}

const Tile* World::tryAt(int x, int y, int z) const
{
    if (!lattice_.isValid(x, y, z)) return nullptr;
    return &at(lattice_.index(x, y, z));
}

Edge World::effectiveEdge(int x, int y, int z, Dir d) const
{
    const Tile* near = tryAt(x, y, z);
    const Tile* far  = tryAt(x + dx(d), y + dy(d), z);

    const Edge nearEdge = near ? near->edge(d) : Edge{};
    const Edge farEdge  = far  ? far->edge(opposite(d)) : Edge{};
    return Edge::combine(nearEdge, farEdge);
}

}  // namespace xcom
