#include "game/path/MoveAnimator.hpp"

#include <cmath>

namespace game {


void MoveAnimator::start(int destinationCell)
{
    running_ = true;
    segment_ = 0;
    t_ = 0.0f;
    destinationCell_ = destinationCell;
}

void MoveAnimator::advance(float deltaSeconds, const std::vector<PathPoint>& path)
{
    float remaining = kSpeed * deltaSeconds;

    while (remaining > 0.0f && segment_ < static_cast<int>(path.size()) - 1) {
        const PathPoint& a = path[static_cast<std::size_t>(segment_)];
        const PathPoint& b = path[static_cast<std::size_t>(segment_) + 1];

        float distance = std::sqrt((b.x - a.x) * (b.x - a.x) +
                                   (b.y - a.y) * (b.y - a.y) +
                                   (b.height - a.height) * (b.height - a.height));
        if (distance < 1e-6f) distance = 1e-6f;

        /* teleport arcs traverse fast */
        const float segmentLength = distance > 2.5f ? distance / 8.0f : distance;
        const float left = (1.0f - t_) * segmentLength;

        if (remaining >= left) {
            remaining -= left;
            segment_++;
            t_ = 0.0f;
        } else {
            t_ += remaining / segmentLength;
            remaining = 0.0f;
        }
    }
}

PathPoint MoveAnimator::positionOn(const std::vector<PathPoint>& path) const
{
    if (path.empty()) return {};

    const int last = static_cast<int>(path.size()) - 1;
    const int index = segment_ < last ? segment_ : last;
    const PathPoint& a = path[static_cast<std::size_t>(index)];
    const PathPoint& b = path[static_cast<std::size_t>(index < last ? index + 1 : index)];

    PathPoint out;
    out.x      = a.x + (b.x - a.x) * t_;
    out.y      = a.y + (b.y - a.y) * t_;
    out.height = a.height + (b.height - a.height) * t_;
    out.cell   = a.cell;
    return out;
}

}  // namespace game
