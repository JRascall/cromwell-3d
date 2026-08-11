/* LayerMatrix.hpp — the project's collision rules, in one table.
 *
 * SINGLE RESPONSIBILITY: hold what each layer is called and how each pair of
 * layers responds to each other, and hand out the trace filter that follows.
 *
 * THIS IS UNITY'S LAYER COLLISION MATRIX AND UNREAL'S CHANNEL RESPONSES, which
 * are the same table drawn two ways. Both engines put it in a settings screen
 * because it is configuration, not code: the rules change during development
 * far more often than the traces that consult them, and every time they are
 * written as conditionals instead they change in six places and disagree in a
 * seventh.
 *
 * WHY THE FILTER IS DERIVED RATHER THAN WRITTEN. A caller says "trace as a
 * bullet" and gets back the filter that the bullet layer's row implies — see
 * `filterFor`. That is the whole benefit: the question at the call site becomes
 * WHAT IS TRACING, which is a thing the caller genuinely knows, instead of WHAT
 * SHOULD IT STOP AT, which is a rule it should not be restating. Change what a
 * bullet passes through, and every shot in the game follows without being
 * touched.
 *
 * SYMMETRIC, and enforced rather than documented. `setResponse(a, b, r)` writes
 * both halves. An asymmetric matrix — A blocks B but B overlaps A — is
 * expressible in Unreal and is a reliable source of bugs that only appear from
 * one side; there is no case in this engine's three target genres that wants it,
 * and the pair that does can be modelled as two layers.
 *
 * NAMES ARE NOT DECORATION. A trace that returns nothing, or returns everything,
 * is debugged by printing what the filter was, and thirty-two bits of hex is not
 * an answer. The registry is what makes the dev panel able to say "bullet blocks
 * world, body; overlaps trigger" — which usually IS the bug, stated plainly.
 *
 * COLD CODE. Configured once at startup, read to build a filter at the top of a
 * query rather than inside it. The per-candidate test reads a TraceFilter's two
 * masks, never this. Do not put a LayerMatrix lookup in a loop; that is exactly
 * the hash-lookup-in-a-hot-loop mistake CLAUDE.md names, and `filterFor` exists
 * so it never has to happen.
 *
 * THE ENGINE REGISTERS NOTHING. There is no default set of layers, for the
 * reason set out in Layer.hpp: what a layer means is the project's decision.
 * A default-constructed matrix ignores everything, which fails loudly and
 * immediately rather than half-working.
 */
#pragma once

#include "cromwell/collision/Layer.hpp"
#include "cromwell/collision/TraceFilter.hpp"

#include <array>
#include <string>
#include <string_view>

namespace cromwell {

class LayerMatrix {
public:
    LayerMatrix();

    /* ---- declaring the layers ------------------------------------------- */

    /* Names a layer. The name is copied — unlike PointerFocus's claimants, this
     * outlives the frame and is built once at startup, so there is no reason to
     * make the caller guarantee a literal's lifetime. */
    void nameLayer(LayerId layer, std::string_view name);

    /* The registered name, or "layer N" for one that was never named. Never
     * empty, because the only caller is a diagnostic and a blank there is worse
     * than a placeholder. */
    std::string layerName(LayerId layer) const;

    /* Finds a layer by name, or an invalid id. For a console command or a config
     * file — NOT for per-frame code, which should hold the LayerId it was given
     * at startup. */
    LayerId findLayer(std::string_view name) const;

    /* ---- the rules ------------------------------------------------------ */

    /* Sets how `a` and `b` respond to each other. Symmetric — see the header. */
    void setResponse(LayerId a, LayerId b, Response response);

    /* Sets one layer's response to every layer at once, including itself. The
     * usual way a row is declared: "the world blocks everything", then a few
     * exceptions written over the top. */
    void setResponseToAll(LayerId layer, Response response);

    Response response(LayerId a, LayerId b) const;

    /* ---- what a query asks ---------------------------------------------- */

    /* The filter for a trace made BY `tracer` — that is, one whose rules are the
     * tracer layer's own row of this table. `SweepSingleByChannel(..., ECC_Bullet)`
     * by another name. */
    TraceFilter filterFor(LayerId tracer) const;

    /* Everything `layer` blocks, and everything it overlaps, as masks. Exposed
     * separately from filterFor because an object-versus-object test — an
     * overlap check, a spawn-point validity test — wants the mask without
     * wrapping it in a query filter. */
    LayerMask blockedBy(LayerId layer) const;
    LayerMask overlappedBy(LayerId layer) const;

    /* A one-line summary of a layer's rules, for the dev panel and the log:
     * "bullet: blocks [world, body] overlaps [trigger]". See the header on why
     * this earns its place. */
    std::string describe(LayerId layer) const;

private:
    /* Two masks per layer rather than a 32x32 byte matrix. Same information —
     * a response is one of three values, so a row is exactly a pair of masks —
     * in 256 bytes instead of 1024, and `blockedBy` becomes a read rather than a
     * loop that rebuilds a mask from bytes every time a filter is made. */
    std::array<LayerMask, LayerId::kCount> blocks_{};
    std::array<LayerMask, LayerId::kCount> overlaps_{};

    std::array<std::string, LayerId::kCount> names_{};
};

}  // namespace cromwell
