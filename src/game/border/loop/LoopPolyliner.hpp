/* LoopPolyliner.hpp — turn one boundary loop into a closed polyline.
 *
 * SINGLE RESPONSIBILITY: geometry. Inset where a wall demands it, join the
 * segments, chamfer the turns, and insert the micro-relief risers.
 *
 * `wallClearance` is NOT a general inset — the line rides the tile boundary,
 * because in XCOM's model nothing lives there and XCOM's own border config has
 * no inset parameter at all. But ART has thickness: a wall mesh straddling the
 * tile edge would swallow a line drawn on it. So the line steps inward by
 * `wallClearance` ONLY along boundaries that actually carry full cover, and
 * stays exactly on the grid line everywhere else.
 *
 * `chamfer` is the 45-degree corner cut (XCOM's MovementBorderLengthFactor of
 * 0.8 stops each edge a fifth short of its corner). `rounded` swaps the
 * straight cut for a small quadratic.
 */
#pragma once

#include "game/border/loop/EdgeCapHeight.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {


struct BorderPoint {
    float x = 0.0f;
    float y = 0.0f;
    float height = 0.0f;
    int   owner = 0;      /* the cell whose surface this point sits on */
};

class LoopPolyliner {
public:
    explicit LoopPolyliner(const World& world) : world_(world), caps_(world), terrain_(world) {}

    /* Appends into `out`, which is cleared first. Empty for a degenerate loop. */
    void build(const LoopSet& loops, int loopIndex,
               float wallClearance, float chamfer, bool rounded,
               std::vector<BorderPoint>& out);

private:
    /* one boundary edge, already inset and height-capped */
    struct Segment {
        float fromX = 0.0f, fromY = 0.0f;
        float toX   = 0.0f, toY   = 0.0f;
        int   unitX = 0, unitY = 0;      /* travel direction, +-1 on one axis */
        int   owner = 0;
        float capHeight = 0.0f;
    };

    /* a joint between two consecutive segments */
    struct Joint {
        float x = 0.0f, y = 0.0f;
        int   previousOwner = 0, owner = 0;
        float capHeight = 0.0f, previousCapHeight = 0.0f;
        bool  turns = false;
        int   inX = 0, inY = 0, outX = 0, outY = 0;
    };

    /* a chamfered vertex, before risers are inserted */
    struct Vertex {
        float x = 0.0f, y = 0.0f;
        int   owner = 0;
        float capHeight = 0.0f;
    };

    void buildSegments(const LoopSet& loops, const Loop& loop, float wallClearance);
    void joinSegments(const LoopSet& loops, const Loop& loop);
    void chamferJoints(float chamfer, bool rounded);
    void emitWithRisers(std::vector<BorderPoint>& out) const;

    float ownerHeight(int owner, float x, float y) const;
    bool  ownerIsRamp(int owner) const;

    const World&  world_;
    EdgeCapHeight caps_;
    Terrain       terrain_;

    /* instance scratch — the C original used three 65536-entry file statics */
    std::vector<Segment> segments_;
    std::vector<Joint>   joints_;
    std::vector<Vertex>  vertices_;
};

}  // namespace game
