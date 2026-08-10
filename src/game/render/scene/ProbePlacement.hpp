/* ProbePlacement.hpp — where this game's reflection probes go.
 *
 * SINGLE RESPONSIBILITY: turn a flooded room partition into probe volumes and
 * hand them to the engine's probe set.
 *
 * WHY THIS IS NOT IN THE ENGINE. cromwell::ReflectionProbeSet owns the cubemap
 * array, the round-robin capture schedule and the GL underneath them — all of
 * which are true of any renderer. WHICH volumes to place is not: it depends on
 * there being rooms, on rooms being a flood fill over a cell lattice, and on a
 * tactical board having an "outdoors" that everything falls back to. Those are
 * facts about this game. The engine takes ProbeVolumes as data and never learns
 * what a room is.
 */
#pragma once

#include "raylib.h"

#include "cromwell/lighting/ReflectionProbeSet.hpp"

namespace game {

using namespace cromwell;

class RoomPartition;
class Lattice;

/* Rebuilds the probe list from a freshly flooded partition. Call after
 * anything that edits the world: destruction merges rooms, and a probe list
 * from before the wall came down describes a building that no longer exists.
 *
 * Placement only — it captures nothing. Every layer is stale afterwards. */
void placeProbes(ReflectionProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vector3 worldMinimum, Vector3 worldMaximum);

}  // namespace game
