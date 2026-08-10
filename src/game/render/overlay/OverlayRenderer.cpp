#include "game/render/overlay/OverlayRenderer.hpp"

#include "game/render/Palette.hpp"

namespace game {


void OverlayRenderer::drawVisibility(const VisibilityField& visibility, int maxStorey) const
{
    const Lattice& lattice = world_.lattice();

    for (int z = 0; z < lattice.depth(); z++) {
        if (Lattice::storeyOfZ(z) > maxStorey) break;

        for (int y = 0; y < lattice.height(); y++)
        for (int x = 0; x < lattice.width(); x++) {
            if (!standability_.isStandable(x, y, z)) continue;

            const Visibility grade = visibility.at(lattice.index(x, y, z));
            const Color colour = (grade == Visibility::Direct)   ? palette::kVisibleDirect
                               : (grade == Visibility::PeekOnly) ? palette::kVisiblePeek
                                                                 : palette::kVisibleNone;

            DrawCube(Vector3{ static_cast<float>(x) + 0.5f,
                              terrain_.centerHeight(x, y, z) + 0.028f,
                              static_cast<float>(y) + 0.5f },
                     0.94f, 0.01f, 0.94f, colour);
        }
    }
}

void OverlayRenderer::drawCoverShields(const Cell& cell) const
{
    for (Dir d : kAllDirs) {
        const Cover grade = cover_.displayCover(cell, d);
        if (grade == Cover::None) continue;

        const bool  full = (grade == Cover::Full);
        const float height = terrain_.centerHeight(cell) +
                             (full ? 0.45f * kStoreyHeight : 0.34f);
        const float px = static_cast<float>(cell.x) + 0.5f + static_cast<float>(dx(d)) * 0.36f;
        const float py = static_cast<float>(cell.y) + 0.5f + static_cast<float>(dy(d)) * 0.36f;

        const Color colour = full ? palette::kCoverFull : palette::kCoverHalf;
        const bool northSouth = (d == Dir::North || d == Dir::South);

        DrawCube(Vector3{ px, height, py },
                 northSouth ? 0.30f : 0.04f,
                 full ? 0.40f : 0.20f,
                 northSouth ? 0.04f : 0.30f,
                 colour);
    }
}

void OverlayRenderer::drawHoverPlate(const Cell& cell, float height, float size, Color colour) const
{
    const float offset = size > 1.0f ? 1.0f : 0.5f;
    DrawCube(Vector3{ static_cast<float>(cell.x) + offset, height,
                      static_cast<float>(cell.y) + offset },
             size, 0.012f, size, colour);
}

void OverlayRenderer::drawPathPreview(const std::vector<PathPoint>& path) const
{
    for (std::size_t i = 0; i + 1 < path.size(); i++) {
        const Vector3 a{ path[i].x,     path[i].height + 0.12f,     path[i].y };
        const Vector3 b{ path[i + 1].x, path[i + 1].height + 0.12f, path[i + 1].y };
        DrawLine3D(a, b, RAYWHITE);
    }
}

}  // namespace game
