/* ProbePlacement.hpp — where this game's reflection probes go.
 *
 * SINGLE RESPONSIBILITY: turn a flooded room partition into probe volumes and
 * hand them to whichever probe set is asking.
 *
 * WHY THIS IS NOT IN THE ENGINE. Both probe sets own a cubemap array, a
 * round-robin capture schedule and the machinery underneath them — all of which
 * are true of any renderer. WHICH volumes to place is not: it depends on there
 * being rooms, on rooms being a flood fill over a cell lattice, and on a
 * tactical board having an "outdoors" that everything falls back to. Those are
 * facts about this game. The engine takes volumes as data and never learns what
 * a room is.
 *
 * ====================== ONE PLACEMENT, TWO PROBE SETS =====================
 *
 * placeProbeVolumes does the work and returns plain data; the two placeProbes
 * overloads are adapters that push it into a set. That shape is deliberate and
 * it is not premature generality — during the migration there are genuinely two
 * probe sets, and the placement rules below are the hard-won part. Writing them
 * twice would mean the outdoor volume's fallback, the four-cell minimum, the
 * largest-first drop order and the exact-bounds-no-margin decision each existed
 * in two copies, drifting apart one edit at a time, with the difference visible
 * only in reflections nobody A/Bs.
 *
 * At parity the raylib overload goes and the other two stay as they are.
 */
#pragma once

#include "raylib.h"

#include "cromwell/lighting/DeviceProbeSet.hpp"
#include "cromwell/lighting/ReflectionProbeSet.hpp"

#include <vector>

namespace game {

using namespace cromwell;

class RoomPartition;
class Lattice;

/* THE PLACEMENT ITSELF, as data. In WORLD space and in the engine's Vec3, so
 * nothing about the result names raylib.
 *
 * Volumes come back largest room first, already capped at the engine's probe
 * ceiling — so a caller that simply pushes them in order gets the right ones,
 * and a map with too many rooms loses its broom cupboards rather than its main
 * hall. */
std::vector<DeviceProbeSet::Volume> placeProbeVolumes(
    const RoomPartition& rooms, const Lattice& lattice,
    Vec3 worldMinimum, Vec3 worldMaximum);

/* Rebuilds a probe list from a freshly flooded partition. Call after anything
 * that edits the world: destruction merges rooms, and a probe list from before
 * the wall came down describes a building that no longer exists.
 *
 * Placement only — it captures nothing. Every layer is stale afterwards, which
 * is what the sets' markAllStale is for. */
void placeProbes(DeviceProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vec3 worldMinimum, Vec3 worldMaximum);

/* The raylib renderer's set. Deleted with FrameRenderer. */
void placeProbes(ReflectionProbeSet& probes,
                 const RoomPartition& rooms, const Lattice& lattice,
                 Vector3 worldMinimum, Vector3 worldMaximum);

}  // namespace game
