#include "game/border/loop/LoopPolyliner.hpp"

#include <cmath>

namespace game {

namespace {

int signOf(float v) { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); }

}  // namespace

float LoopPolyliner::ownerHeight(int owner, float x, float y) const
{
    return terrain_.surfaceHeightAt(world_.lattice().cellAt(owner), x, y);
}

bool LoopPolyliner::ownerIsRamp(int owner) const
{
    return world_.at(owner).isRamp();
}

/* The line rides the tile boundary. It steps inward ONLY where that boundary
 * carries a WALL, because our wall art straddles the tile edge and would
 * otherwise swallow the line whole: a full wall is 0.09 thick (+-0.045)
 * against a ribbon half-width of ~0.028. Kerbs and open boundaries keep the
 * line exactly on the grid line. */
void LoopPolyliner::buildSegments(const LoopSet& loops, const Loop& loop, float wallClearance)
{
    const Lattice& lattice = world_.lattice();
    segments_.clear();
    segments_.reserve(static_cast<std::size_t>(loop.count));

    for (int i = 0; i < loop.count; i++) {
        const EdgeId edge = loops.edgeAt(loop, i);
        const int    cellIndex = edge.cell();
        const Dir    d = edge.dir();
        const Cell   cell = lattice.cellAt(cellIndex);

        const EdgeSegment corners = edgeCorners(cell.x, cell.y, d);
        const int unitX = signOf(corners.toX - corners.fromX);
        const int unitY = signOf(corners.toY - corners.fromY);
        const int leftX = -unitY, leftY = unitX;   /* rotate 90 CCW = interior side */

        /* Ramps never take the clearance. A flight beside a wall would other-
         * wise be pushed off the grid line, so its two side rails stop meeting
         * the landing squarely and need a stub across the top to close up.
         * XCOM has no inset anywhere, and on stairs it shows: the line simply
         * follows the grid up one side and down the other. */
        const bool onRamp = world_.at(cellIndex).isRamp();
        const float inset =
            (!onRamp && world_.effectiveEdge(cell, d).cover == Cover::Full) ? wallClearance : 0.0f;

        Segment segment;
        segment.unitX = unitX;
        segment.unitY = unitY;
        segment.owner = cellIndex;
        segment.fromX = corners.fromX + static_cast<float>(leftX) * inset;
        segment.fromY = corners.fromY + static_cast<float>(leftY) * inset;
        segment.toX   = corners.toX   + static_cast<float>(leftX) * inset;
        segment.toY   = corners.toY   + static_cast<float>(leftY) * inset;
        /* ramps keep kNoCap so their height is sampled from the plane */
        segment.capHeight = onRamp
            ? EdgeCapHeight::kNoCap
            : caps_.capFor(cellIndex, d, terrain_.centerHeight(cell));

        segments_.push_back(segment);
    }
}

/* Join consecutive segments: collinear, perpendicular, or a 180-degree
 * retrace around the tip of a thin wall. */
