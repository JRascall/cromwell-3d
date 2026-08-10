/* BodyComponent.hpp — the shape an entity occupies and how it sits in the world.
 *
 * SINGLE RESPONSIBILITY: describe the body. Not how it moves, not how it is
 * drawn, not what it is called.
 *
 * COMPOSITION, NOT INHERITANCE. Soldier and Vehicle used to be classes whose
 * only difference on most of these was a constant — blocksLineOfSight returned
 * `false` in one and `true` in the other. A class hierarchy to hold two bools
 * is a vtable and two files buying what a field gives for nothing, and it makes
 * a THIRD body (a drone: 1x1 like infantry, sight-blocking like a hull) a new
 * class rather than a new value.
 *
 * NOT UNIT-SPECIFIC. A crate has a footprint, blocks sight and rests on the
 * floor; it is not a unit and should not have to be one to say so.
 */
#pragma once

#include "cromwell/entities/Component.hpp"
#include "game/units/Footprint.hpp"

namespace game {

using namespace cromwell;

class World;
class Cell;

/* How a body's base height is found. The two are genuinely different sums, not
 * one with a parameter: a single tile reads the terrain under it, including
 * ramp slope; a multi-tile hull sits on the HIGHEST floor it spans, because a
 * tank bridging a kerb rests on the kerb. */
enum class BaseHeightMode {
    TerrainCentre,
    HighestUnderFootprint,
};

/* The highest floor a footprint spans when anchored at `anchor`. Free rather
 * than a member because callers ask it about cells no body occupies — "how high
 * would the hull sit if it moved here" is the path preview's whole question. */
float highestFloorUnder(const World& world, const Cell& anchor, const Footprint& footprint);

class BodyComponent : public Component {
public:
    const Footprint& footprint() const { return footprint_; }
    void setFootprint(Footprint footprint) { footprint_ = std::move(footprint); }

    /* Only BIG bodies block sight — 1x1 bodies are transparent, so you shoot
     * past squadmates as in XCOM. */
    bool blocksLineOfSight() const { return blocksLineOfSight_; }
    void setBlocksLineOfSight(bool blocks) { blocksLineOfSight_ = blocks; }

    /* How tall the hull stands, for LOS. Meaningless when transparent. */
    float hullHeight() const { return hullHeight_; }
    void  setHullHeight(float height) { hullHeight_ = height; }

    BaseHeightMode baseHeightMode() const { return baseHeightMode_; }
    void setBaseHeightMode(BaseHeightMode mode) { baseHeightMode_ = mode; }

    /* The vertical slab the cursor is tested against, as a fraction of a cell.
     * Was two visitor overrides that each called testBox with two literals. */
    float pickMinHeight() const { return pickMinHeight_; }
    float pickMaxHeight() const { return pickMaxHeight_; }
    void setPickHeights(float minHeight, float maxHeight)
    {
        pickMinHeight_ = minHeight;
        pickMaxHeight_ = maxHeight;
    }

    /* Resolves the base height against the world for a body anchored at
     * `anchor`. */
    float baseHeightAt(const World& world, const Cell& anchor) const;

private:
    Footprint      footprint_ = Footprint::single();
    bool           blocksLineOfSight_ = false;
    float          hullHeight_ = 0.0f;
    BaseHeightMode baseHeightMode_ = BaseHeightMode::TerrainCentre;
    float          pickMinHeight_ = 0.25f;
    float          pickMaxHeight_ = 0.75f;
};

}  // namespace game
