#include "app/path/PathPreviewBuilder.hpp"

#include "core/units/Soldier.hpp"
#include "core/units/Vehicle.hpp"

#include <algorithm>

namespace xcom {

void PathPreviewBuilder::visit(const Soldier&)
{
    const Lattice& lattice = world_.lattice();

    for (std::size_t i = 0; i < route_->size(); i++) {
        const int  index = (*route_)[i];
        const Cell cell  = lattice.cellAt(index);
        const float height = terrain_.centerHeight(cell);

        if (i > 0) {
            const int  previousIndex = (*route_)[i - 1];
            const Cell previous = lattice.cellAt(previousIndex);
            const float previousHeight = terrain_.centerHeight(previous);
            const MoveKind kind = reach_->arrivalKind(index);

            const float midX = (static_cast<float>(cell.x) + static_cast<float>(previous.x)) * 0.5f + 0.5f;
            const float midY = (static_cast<float>(cell.y) + static_cast<float>(previous.y)) * 0.5f + 0.5f;

            if (kind == MoveKind::Drop || kind == MoveKind::Ladder || kind == MoveKind::Mantle) {
                out_->push_back({ midX, midY, previousHeight, previousIndex });  /* step out */
                out_->push_back({ midX, midY, height, index });                  /* the riser */
            } else if (kind == MoveKind::Climb) {
                out_->push_back({ midX, midY, std::max(height, previousHeight) + 0.42f, index });
            } else if (kind == MoveKind::Portal) {
                out_->push_back({ static_cast<float>(previous.x) + 0.5f,
                                  static_cast<float>(previous.y) + 0.5f,
                                  previousHeight + 1.1f, previousIndex });
                out_->push_back({ static_cast<float>(cell.x) + 0.5f,
                                  static_cast<float>(cell.y) + 0.5f,
                                  height + 1.1f, index });
            }
        }

        out_->push_back({ static_cast<float>(cell.x) + 0.5f,
                          static_cast<float>(cell.y) + 0.5f,
                          height, index });
    }
}

/* A straight anchor-centre chain on its storey. No stairs, drops or portals to
 * articulate — vehicles stay on one surface — and each anchor's height follows
 * the highest floor under the footprint. */
void PathPreviewBuilder::visit(const Vehicle& vehicle)
{
    const Lattice& lattice = world_.lattice();

    for (int index : *route_) {
        const Cell cell = lattice.cellAt(index);
        out_->push_back({ static_cast<float>(cell.x) + 1.0f,
                          static_cast<float>(cell.y) + 1.0f,
                          Vehicle::footprintBaseHeight(world_, cell, vehicle.footprint()),
                          index });
    }
}

void PathPreviewBuilder::build(const Unit& unit, const ReachField& reach,
                               const std::vector<int>& route, std::vector<PathPoint>& out)
{
    out.clear();
    if (route.empty()) return;

    unit_  = &unit;
    reach_ = &reach;
    route_ = &route;
    out_   = &out;
    unit.accept(*this);
}

}  // namespace xcom
