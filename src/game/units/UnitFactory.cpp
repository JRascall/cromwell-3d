#include "game/units/UnitFactory.hpp"

#include "game/lattice/Constants.hpp"

namespace game {

std::unique_ptr<Unit> makeSoldier(Cell position, Team team)
{
    auto unit = std::make_unique<Unit>(position, team);

    unit->addComponent<BodyComponent>()
        .withFootprint(Footprint::single())
        .withBlocksLineOfSight(false)   /* you shoot past squadmates, as in XCOM */
        .withHullHeight(0.0f)
        .withBaseHeightMode(BaseHeightMode::TerrainCentre)
        .withPickHeights(0.25f, 0.75f);

    unit->addComponent<MobilityComponent>()
        .withGraph(MoveGraphKind::Infantry)
        .withPathStyle(PathStyle::Articulated)
        .withCanRestOnRamp(false)   /* stairs are pass-through only */
        .withCrushesHalfCover(false);

    unit->addComponent<CoverComponent>()
        .withGrantsHullCover(false)
        .withShowsCoverShields(true);

    unit->addComponent<DestructibleComponent>().withLeavesWreckage(false);

    unit->addComponent<PresentationComponent>()
        .withVisual(VisualKind::Infantry)
        .withNames("soldier", "soldier",
                   "selected the soldier");

    return unit;
}

std::unique_ptr<Unit> makeVehicle(Cell position, Team team)
{
    auto unit = std::make_unique<Unit>(position, team);

    unit->addComponent<BodyComponent>()
        .withFootprint(Footprint::square2x2())
        .withBlocksLineOfSight(true)    /* the hull is terrain, for sight */
        .withHullHeight(kVehicleLosHeight)
        .withBaseHeightMode(BaseHeightMode::HighestUnderFootprint)
        .withPickHeights(0.1f, 1.9f);

    unit->addComponent<MobilityComponent>()
        .withGraph(MoveGraphKind::Vehicle)
        .withPathStyle(PathStyle::Anchored)
        .withCanRestOnRamp(false)   /* cannot use stairs at all */
        .withCrushesHalfCover(true);

    unit->addComponent<CoverComponent>()
        .withGrantsHullCover(true)      /* XCOM treats a big unit as mobile high cover */
        .withShowsCoverShields(false);  /* armour, not cover */

    unit->addComponent<DestructibleComponent>().withLeavesWreckage(true);

    unit->addComponent<PresentationComponent>()
        .withVisual(VisualKind::Vehicle)
        .withNames("tank", "TANK 2x2",
                   "selected the tank (2x2, crushes cover)");

    return unit;
}

}  // namespace game
