/* TraceHit.hpp — what a trace found, and where the multiple answers go.
 *
 * SINGLE RESPONSIBILITY: carry one contact, and collect several without
 * allocating.
 *
 * THIS IS Unreal's FHitResult, cut down to what is actually read. The fields
 * that survived are the ones whose absence forces the caller to recompute
 * something the trace already knew — and every one of those recomputations is a
 * chance to get it subtly wrong.
 *
 * ================ THE FIELD EVERYBODY MISSES: point VERSUS end ==============
 *
 * FOR A RAY THEY ARE THE SAME POINT. FOR A SWEPT SHAPE THEY ARE NOT, and
 * confusing them is the single most common mistake made with sweep results.
 *
 *   point — where the surfaces touched. On the wall. Put the bullet decal here,
 *           spawn the impact spark here.
 *   end   — where the SHAPE'S CENTRE ends up at the moment of contact, half a
 *           box short of the wall. Move the character here.
 *
 * Placing a character at `point` buries it in the wall by its own half-extent;
 * placing a decal at `end` floats it off the surface by the same amount. Both
 * are supplied because both are wanted and neither can be derived from the
 * other without knowing the shape, which the caller may no longer have in hand.
 *
 * ======================= STARTING INSIDE SOMETHING =========================
 *
 * A SWEEP THAT BEGINS OVERLAPPING IS NOT A MISS, and it is not an ordinary hit
 * either. It happens constantly and legitimately: a character standing on a
 * floor sweeps downward from a position that already touches it, a grenade
 * spawns clipping the wall it bounced off. Reporting nothing makes the caller
 * walk through geometry; reporting a normal hit at distance zero makes it
 * resolve a contact whose direction is meaningless.
 *
 * So `startPenetrating` is a field, distance is zero, and the normal is the
 * shallowest separating direction rather than a surface normal — which is what
 * a depenetration step actually wants. Unreal reports the same thing under
 * `bStartPenetrating`, and every caller that ignores it eventually files a bug
 * about characters falling through floors.
 *
 * ==================== WHY THE MULTI-HIT BUFFER IS BORROWED =================
 *
 * NO ALLOCATION, per CLAUDE.md's hot-loop rule and for the same reason
 * SpatialHash::queryRadius fills a caller-supplied vector: a trace runs per
 * agent per frame, and a std::vector returned by value is a malloc in that
 * loop. TraceHits wraps storage the caller owns — a stack array at the call
 * site, a member that persists between frames, whatever suits — and reports
 * `overflowed` rather than growing.
 *
 * OVERFLOW IS REPORTED, NOT HIDDEN. A trace that quietly returned the first
 * eight of twelve hits would be wrong in a way no assertion catches, so the
 * flag exists and the FARTHEST hit is the one dropped, so what survives is
 * always the nearest N — which is what a caller that undersized its buffer
 * almost certainly wanted.
 */
#pragma once

#include "cromwell/collision/Layer.hpp"
#include "cromwell/math/Vec3.hpp"

#include <algorithm>

namespace cromwell {

/* One contact. ONE-SHOT DATA CARRIER (see ui/core/UiColor.hpp) — produced by a
 * trace, read at the call site. Public members for the same reason. */
struct TraceHit {
    /* Block or Overlap. Never Ignore: an ignored candidate does not become a
     * hit, so a hit always has a real response, and the field says WHICH rather
     * than whether. */
    Response response = Response::Block;

    /* Metres from the trace's start to first contact, and the same as a fraction
     * of the trace's length. The fraction is what a caller compares against
     * another trace's, or scales a velocity by; the distance is what a caller
     * prints. Both, because deriving either needs the trace length that the
     * caller may not have kept. */
    float distance = 0.0f;
    float fraction = 0.0f;

    /* Where the surfaces touched. See the header — this is NOT where to put the
     * shape. */
    Vec3 point;

    /* Out of the surface, toward where the trace came from. Unit length.
     *
     * For a `startPenetrating` hit this is the shallowest separating direction
     * instead, which is what a depenetration push needs; it is still unit
     * length, so a caller that does not check the flag gets something sane
     * rather than a zero vector. */
    Vec3 normal;

    /* Where the swept shape's CENTRE sits at the moment of contact. The position
     * to move a character to. Equals `point` for a ray. */
    Vec3 end;