void LoopPolyliner::joinSegments(const LoopSet& loops, const Loop& loop)
{
    const Lattice& lattice = world_.lattice();
    const int count = static_cast<int>(segments_.size());
    joints_.clear();
    joints_.reserve(segments_.size() + 1);

    for (int i = 0; i < count; i++) {
        /* AN OPEN RUN HAS NO SEGMENT BEFORE ITS FIRST. Joining segment 0 to
         * the last one there would draw a chord straight across the gap the
         * suppression opened, which is the whole thing we are avoiding. It
         * starts on its own first corner instead, and the matching end joint
         * is appended after the loop. */
        if (!loop.closed && i == 0) {
            const Segment& first = segments_[0];
            Joint start;
            start.previousOwner     = first.owner;
            start.owner             = first.owner;
            start.capHeight         = first.capHeight;
            start.previousCapHeight = first.capHeight;
            start.x = first.fromX;
            start.y = first.fromY;
            start.turns = false;
            joints_.push_back(start);
            continue;
        }

        const Segment& previous = segments_[static_cast<std::size_t>((i - 1 + count) % count)];
        const Segment& current  = segments_[static_cast<std::size_t>(i)];

        Joint joint;
        joint.previousOwner     = previous.owner;
        joint.owner             = current.owner;
        joint.capHeight         = current.capHeight;
        joint.previousCapHeight = previous.capHeight;

        if (previous.unitX == current.unitX && previous.unitY == current.unitY) {
            /* collinear */
            joint.x = current.fromX;
            joint.y = current.fromY;
            joint.turns = false;
        } else if (previous.unitX == -current.unitX && previous.unitY == -current.unitY) {
            /* A SLIT ENDS IN A POINT, not a stub.
             *
             * The foot and head of a flight are slits: the ramp's side face
             * and the ground beside it are both boundaries, so the line
             * reverses there. This used to push the two reversal vertices
             * apart to give the turn some width, which reads as a little
             * connector bridging the two rails at the top and bottom of every
             * staircase. XCOM turns on the spot — one vertex, sharp V, and the
             * line simply carries on along the grid.
             *
             * turns=false so the corner is NOT chamfered: a chamfer here would
             * reintroduce the same stub by another route. */
            const EdgeId previousEdge = loops.edgeAt(loop, (i - 1 + count) % count);
            const Cell   previousCell = lattice.cellAt(previous.owner);
            const EdgeSegment corners =
                edgeCorners(previousCell.x, previousCell.y, previousEdge.dir());
            joint.x = corners.toX;
            joint.y = corners.toY;
            joint.turns = false;
        } else {
            /* perpendicular */
            joint.x = (previous.unitY == 0) ? current.fromX : previous.fromX;
            joint.y = (previous.unitY == 0) ? previous.fromY : current.fromY;
            joint.turns = true;
            joint.inX  = previous.unitX;
            joint.inY  = previous.unitY;
            joint.outX = current.unitX;
            joint.outY = current.unitY;
        }
        joints_.push_back(joint);
    }

    /* The far end of the last segment. Only an open run needs it: on a cycle
     * that point IS joint 0. */
    if (!loop.closed && count > 0) {
        const Segment& last = segments_[static_cast<std::size_t>(count - 1)];
        Joint end;
        end.previousOwner     = last.owner;
        end.owner             = last.owner;
        end.capHeight         = last.capHeight;
        end.previousCapHeight = last.capHeight;
        end.x = last.toX;
        end.y = last.toY;
        end.turns = false;
        joints_.push_back(end);
    }
}

void LoopPolyliner::chamferJoints(float chamfer, bool rounded, bool closed)
{
    const int count = static_cast<int>(joints_.size());
    vertices_.clear();
    vertices_.reserve(joints_.size() * 2);

    for (int i = 0; i < count; i++) {
        const Joint& joint = joints_[static_cast<std::size_t>(i)];

        if (!joint.turns) {
            vertices_.push_back({ joint.x, joint.y, joint.owner, joint.capHeight });
            continue;
        }

        /* Both neighbours exist without wrapping on an open run: its first and
         * last joints are the added end stops, which never turn, so a turning
         * joint is always strictly interior. */
        const int previousIndex = closed ? (i - 1 + count) % count : i - 1;
        const int nextIndex     = closed ? (i + 1) % count         : i + 1;
        const Joint& previous = joints_[static_cast<std::size_t>(previousIndex)];
        const Joint& next     = joints_[static_cast<std::size_t>(nextIndex)];

        const float lengthIn  = std::hypot(joint.x - previous.x, joint.y - previous.y);
        const float lengthOut = std::hypot(next.x - joint.x, next.y - joint.y);

        /* Both turn directions get the cut. XCOM's MovementBorderLengthFactor
         * of 0.8 shortens EVERY edge by a fifth at BOTH ends, and the gap that
         * leaves is the 45-degree connector — it does not care whether the
         * corner is convex or concave. A concave cut does clip the corner of
         * the tile it wraps, which is what the depth fade is there for.
         *
         * (This was briefly clamped to 0 on concave turns. That came from the
         * old `min(chamfer, inset * 0.7)` collapsing once the inset went to
         * zero — an accident, not a rule.) */
        const float radius = std::fmin(chamfer, std::fmin(lengthIn * 0.45f, lengthOut * 0.45f));

        const float startX = joint.x - static_cast<float>(joint.inX)  * radius;
        const float startY = joint.y - static_cast<float>(joint.inY)  * radius;
        const float endX   = joint.x + static_cast<float>(joint.outX) * radius;
        const float endY   = joint.y + static_cast<float>(joint.outY) * radius;

        if (!rounded) {
            vertices_.push_back({ startX, startY, joint.previousOwner, joint.previousCapHeight });
            vertices_.push_back({ endX,   endY,   joint.owner,         joint.capHeight });
        } else {
            static constexpr float kSamples[4] = { 0.0f, 0.34f, 0.67f, 1.0f };
            for (float t : kSamples) {
                const float q = 1.0f - t;
                vertices_.push_back({
                    q * q * startX + 2 * q * t * joint.x + t * t * endX,
                    q * q * startY + 2 * q * t * joint.y + t * t * endY,
                    t < 0.5f ? joint.previousOwner : joint.owner,
                    t < 0.5f ? joint.previousCapHeight : joint.capHeight });
            }
        }
    }
}

