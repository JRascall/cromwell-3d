/* RaylibInterop.hpp — the one place cromwell's types meet raylib's, and the
 * policy that keeps it one place.
 *
 * SINGLE RESPONSIBILITY: convert between the engine's value types and raylib's,
 * and be the only file that needs both.
 *
 * ===================== THE POLICY, BECAUSE IT HAS TO BE STATED ==============
 *
 * cromwell is meant to be lifted into the next project, and raylib is a choice
 * that project might not keep. That does not mean wrapping raylib — it means
 * knowing exactly where it is, so the surface is a knowable size rather than a
 * surprise. Two rules, and they are different for the two kinds of type:
 *
 *   VALUE TYPES — a position, a direction, a rotation, a ray, a colour, a
 *   rectangle. THE ENGINE OWNS THESE AND raylib's MUST NOT APPEAR IN AN
 *   INTERFACE. They are plain arithmetic, the headless half needs every one of
 *   them, and cromwell already defines its own: Vec2, Vec3, Quat, Ray, and the
 *   UI kit's colour and rect. Using raylib's spelling for one is not coupling
 *   so much as DUPLICATION — two names for the same four floats, converted back
 *   and forth at every boundary for no gain.
 *
 *   RESOURCE HANDLES — a texture, a shader, a mesh, a render target. THESE ARE
 *   THE RHI's, and raylib's spelling of them must not appear in an interface
 *   either. See cromwell/rhi/Handles.hpp. This rule REVERSED in August 2026 and
 *   the old one is reproduced below, because it is the more tempting answer and
 *   it needs refuting rather than deleting.
 *
 * ============ THE RULE THAT USED TO BE HERE, AND WHY IT WAS WRONG ==========
 *
 * This file used to say: "Texture2D, Shader, Mesh, Material, RenderTexture2D,
 * Font — THESE ARE RAYLIB'S AND STAY RAYLIB'S, in interfaces and all", on the
 * argument that a `cromwell::Texture` wrapping a `Texture2D` only changes a
 * type in a signature and does not change that the shadow pass, the tone map,
 * the decal projector and the PBR shader are written against GL through rlgl.
 * Swapping raylib means reimplementing those passes; the cost is the passes,
 * not the typedefs.
 *
 * EVERY SENTENCE OF THAT IS TRUE AND THE CONCLUSION DOES NOT FOLLOW.
 *
 * The passes being the cost is the argument FOR the interface, not against it.
 * With the passes written against IRenderDevice, two backends COEXIST and one
 * is chosen at startup. With them written against rlgl, a second backend means
 * rewriting the first in place, and there is no point in that process where the
 * tree builds and runs on both. The first is a port. The second is a rolling
 * outage, and it has to be lived through once per platform.
 *
 * It also silently assumed ONE target. cromwell ships to Windows and Linux
 * (GL 4.3), to macOS — where GL is deprecated, capped at 4.1, and has none of
 * the compute cromwell/gpu/compute already uses — and to consoles, whose APIs
 * are explicit, proprietary, and whose headers cannot be committed to this
 * repository at all. "Reimplement the passes" is not a one-off cost against
 * that list. It is a cost per platform, forever, paid in the place a bug is
 * most expensive to find.
 *
 * The old rule was also, in fairness, self-fulfilling: it is only true that a
 * wrapper "adds indirection for nothing" while the passes are still GL. Once
 * they are not, the handle types are what let one pass serve four backends.
 *
 * WHAT TO DO WHEN YOU ADD SOMETHING. If it is four floats, use the engine's
 * type. If it is a GPU object, use the RHI's handle — and if the thing you are
 * adding needs a raylib type in its signature to work, that is the signal it
 * belongs behind IRenderDevice rather than in front of it. If you find yourself
 * converting a value type in both directions inside one function, the interface
 * above it has the wrong type on it.
 *
 * WHAT THIS FILE IS NOW FOR: the value types only, and the shrinking set of
 * call sites still talking to raylib directly while the passes migrate. It is
 * scaffolding with a demolition date, not a permanent seam.
 *
 * ===========================================================================
 *
 * THE CONVERSIONS ARE FREE. Both layouts are floats in the same order, so these
 * compile to nothing; they exist to make the boundary visible rather than to do
 * work. INCLUDE THIS ONLY FROM CODE THAT ALREADY LINKS raylib — anything in
 * cromwell_base including it would defeat the whole arrangement.
 */
#pragma once

#include "raylib.h"

#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Ray.hpp"
#include "cromwell/math/Vec2.hpp"
#include "cromwell/math/Vec3.hpp"

namespace cromwell {

inline Vector2 toRaylib(Vec2 v) { return Vector2{ v.x, v.y }; }
inline Vec2    fromRaylib(Vector2 v) { return Vec2{ v.x, v.y }; }

inline Vector3 toRaylib(Vec3 v) { return Vector3{ v.x, v.y, v.z }; }
inline Vec3    fromRaylib(Vector3 v) { return Vec3{ v.x, v.y, v.z }; }

inline Quaternion toRaylib(Quat q) { return Quaternion{ q.x, q.y, q.z, q.w }; }
inline Quat       fromRaylib(Quaternion q) { return Quat{ q.x, q.y, q.z, q.w }; }

/* Rays cross more boundaries than anything else here — every pick, every trace,
 * every line-of-sight test — which is exactly why the engine owns the type. See
 * math/Ray.hpp. */
inline ::Ray toRaylib(const Ray& ray)
{
    return ::Ray{ toRaylib(ray.origin), toRaylib(ray.direction) };
}
inline Ray fromRaylib(const ::Ray& ray)
{
    return Ray{ fromRaylib(ray.position), fromRaylib(ray.direction) };
}

/* The layouts are asserted rather than assumed. If raylib ever reorders its
 * fields or pads them, this fails at compile time here instead of producing
 * geometry that is subtly wrong somewhere else. */
static_assert(sizeof(Vector2) == sizeof(Vec2), "Vec2 must match raylib's Vector2 layout");
static_assert(sizeof(Vector3) == sizeof(Vec3), "Vec3 must match raylib's Vector3 layout");
static_assert(sizeof(Quaternion) == sizeof(Quat), "Quat must match raylib's Quaternion layout");
static_assert(sizeof(::Ray) == sizeof(Ray), "Ray must match raylib's Ray layout");

}  // namespace cromwell