    /* The sweep began already overlapping this. distance and fraction are zero;
     * see the header. */
    bool startPenetrating = false;

    /* What was hit, in the caller's own terms. -1 for static world geometry,
     * which has no id — the cell coordinates are in `cell` for that case. */
    int id = -1;

    /* The integer cell, for a hit against grid geometry. Meaningless when `id`
     * names a dynamic body; the trace fills whichever applies. */
    int cellX = 0;
    int cellY = 0;
    int cellZ = 0;

    /* What the thing hit was, for a caller that filtered loosely and wants to
     * branch on the answer — a shot that penetrates wood and stops at steel. */
    LayerId layer;
};

/* A bounded, borrowed collection of hits, kept sorted by distance.
 *
 * ONE-SHOT: built around storage the caller owns, filled by one trace, read,
 * discarded. It does not own the array and must not outlive it. */
class TraceHits {
public:
    TraceHits(TraceHit* storage, int capacity)
        : storage_(storage), capacity_(storage != nullptr ? capacity : 0) {}

    /* Inserts in distance order, so the buffer is sorted at all times and the
     * caller never has to remember to sort it.
     *
     * INSERTION SORT, DELIBERATELY. A trace's hit count is single digits in
     * every real case; an insertion into a sorted array of eight is a handful of
     * moves and beats sorting afterwards, and — the part that matters — it is
     * what makes dropping the FARTHEST hit on overflow correct rather than
     * arbitrary. Returns false when the hit was too far to keep. */
    bool add(const TraceHit& hit)
    {
        if (capacity_ <= 0) {
            overflowed_ = true;
            return false;
        }

        if (count_ == capacity_) {
            overflowed_ = true;
            /* Full, and this one is no nearer than the farthest kept: drop it. */
            if (hit.distance >= storage_[count_ - 1].distance) return false;
            --count_;  /* evict the farthest to make room */
        }

        int index = count_;
        while (index > 0 && storage_[index - 1].distance > hit.distance) {
            storage_[index] = storage_[index - 1];
            --index;
        }
        storage_[index] = hit;
        ++count_;
        return true;
    }

    void clear() { count_ = 0; overflowed_ = false; }

    /* Discards everything past the first blocking hit.
     *
     * WHY A TRACE NEEDS THIS AT THE END RATHER THAN AS IT GOES. Contacts do not
     * arrive in distance order — a swept shape enters several cells at once, and
     * an overlap from one of them can be handed over before a nearer block from
     * another. So a trace collects, and then cuts: everything past the first
     * block is on the far side of something solid and did not happen.
     *
     * Cheap because the buffer is already sorted — it is a change to the count
     * and nothing else. */
    void dropBeyondFirstBlock()
    {
        for (int index = 0; index < count_; ++index) {
            if (storage_[index].response == Response::Block) {
                count_ = index + 1;
                return;
            }
        }
    }

    int count() const { return count_; }
    bool empty() const { return count_ == 0; }
    int capacity() const { return capacity_; }

    /* True when hits were discarded because the buffer was full. See the header
     * — this is checked, not assumed. */
    bool overflowed() const { return overflowed_; }

    const TraceHit& operator[](int index) const { return storage_[index]; }

    const TraceHit* begin() const { return storage_; }
    const TraceHit* end() const { return storage_ + count_; }

    /* The nearest blocking hit, or nullptr when the trace passed through
     * everything. The question "did it get there" — every overlap before this is
     * something the trace went through on the way. */
    const TraceHit* blocking() const
    {
        for (int index = 0; index < count_; ++index) {
            if (storage_[index].response == Response::Block) return &storage_[index];
        }
        return nullptr;
    }

private:
    TraceHit* storage_ = nullptr;
    int capacity_ = 0;
    int count_ = 0;
    bool overflowed_ = false;
};

/* Storage and the view over it, for the common call site that just wants a few
 * hits on the stack: `TraceHitBuffer<8> hits; traceMulti(..., hits.view());` */
template <int Capacity>
class TraceHitBuffer {
public:
    TraceHits view() { return TraceHits{ storage_, Capacity }; }

private:
    TraceHit storage_[Capacity];
};

}  // namespace cromwell
