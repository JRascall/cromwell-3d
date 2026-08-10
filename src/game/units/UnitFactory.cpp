#include "game/units/UnitFactory.hpp"

#include "game/lattice/Constants.hpp"

namespace game {

std::unique_ptr<Unit> makeSoldier(Cell position, Team team)
{
    auto unit = std::make_unique<Unit>(position, team);

    BodyComponent& body    = unit->addComponent<BodyComponent>();
    body.setFootprint(Footprint::single());
    body.setBlocksLineOfSight(false);   /* you shoot past squadmates, as in XCOM */
    body.setHullHeight(0.0f);
    body.setBaseHeightMode(BaseHeightMode::TerrainCentre);
    body.setPickHeights(0.25f, 0.75f);

    MobilityComponent& mobility = unit->addComponent<MobilityComponent>();
    mobility.setGraph(MoveGraphKind::Infantry);
    mobility.setPathStyle(PathStyle::Articulated);
    mobility.setCanRestOnRamp(false);   /* stairs are pass-through only */
    mobility.setCrushesHalfCover(false);

    CoverComponent& cover   = unit->addComponent<CoverComponent>();
    cover.setGrantsHullCover(false);
    cover.setShowsCoverShields(true);

    unit->addComponent<DestructibleComponent>().setLeavesWreckage(false);

    PresentationComponent& presentation = unit->addComponent<PresentationComponent>();
    presentation.setVisual(VisualKind::Infantry);
    presentation.setNames("soldier", "soldier",
                          "selected the soldier");

    return unit;
}

std::unique_ptr<Unit> makeVehicle(Cell position, Team team)
{
    auto unit = std::make_unique<Unit>(position, team);

    BodyComponent& body    = unit->addComponent<BodyComponent>();
    body.setFootprint(Footprint::square2x2());
    body.setBlocksLineOfSight(true);    /* the hull is terrain, for sight */
    body.setHullHeight(kVehicleLosHeight);
    body.setBaseHeightMode(BaseHeightMode::HighestUnderFootprint);
    body.setPickHeights(0.1f, 1.9f);

    MobilityComponent& mobility = unit->addComponent<MobilityComponent>();
    mobility.setGraph(MoveGraphKind::Vehicle);
    mobility.setPathStyle(PathStyle::Anchored);
    mobility.setCanRestOnRamp(false);   /* cannot use stairs at all */
    mobility.setCrushesHalfCover(true);

    CoverComponent& cover   = unit->addComponent<CoverComponent>();
    cover.setGrantsHullCover(true);   /* XCOM treats a big unit as mobile high cover */
    cover.setShowsCoverShields(false);  /* armour, not cover */

    unit->addComponent<DestructibleComponent>().setLeavesWreckage(true);

    PresentationComponent& presentation = unit->addComponent<PresentationComponent>();
    presentation.setVisual(VisualKind::Vehicle);
    presentation.setNames("tank", "TANK 2x2",
                          "selected the tank (2x2, crushes cover)");

    return unit;
}

}  // namespace game
