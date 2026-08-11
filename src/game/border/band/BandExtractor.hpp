/* BandExtractor.hpp — walk a band's boundary into closed loops.
 *
 * SINGLE RESPONSIBILITY: emit one boundary edge per tile face with no
 * CONNECTED in-band neighbour, and chain those edges into loops using
 * BandConnectivity's corner rule.
 *
 * Every directed boundary edge has exactly one successor, so each walk is a
 * cycle; the visited set also guards against a malformed band.
 *
 * SUPPRESSION, and why a boundary walk needs it at all. The rings nest: the
 * sprint band CONTAINS the move band, so the two boundaries coincide exactly
 * wherever the frontier is set by geometry rather than by budget — the lip of
 * an upper storey's floor plate being the obvious case, where neither budget
 * is the thing stopping you. Two ribbons on one grid line is one ribbon plus a
 * z-fight, and the amber one wins by being drawn second, hiding the blue.
 *
 * So the outer ring is extracted with the inner band passed as `suppress`, and
 * every edge it shares is dropped. The walk still traverses them — the corner
 * rule needs the whole cycle to find its way round — but they are not emitted,
 * which breaks the cycle into OPEN runs. That is why Loop carries `closed` and
 * why the polyliner and the strip builder have to honour it.
 */
#pragma once

#include "game/border/band/Band.hpp"
#include "game/border/band/BandConnectivity.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {


class BandExtractor {
public:
    explicit BandExtractor(const World& world)
        : world_(world), connectivity_(world) {}

    void extract(const Band& band, LoopSet& out) { extract(band, nullptr, out); }

    /* As above, but every edge whose cell is in `suppress` is walked and not
     * emitted. Pass the inner ring's band when extracting the outer one; pass
     * nullptr (or use the overload) for a band that stands alone.
     *
     * `suppress` must be a SUBSET of `band` for the result to mean anything:
     * the guarantee being relied on is that a boundary edge of `band` whose
     * cell is in `suppress` is also a boundary edge of `suppress`, which holds
     * only because a subset cannot link a neighbour the superset did not. */
    void extract(const Band& band, const Band* suppress, LoopSet& out);

private:
    /* One traversed cycle into loops: whole, or split at the suppressed edges. */
    void emit(const Band* suppress, LoopSet& out);
    void emitLoop(int first, int count, bool closed, LoopSet& out) const;

    const World&     world_;
    BandConnectivity connectivity_;

    /* instance scratch — the C original used a file-scope array */
    std::vector<unsigned char> visited_;
    std::vector<EdgeId>        cycle_;   /* the walk in progress, before emitting */
};

}  // namespace game
