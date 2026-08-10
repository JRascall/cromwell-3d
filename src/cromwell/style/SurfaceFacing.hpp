/* SurfaceFacing.hpp — which way a surface's OUTWARD side points.
 *
 * SINGLE RESPONSIBILITY: name the four axis directions a wall can face away
 * from the space it encloses, plus "no facing at all", so geometry can be
 * bucketed by them at bake time.
 *
 * WHAT THIS IS FOR. A cutaway that lets the camera see into a building has to
 * remove the walls BETWEEN the camera and the interior and leave the ones
 * behind it — remove both and the building loses its backdrop and reads as a
 * floating floor. Deciding that per frame means knowing, for each wall, which
 * side of it is the inside. That is a question about the world, it never
 * changes unless the world does, and it is therefore answered once when the
 * geometry is built rather than every frame.
 *
 * SO THIS IS A SECOND BATCHING AXIS, alongside SurfaceKind. Kind splits by
 * what a surface is made of because a draw call binds one texture set; facing
 * splits by which way it points because a cutaway drops whole directions at a
 * time. Both are decided at bake time and both turn a per-frame decision into
 * "stop iterating", which is the same trick the storey split already plays.
 *
 * WORLD AXES, NOT COMPASS POINTS. The engine has no idea which way north is,
 * and a tile game's north is its own business — it maps its Dir onto these
 * when it emits. Naming them PlusX/MinusZ keeps this usable by an RTS whose
 * buildings sit at arbitrary angles, which would bucket by whichever axis its
 * wall is nearest.
 *
 * `None` IS NOT A DIRECTION, it is "never cut me". Floors, ramps, blocked
 * mass, crates, ladders and any wall whose outward side could not be
 * determined all land here. It is the safe default in the exact sense that
 * matters: a surface nobody classified stays visible, so a mistake in the
 * classifier costs a wall that fails to open, never a hole in the world.
 */
#pragma once

#include <cstddef>

namespace cromwell {

enum class SurfaceFacing : int {
    None = 0,
    PlusX,
    MinusX,
    PlusZ,
    MinusZ,
    Count
};

inline constexpr int kSurfaceFacingCount = static_cast<int>(SurfaceFacing::Count);

inline constexpr std::size_t indexOf(SurfaceFacing facing)
{
    return static_cast<std::size_t>(facing);
}

/* One bit per facing, for the mask a cutaway carries. */
inline constexpr unsigned bitOf(SurfaceFacing facing)
{
    return 1u << static_cast<unsigned>(facing);
}

/* Every facing shown — the world as it is, with nothing cut. */
inline constexpr unsigned kAllFacings = (1u << static_cast<unsigned>(SurfaceFacing::Count)) - 1u;

/* The outward direction as a world-space vector, for testing against a view
 * direction. `None` returns the zero vector, which no dot-product test can
 * push below a negative threshold — that is what makes unclassified geometry
 * uncuttable by construction rather than by a special case at every call. */
inline constexpr float facingX(SurfaceFacing facing)
{
    return facing == SurfaceFacing::PlusX ?  1.0f
         : facing == SurfaceFacing::MinusX ? -1.0f
                                           :  0.0f;
}

inline constexpr float facingZ(SurfaceFacing facing)
{
    return facing == SurfaceFacing::PlusZ ?  1.0f
         : facing == SurfaceFacing::MinusZ ? -1.0f
                                           :  0.0f;
}

}  // namespace cromwell
