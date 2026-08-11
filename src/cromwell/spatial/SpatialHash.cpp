#include "cromwell/spatial/SpatialHash.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {

namespace {

/* Teschner, Heidelberger, Mueller, Pomeranets & Gross, "Optimized Spatial
 * Hashing for Collision Detection of Deformable Objects" (VMV 2003). Three
 * large primes, one per axis, XORed — the standard choice, and standard
 * because it scatters axis-aligned rows of cells rather than mapping them onto
 * consecutive buckets the way a naive sum does. */
constexpr std::uint32_t kPrimeX = 73856093u;
constexpr std::uint32_t kPrimeY = 19349663u;
constexpr std::uint32_t kPrimeZ = 83492791u;

/* 21 bits per axis in the packed cell key. */
constexpr std::uint64_t kAxisMask = 0x1FFFFFull;

int roundUpToPowerOfTwo(int value)
{
    int result = 1;
    while (result < value) result <<= 1;
    return result;
}

}  // namespace

SpatialHash::SpatialHash(float cellSize, int bucketCount)
    : cellSize_(cellSize > 1e-6f ? cellSize : 1.0f),
      inverseCellSize_(1.0f / (cellSize > 1e-6f ? cellSize : 1.0f)),
      buckets_(static_cast<std::size_t>(
                   roundUpToPowerOfTwo(bucketCount > 1 ? bucketCount : 1)),
               -1)
{
}

void SpatialHash::clear()
{
    /* assign rather than clear+resize: the bucket count never changes, so this
     * is one fill over memory already held. entries_ keeps its capacity. */
    buckets_.assign(buckets_.size(), -1);
    entries_.clear();
}

std::int32_t SpatialHash::cellCoord(float value, float inverseCellSize)
{
    return static_cast<std::int32_t>(std::floor(value * inverseCellSize));
}

std::uint64_t SpatialHash::cellKey(std::int32_t x, std::int32_t y, std::int32_t z)
{
    /* Low 21 bits of each two's-complement coordinate. Unique for the range
     * documented in the header; outside it two far-apart cells would alias,
     * which is a world larger than float positions can address anyway. */
    return ((static_cast<std::uint64_t>(x) & kAxisMask) << 42)
         | ((static_cast<std::uint64_t>(y) & kAxisMask) << 21)
         |  (static_cast<std::uint64_t>(z) & kAxisMask);
}

std::size_t SpatialHash::bucketOf(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    /* Unsigned throughout: signed overflow is undefined, and these products
     * are meant to wrap. */
    const std::uint32_t hash = (static_cast<std::uint32_t>(x) * kPrimeX)
                             ^ (static_cast<std::uint32_t>(y) * kPrimeY)
                             ^ (static_cast<std::uint32_t>(z) * kPrimeZ);

    /* Power-of-two bucket count, so the modulo is a mask. */
    return static_cast<std::size_t>(hash) & (buckets_.size() - 1);
}

void SpatialHash::insert(int id, Vec3 position)
{
    const std::int32_t cx = cellCoord(position.x, inverseCellSize_);
    const std::int32_t cy = cellCoord(position.y, inverseCellSize_);
    const std::int32_t cz = cellCoord(position.z, inverseCellSize_);

    const std::size_t bucket = bucketOf(cx, cy, cz);

    Entry entry;
    entry.cell     = cellKey(cx, cy, cz);
    entry.position = position;
    entry.id       = id;
    entry.next     = buckets_[bucket];   /* push onto the front of the chain */

    entries_.push_back(entry);
    buckets_[bucket] = static_cast<int>(entries_.size()) - 1;
}

template <class Visit>
void SpatialHash::forEachInCellRange(Vec3 min, Vec3 max, Visit visit) const
{
    if (entries_.empty()) return;
    if (max.x < min.x || max.y < min.y || max.z < min.z) return;

    const std::int32_t minX = cellCoord(min.x, inverseCellSize_);
    const std::int32_t minY = cellCoord(min.y, inverseCellSize_);
    const std::int32_t minZ = cellCoord(min.z, inverseCellSize_);
    const std::int32_t maxX = cellCoord(max.x, inverseCellSize_);
    const std::int32_t maxY = cellCoord(max.y, inverseCellSize_);
    const std::int32_t maxZ = cellCoord(max.z, inverseCellSize_);

    for (std::int32_t z = minZ; z <= maxZ; z++)
    for (std::int32_t y = minY; y <= maxY; y++)
    for (std::int32_t x = minX; x <= maxX; x++) {
        const std::uint64_t key = cellKey(x, y, z);

        for (int i = buckets_[bucketOf(x, y, z)]; i >= 0; ) {
            const Entry& entry = entries_[static_cast<std::size_t>(i)];
            i = entry.next;

            /* The bucket may hold entries from other cells that hashed here.
             * Without this test they would be emitted, and an entry could come
             * back twice from two different cells in the same query. */
            if (entry.cell != key) continue;
            visit(entry);
        }
    }
}

void SpatialHash::queryRadius(Vec3 centre, float radius, std::vector<int>& out) const
{
    out.clear();
    if (radius <= 0.0f) return;

    const Vec3  extent{ radius, radius, radius };
    const float radiusSquared = radius * radius;

    forEachInCellRange(centre - extent, centre + extent,
                       [&](const Entry& entry) {
                           /* Exact: the cell sweep is only a candidate filter. */
                           if (distanceSquared(entry.position, centre) <= radiusSquared)
                               out.push_back(entry.id);
                       });
}

void SpatialHash::queryBox(Vec3 min, Vec3 max, std::vector<int>& out) const
{
    out.clear();

    forEachInCellRange(min, max, [&](const Entry& entry) {
        const Vec3& p = entry.position;
        if (p.x >= min.x && p.x <= max.x &&
            p.y >= min.y && p.y <= max.y &&
            p.z >= min.z && p.z <= max.z)
            out.push_back(entry.id);
    });
}

void SpatialHash::querySegment(Vec3 start, Vec3 end, float radius, std::vector<int>& out) const
{
    out.clear();
    if (radius <= 0.0f) return;

    const Vec3 min{ std::min(start.x, end.x) - radius, std::min(start.y, end.y) - radius,
                    std::min(start.z, end.z) - radius };
    const Vec3 max{ std::max(start.x, end.x) + radius, std::max(start.y, end.y) + radius,
                    std::max(start.z, end.z) + radius };

    const Vec3  along = end - start;
    const float lengthSquared = along.lengthSquared();
    const float radiusSquared = radius * radius;

    forEachInCellRange(min, max, [&](const Entry& entry) {
        /* Distance from the point to the SEGMENT, not to the infinite line: a
         * body well past the end of a trace is not along it, and clamping the
         * parameter is the whole difference. A degenerate segment — a zero-length
         * trace, which happens when a sweep is asked for before anything has
         * moved — falls back to the distance from the start, which is the right
         * answer rather than a division by zero. */
        float t = 0.0f;
        if (lengthSquared > 1e-12f) {
            t = dot(entry.position - start, along) / lengthSquared;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

        const Vec3 closest = start + along * t;
        if (distanceSquared(entry.position, closest) <= radiusSquared) out.push_back(entry.id);
    });
}

int SpatialHash::longestChain() const
{
    int longest = 0;
    for (int head : buckets_) {
        int length = 0;
        for (int i = head; i >= 0; i = entries_[static_cast<std::size_t>(i)].next) length++;
        if (length > longest) longest = length;
    }
    return longest;
}

}  // namespace cromwell
