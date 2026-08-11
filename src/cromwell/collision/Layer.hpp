/* Layer.hpp — what a thing IS, for the purposes of being hit.
 *
 * SINGLE RESPONSIBILITY: name the categories the world is divided into, and say
 * how a query responds to each.
 *
 * THE PROBLEM. Every trace in a game is really two questions — "what is along
 * this line" and "which of those do I care about" — and the second one is
 * answered differently by every caller. A bullet stops at walls and at bodies
 * but not at the trigger volume it passes through. A camera collision probe
 * stops at walls and ignores bodies entirely, or the camera slams forward every
 * time a friendly walks past. A grenade's fuse trace wants to know about water.
 * A cursor pick wants floors but not the ceiling above them.
 *
 * Written as `if` statements at each call site, those rules end up scattered,
 * contradictory and impossible to change. Unity and Unreal both solve it the
 * same way and it is the right answer: give every object a LAYER, give every
 * query a set of layers it responds to, and put the rules in one table.
 *
 * ============================ THE THREE RESPONSES ==========================
 *
 * IGNORE, OVERLAP, BLOCK — Unreal's vocabulary, kept because the distinction is
 * real and two-valued filters keep having to grow into it.
 *
 *   IGNORE  — not even considered. Never appears in a result.
 *   OVERLAP — recorded as a hit, and the trace CONTINUES through it. Trigger
 *             volumes, water, smoke, foliage, an area a shot passes through and
 *             should be reported for.
 *   BLOCK   — recorded, and the trace STOPS. Walls, floors, solid bodies.
 *
 * OVERLAP IS WHAT MAKES A MULTI-HIT TRACE MEAN ANYTHING. Without it, "give me
 * everything along this line" is either a single blocking hit or an unfiltered
 * list, and the useful answer — every trigger the shot passed through, then the
 * wall it stopped at — cannot be expressed. See TraceHit.hpp, where the result
 * buffer is built around exactly that shape.
 *
 * ========================= THE MASK ITSELF IS SHARED ========================
 *
 * A MASK IS THE OPERATION EVERY CALLER ACTUALLY WANTS. "Walls or bodies", "not
 * this one thing", "everything the bullet channel blocks" — those are set
 * operations, and a 32-bit mask does them in one instruction.
 *
 * The bitset, the ids and the naming live in math/Mask.hpp, because DRAW layers
 * need exactly the same machinery and duplicating it would let the two drift.
 * This file adds only what is specific to collision: the three responses, and
 * the tag that keeps a collision layer from being passed where a draw layer
 * belongs. The engine still declares no layers of its own; that is the game's,
 * and LayerMatrix is where it records them.
 */
#pragma once

#include "cromwell/math/Mask.hpp"

#include <cstdint>

namespace cromwell {

/* How a query reacts to a layer. Ordered by severity so the strongest response
 * across several sources can be taken with a max — see TraceFilter. */
enum class Response : std::uint8_t {
    Ignore = 0,
    Overlap = 1,
    Block = 2,
};

/* Collision layers are one KIND of 32-category mask; draw layers are another,
 * and an id from one is meaningless in the other. Both are stamped from the
 * same template so the mechanism exists once and the compiler still refuses the
 * mix-up — see math/Mask.hpp, which also explains why the engine names none of
 * them itself. */
struct CollisionLayerTag {};

using LayerId = MaskId<CollisionLayerTag>;
using LayerMask = Mask<CollisionLayerTag>;

}  // namespace cromwell