/* MICRO-RELIEF RISERS.
 *
 * Every polyline joint sits exactly ON a tile boundary, and each joint carries
 * the height of the tile it is entering. Join two such points with a plain
 * chord and the height change gets spread across the WHOLE of the previous
 * tile's face: crossing a kerb, the line starts dropping a full tile early and
 * sinks below the slab it is meant to sit on, cutting straight through it.
 * That is the kerb bug — and it is geometry, not shading, which is why it
 * showed up in the JS build too, where the ribbon is drawn with nothing but a
 * polygon offset.
 *
 * Correct behaviour: hold the level right up to the lip, then drop vertically.
 * Ramps stay exempt — a chord across an inclined plane IS the plane. */
void LoopPolyliner::emitWithRisers(bool closed, std::vector<BorderPoint>& out) const
{
    const int count = static_cast<int>(vertices_.size());

    for (int i = 0; i < count; i++) {
        const Vertex& current = vertices_[static_cast<std::size_t>(i)];

        const float currentHeight = (current.capHeight > EdgeCapHeight::kNoCap)
            ? current.capHeight
            : ownerHeight(current.owner, current.x, current.y);

        out.push_back({ current.x, current.y, currentHeight, current.owner });

        /* An open run's last vertex has nothing to rise TO. */
        if (!closed && i == count - 1) break;

        const Vertex& next = vertices_[static_cast<std::size_t>((i + 1) % count)];
        if (ownerIsRamp(current.owner) || ownerIsRamp(next.owner)) continue;

        const float nextHeight = (next.capHeight > EdgeCapHeight::kNoCap)
            ? next.capHeight
            : ownerHeight(next.owner, next.x, next.y);
        if (std::fabs(currentHeight - nextHeight) <= 1e-6f) continue;

        /* hold this level all the way to the lip; the next iteration emits the
         * same position at its own height, and the pair becomes a vertical
         * riser exactly at the tile boundary */
        out.push_back({ next.x, next.y, currentHeight, current.owner });
    }
}

void LoopPolyliner::build(const LoopSet& loops, int loopIndex,
                          float wallClearance, float chamfer, bool rounded,
                          std::vector<BorderPoint>& out)
{
    out.clear();
    if (loopIndex < 0 || loopIndex >= loops.loopCount()) return;

    const Loop& loop = loops.loop(loopIndex);
    /* Two edges is the smallest closed thing; a run of one is a legitimate
     * single grid edge with a point at each end. */
    if (loop.count < (loop.closed ? 2 : 1)) return;

    buildSegments(loops, loop, wallClearance);
    joinSegments(loops, loop);
    chamferJoints(chamfer, rounded, loop.closed);
    emitWithRisers(loop.closed, out);
}

}  // namespace game
