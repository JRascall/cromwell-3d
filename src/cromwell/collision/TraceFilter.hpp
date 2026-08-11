/* TraceFilter.hpp — what THIS query hits, and how it reacts.
 *
 * SINGLE RESPONSIBILITY: hold the two masks that turn a layer into a Response,
 * and be cheap enough to pass by value into a per-cell test.
 *
 * TWO MASKS RATHER THAN A TABLE OF THIRTY-TWO RESPONSES. A response is one of
 * three values, so a full per-layer table is 32 entries and a lookup; two masks
 * are eight bytes and two bit tests. The encoding is exact — Block wins over
 * Overlap, anything in neither is Ignore — and it costs nothing to pass around.
 * That matters because this is read inside the trace's inner loop, once per
 * candidate cell, which is precisely where CLAUDE.md's rule about map lookups in
 * hot loops applies.
 *
 * WHERE A FILTER COMES FROM. Usually not from here: `LayerMatrix::filterFor`
 * builds one from the project's response table, so a caller says "trace as a
 * bullet" and the rules about what a bullet stops at live in one place rather
 * than at every shot. Building one by hand is for the cases that genuinely are
 * one-off — a debug probe, an editor pick.
 *
 * IGNORING SPECIFIC INSTANCES IS NOT HERE, and that is deliberate. Unreal's
 * query params carry an actor ignore list, which is the right feature and the
 * wrong place for it: a list means a linear scan per candidate, inside the loop,
 * for a case that is almost always "ignore the thing firing the trace". The
 * trace functions take a single ignored id instead — one comparison — and a
 * caller that genuinely needs a set can filter the multi-hit results
 * afterwards, outside the loop, where a set lookup is free.
 */
#pragma once

#include "cromwell/collision/Layer.hpp"

namespace cromwell {

class TraceFilter {
public:
    constexpr TraceFilter() = default;

    constexpr TraceFilter(LayerMask blocking, LayerMask overlapping)
        : blocking_(blocking), overlapping_(overlapping) {}

    /* Stops at everything. The default a cursor pick or a line-of-sight test
     * wants, and the one to start from when unsure. */
    static constexpr TraceFilter blockAll() { return TraceFilter{ LayerMask::all(), LayerMask::none() }; }

    /* Stops only at these, ignores the rest. */
    static constexpr TraceFilter blockOnly(LayerMask blocking)
    {
        return TraceFilter{ blocking, LayerMask::none() };
    }

    /* Reports everything along the line and stops at nothing — the "what did
     * this pass through" query. Only meaningful with a multi-hit trace; a single
     * trace with this filter returns the first thing it met, which is the same
     * answer blockAll would have given more cheaply. */
    static constexpr TraceFilter overlapAll()
    {
        return TraceFilter{ LayerMask::none(), LayerMask::all() };
    }

    constexpr LayerMask blocking() const { return blocking_; }
    constexpr LayerMask overlapping() const { return overlapping_; }

    /* Every layer this query cares about at all. The cheap early rejection: a
     * candidate outside this is skipped before anything geometric happens, which
     * is CLAUDE.md's cull-cheaply-before-testing-expensively rule at its most
     * literal — one bit test against a slab intersection. */
    constexpr LayerMask relevant() const { return blocking_ | overlapping_; }

    constexpr Response responseTo(LayerId layer) const
    {
        if (blocking_.has(layer)) return Response::Block;
        if (overlapping_.has(layer)) return Response::Overlap;
        return Response::Ignore;
    }

    constexpr bool ignores(LayerId layer) const { return !relevant().has(layer); }

    /* Derived filters, for the small adjustments a call site legitimately makes
     * without wanting a whole new channel: a shot that should pass through the
     * cover it was fired from, a probe that should not stop at glass. */
    constexpr TraceFilter ignoring(LayerId layer) const
    {
        return TraceFilter{ blocking_.without(layer), overlapping_.without(layer) };
    }
    constexpr TraceFilter alsoOverlapping(LayerId layer) const
    {
        /* Not added to overlapping if it already blocks: Block is the stronger
         * response and downgrading it here would silently change what stops the
         * trace, which is the opposite of what "also report this" asks for. */
        return blocking_.has(layer) ? *this
                                    : TraceFilter{ blocking_, overlapping_.with(layer) };
    }

private:
    LayerMask blocking_;
    LayerMask overlapping_;
};

}  // namespace cromwell
