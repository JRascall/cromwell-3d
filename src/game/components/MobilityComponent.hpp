/* MobilityComponent.hpp — how an entity moves, and what its movement does to
 * the world it moves through.
 *
 * SINGLE RESPONSIBILITY: supply the move graph and the rules that travel with
 * it. Crushing lives here rather than in a destruction component because it is
 * a property of DRIVING over something, not of being destructible.
 */
#pragma once

#include "cromwell/entities/Component.hpp"

#include <memory>

namespace game {

using namespace cromwell;

class MoveGraph;
class World;

/* Which graph enumerates this body's legal steps. A closed set — adding one is
 * a deliberate edit to createGraph, and the compiler will say so. */
enum class MoveGraphKind {
    Infantry,
    Vehicle,
};

/* How the path preview is drawn along the route. Articulated walks cell centres
 * and turns; Anchored runs through the middle of a multi-tile footprint. */
enum class PathStyle {
    Articulated,
    Anchored,
};

class MobilityComponent : public Component {
public:
    MoveGraphKind graph() const { return graph_; }
    void setGraph(MoveGraphKind graph) { graph_ = graph; }

    PathStyle pathStyle() const { return pathStyle_; }
    void setPathStyle(PathStyle style) { pathStyle_ = style; }

    /* Infantry may not STOP on stairs (pass-through only); vehicles cannot use
     * them at all. Both answer false today, and it stays a field rather than a
     * constant because the first flying body will want true. */
    bool canRestOnRamp() const { return canRestOnRamp_; }
    void setCanRestOnRamp(bool canRest) { canRestOnRamp_ = canRest; }

    /* Does driving over destructible half cover flatten it? */
    bool crushesHalfCover() const { return crushesHalfCover_; }
    void setCrushesHalfCover(bool crushes) { crushesHalfCover_ = crushes; }

    std::unique_ptr<MoveGraph> createGraph(const World& world) const;

private:
    MoveGraphKind graph_ = MoveGraphKind::Infantry;
    PathStyle     pathStyle_ = PathStyle::Articulated;
    bool          canRestOnRamp_ = false;
    bool          crushesHalfCover_ = false;
};

}  // namespace game
