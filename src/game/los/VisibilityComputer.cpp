#include "game/los/VisibilityComputer.hpp"

#include "game/lattice/Constants.hpp"
#include "game/los/RayCaster.hpp"
#include "game/query/Terrain.hpp"

namespace game {


VisibilityComputer::VisibilityComputer(const World& world)
    : world_(world), standability_(world), eyes_(world), roster_(nullptr), viewer_(nullptr)
{
}

VisibilityComputer::VisibilityComputer(const World& world, const UnitRoster& roster,
                                       const Unit* viewer)
    : world_(world), standability_(world), eyes_(world), roster_(&roster), viewer_(viewer)
{
}

void VisibilityComputer::compute(const Cell& from, VisibilityField& out) const
{
    const Lattice& lattice = world_.lattice();
    out.reset(lattice.cellCount());

    const Terrain terrain(world_);
    const std::vector<Eye> eyes = eyes_.eyesFor(from);

    /* one caster for the whole sweep — the hull context is fixed for its life */
    const RayCaster caster = roster_ ? RayCaster(world_, *roster_, viewer_)
                                     : RayCaster(world_);

    for (int z = 0; z < lattice.depth(); z++)
    for (int y = 0; y < lattice.height(); y++)
    for (int x = 0; x < lattice.width(); x++) {
        if (!standability_.isStandable(x, y, z)) continue;

        const float targetHeight = terrain.centerHeight(x, y, z) + kEyeHeight;

        for (const Eye& eye : eyes) {
            if (caster.cast(eye.x, eye.y, eye.height,
                            static_cast<float>(x) + 0.5f,
                            static_cast<float>(y) + 0.5f,
                            targetHeight)) {
                out.set(lattice.index(x, y, z),
                        eye.isPeek ? Visibility::PeekOnly : Visibility::Direct);
                break;                     /* the direct eye is tried first */
            }
        }
    }
}

}  // namespace game
