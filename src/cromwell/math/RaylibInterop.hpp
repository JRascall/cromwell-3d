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
 *   RESOURCE HANDLES — Texture2D, Shader, Mesh, Material, RenderTexture2D,
 *   Font. THESE ARE RAYLIB'S AND STAY RAYLIB'S, in interfaces and all. They are
 *   handles to GPU and asset objects, they are only meaningful to code that
 *   already links raylib, and every one of them lives behind a pass that would
 *   have to be REWRITTEN for another backend anyway.
 *
 * ================= WHY WRAPPING THE HANDLES WOULD NOT HELP =================
 *
 * IT IS WORTH BEING HONEST ABOUT THIS, because "abstract the renderer" sounds
 * like insurance and mostly is not. A `cromwell::Texture` wrapping a
 * `Texture2D` changes the type in a signature; it does not change that the
 * shadow pass, the tone map, the decal projector and the PBR shader are written
 * against GL through rlgl. Swapping raylib means reimplementing those passes,
 * and the cost of that is the passes, not the typedefs. A wrapper layer would
 * add indirection to every one of them today in exchange for saving a rename on
 * a day that may not come.
 *
 * So the seam is DECLARED rather than abstracted. `Camera::toRaylib()`,
 * `toRaylib(Vec3)` and this file are the crossing points; a port replaces the
 * passes behind them, and everything above — the simulation, the traces, the
 * camera model, the UI geometry, the entity system — never mentioned raylib and
 * does not move.
 *
 * WHAT TO DO WHEN YOU ADD SOMETHING. If it is four floats, use the engine's
 * type. If it is a GPU object, raylib's is fine and belongs behind a pass. If
 * you find yourself converting a value type in both directions inside one
 * function, the interface above it has the wrong type on it.
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
