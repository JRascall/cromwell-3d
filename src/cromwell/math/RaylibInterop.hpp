/* RaylibInterop.hpp — the one place cromwell's math types meet raylib's.
 *
 * SINGLE RESPONSIBILITY: convert between Vec3/Quat and raylib's Vector3/
 * Quaternion, and be the only file that needs both.
 *
 * WHY THE ENGINE HAS ITS OWN VECTOR AT ALL. cromwell's headless half — the
 * simulation, the entity system, every test — must build and run without
 * linking a window library. raylib's Vector3 arrives with raylib.h, which
 * arrives with a GL context's worth of expectations. So the engine owns its
 * types and converts at the boundary.
 *
 * THE CONVERSION IS FREE. Both layouts are floats in the same order, so these
 * compile to nothing; they exist to make the boundary visible rather than to do
 * work. INCLUDE THIS ONLY FROM CODE THAT ALREADY LINKS raylib — anything in
 * cromwell_base including it would defeat the whole arrangement.
 */
#pragma once

#include "raylib.h"

#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"

namespace cromwell {

inline Vector3 toRaylib(Vec3 v) { return Vector3{ v.x, v.y, v.z }; }
inline Vec3    fromRaylib(Vector3 v) { return Vec3{ v.x, v.y, v.z }; }

inline Quaternion toRaylib(Quat q) { return Quaternion{ q.x, q.y, q.z, q.w }; }
inline Quat       fromRaylib(Quaternion q) { return Quat{ q.x, q.y, q.z, q.w }; }

/* The layouts are asserted rather than assumed. If raylib ever reorders its
 * fields or pads them, this fails at compile time here instead of producing
 * geometry that is subtly wrong somewhere else. */
static_assert(sizeof(Vector3) == sizeof(Vec3), "Vec3 must match raylib's Vector3 layout");
static_assert(sizeof(Quaternion) == sizeof(Quat), "Quat must match raylib's Quaternion layout");

}  // namespace cromwell
