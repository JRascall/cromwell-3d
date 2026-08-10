/* VisibilityComputer.hpp — fill a VisibilityField from a viewpoint.
 *
 * SINGLE RESPONSIBILITY: run the eye-by-cell sweep. It owns the loop; the
 * casting is RayCaster's, the eyes are EyeSet's, the storage is
 * VisibilityField's.
 */
#pragma once

#include "core/los/EyeSet.hpp"
#include "core/los/VisibilityField.hpp"
#include "core/query/Standability.hpp"
#include "core/world/World.hpp"

namespace xcom {

class Unit;
class UnitRoster;

class VisibilityComputer {
public:
    /* Pure terrain. */
    explicit VisibilityComputer(const World& world);

    /* Hull-aware: big units block sight, except the viewer's own hull. */
    VisibilityComputer(const World& world, const UnitRoster& roster, const Unit* viewer);

    void compute(const Cell& from, VisibilityField& out) const;

private:
    const World&      world_;
    Standability      standability_;
    EyeSet            eyes_;
    const UnitRoster* roster_;
    const Unit*       viewer_;
};

}  // namespace xcom
