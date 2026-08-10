/* MoveAnimator.hpp — walk a unit along its preview.
 *
 * SINGLE RESPONSIBILITY: advance a parameter along a polyline and report the
 * interpolated position. It does not move the unit, edit the world or draw —
 * the caller does that when isFinished() turns true.
 */
#pragma once

#include "game/movement/search/PathPoint.hpp"

#include <vector>

namespace game {


class MoveAnimator {
public:
    static constexpr float kSpeed = 4.2f;   /* tiles per second along a path */

    bool isRunning() const { return running_; }

    void start(int destinationCell);
    void stop() { running_ = false; }

    int destinationCell() const { return destinationCell_; }

    /* Advances along `path`; becomes finished once the last segment is done. */
    void advance(float deltaSeconds, const std::vector<PathPoint>& path);

    bool isFinished(const std::vector<PathPoint>& path) const
    {
        return segment_ >= static_cast<int>(path.size()) - 1;
    }

    /* Interpolated position; only meaningful while running. */
    PathPoint positionOn(const std::vector<PathPoint>& path) const;

private:
    bool  running_ = false;
    int   segment_ = 0;
    float t_ = 0.0f;
    int   destinationCell_ = -1;
};

}  // namespace game
