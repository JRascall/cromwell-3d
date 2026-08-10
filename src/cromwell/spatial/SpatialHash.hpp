/* SpatialHash.hpp — what is near this point.
 *
 * SINGLE RESPONSIBILITY: index positions by cell so a neighbourhood query
 * touches the neighbourhood instead of the whole world. It owns no entities and
 * knows nothing about what an id means.
 *
 * THIS IS NOT NAVIGATION, AND THE TWO GET CONFUSED CONSTANTLY. This answers
 * "what is near XYZ". It will never tell you a wall is in the way. Pathfinding
 * needs a different structure with a different update rate — see
 * study/navigation.md, which is the long version of why they must not be the
 * same object.
 *
 * WHY A HASH RATHER THAN A DENSE GRID. The game on top of this engine happens
 * to have a lattice, and for it a dense array is exactly right — a tile world's
 * coordinates ARE the index, which is why game/ has OccupancyGrid and does not
 * use this. An ENGINE cannot assume that. A hash takes any coordinate without
 * being told the world's extents, and spends memory only where something
 * stands, which is what an open world needs.
 *
 * WHY NOT A TREE. Entities move every frame. Insert and query here are O(1)
 * with a small constant and no rebalancing; a BVH or octree pays for its better
 * query bound with an update cost that a crowd never stops incurring. A tree
 * earns its place over large, mostly-static geometry and over ray queries —
 * a different structure for a different job, added when there is one, not
 * instead of this.
 *
 * REBUILD PER FRAME, RATHER THAN INCREMENTAL UPDATE. There is no remove() and
 * no move(). Clear it and refill it.
 *
 *   - Almost everything in it is moving anyway. A crowd where every agent moves
 *     every frame gets nothing from an incremental path but the bookkeeping.
 *   - It cannot go stale. An index with removal has an invariant to maintain
 *     across every caller; this one has a lifetime of one frame and no way to
 *     hold a wrong answer into the next.
 *   - Refilling is a linear write over contiguous memory, which is the pattern
 *     hardware is fastest at. Unlinking scattered nodes is not.
 *
 * For things that rarely move — props, cover markers, spawn points — keep a
 * SECOND instance and rebuild it when that set changes rather than per frame.
 * Two cheap indexes with clear lifetimes beat one clever one.
 *
 * POSITIONS ARE COPIED IN, not looked up through a pointer at query time. A
 * query that had to dereference every candidate entity to get its position
 * would scatter across the heap and undo the whole point of the index; holding
 * the position beside the id keeps the walk sequential.
 *
 * WHAT THIS UNLOCKS. Local steering — the flocking and collision avoidance that
 * a swarm needs — is a neighbourhood query per agent per frame. Done against
 * every other agent it is O(n^2) and dies in the low hundreds; done against a
 * spatial index it is O(n) with a small constant, which is the difference
 * between a crowd and a demo. It is the enabling structure for that work, more
 * than any navigation mesh is.
 */
#pragma once

#include "cromwell/math/Vec3.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

class SpatialHash {
public:
    /* CELL SIZE IS THE ONLY TUNING KNOB, AND IT IS NOT A CORRECTNESS KNOB —
     * every value returns identical answers, only faster or slower. It trades
     * two costs: cells much smaller than a query make it sweep many mostly
     * empty ones, and cells much larger fill each with candidates that fail the
     * distance test.
     *
     * MEASURED, on 2000 agents with a 2 unit query radius (`xcom_perf`):
     *
     *     cell = 0.25x radius    6.109 ms      <- 12x worse than the best
     *     cell = 0.5x  radius    1.532 ms
     *     cell = 1x    radius    0.558 ms
     *     cell = 2x    radius    0.510 ms      <- best
     *     cell = 4x    radius    0.552 ms
     *     cell = 8x    radius    1.032 ms
     *
     * So: SET IT TO ONE OR TWO TIMES THE RADIUS YOU QUERY WITH. The bowl is
     * shallow between 1x and 4x — anywhere in there is within a tenth of the
     * best — and it is far steeper on the small side than the large, so when
     * unsure, guess big. Halving it from the optimum costs 3x; doubling costs
     * nothing measurable.
     *
     * Serving two very different radii from one index gets the worse of both.
     * Use two indexes; they are cheap.
     *
     * `bucketCount` is rounded up to a power of two so the modulo is a mask.
     * Size it above the expected entry count — collisions cost a chain walk,
     * and longestChain() is there to tell you when that has gone wrong. */
    explicit SpatialHash(float cellSize = 4.0f, int bucketCount = 4096);

    /* Empties it without giving the memory back, so a per-frame rebuild settles
     * into steady state after the first few frames and stops allocating. */
    void clear();

    void insert(int id, Vec3 position);

    /* Ids whose position lies within `radius` of `centre`. `out` is CLEARED and
     * refilled — pass the same vector every frame and the query stops
     * allocating entirely, which matters when several hundred agents each run
     * one per frame.
     *
     * Exact, not cell-approximate: candidates come from the overlapping cells
     * and are then distance-tested, so nothing outside the sphere is returned. */
    void queryRadius(Vec3 centre, float radius, std::vector<int>& out) const;

    /* The same, for an axis-aligned box. Inclusive on both bounds. */
    void queryBox(Vec3 min, Vec3 max, std::vector<int>& out) const;

    int  size() const { return static_cast<int>(entries_.size()); }
    bool empty() const { return entries_.empty(); }
    float cellSize() const { return cellSize_; }
    int   bucketCount() const { return static_cast<int>(buckets_.size()); }

    /* THE FAILURE MODE, MADE VISIBLE. A hash grid degenerates when everything
     * lands in one bucket — an army standing on one spot, or a cell size far
     * too large. Then a query walks a list instead of a neighbourhood and the
     * index has quietly become the linear scan it replaced. Nothing about the
     * results changes, which is exactly why it needs measuring rather than
     * noticing. Cheap enough to assert on in a debug build. */
    int longestChain() const;

private:
    struct Entry {
        std::uint64_t cell;       /* packed cell coordinate, see cellKey */
        Vec3          position;
        int           id = 0;
        int           next = -1;  /* index of the next entry in this bucket */
    };

    /* Cell coordinate of a position, flooring so that negative coordinates step
     * the same way positive ones do — integer division truncates toward zero
     * and would make the cells either side of the origin twice as wide. */
    static std::int32_t cellCoord(float value, float inverseCellSize);

    /* The three cell coordinates packed into one comparable value, 21 bits per
     * axis. That is +-1,048,576 cells per axis — at a 4 unit cell, a world
     * about eight million units across, which is past where float positions
     * lose the precision to tell neighbours apart anyway.
     *
     * Stored per entry so a query can tell a genuine cell match from a HASH
     * COLLISION. Without it, two different cells sharing a bucket would each
     * report the other's entries, and an entry could be emitted twice in one
     * query — a duplicate that no distance test can catch. */
    static std::uint64_t cellKey(std::int32_t x, std::int32_t y, std::int32_t z);

    /* Teschner et al.'s three large primes — the standard spatial hash. */
    std::size_t bucketOf(std::int32_t x, std::int32_t y, std::int32_t z) const;

    /* Walks the cells covering a box and calls `visit(entry)` for each entry
     * genuinely in one of them. Shared by both queries, so the collision
     * handling exists once. */
    template <class Visit>
    void forEachInCellRange(Vec3 min, Vec3 max, Visit visit) const;

    float cellSize_;
    float inverseCellSize_;

    std::vector<int>   buckets_;   /* head index per bucket, -1 when empty */
    std::vector<Entry> entries_;
};

}  // namespace cromwell
