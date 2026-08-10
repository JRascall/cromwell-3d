/* SurfaceKind.hpp — the material classes the world is built from.
 *
 * SINGLE RESPONSIBILITY: name the distinct surfaces, so geometry, palette and
 * material can all be keyed off one enum instead of agreeing by convention.
 *
 * THIS IS THE BATCHING UNIT. Once surfaces carry textures they can no longer
 * all share one draw call, because a draw call binds one texture set. The
 * static world is therefore baked into one mesh per (storey, kind) — which is
 * how a level renderer batches, and what Source 2 does when it sorts a map's
 * geometry by material.
 *
 * The three floor tints are NOT three kinds. They differ only in colour, and
 * colour already travels per vertex; splitting them would triple the draw
 * calls to say something the vertex stream is saying for free.
 */
#pragma once

#include <cstddef>

namespace cromwell {

enum class SurfaceKind : int {
    Floor = 0,
    Road,
    Grass,
    Wall,
    Window,
    Cover,       /* sandbags and crates — anything half height */
    Ramp,
    Block,       /* solid impassable mass */
    Canopy,
    Portal,      /* a gameplay marker that happens to be geometry */
    Ladder,
    Body,        /* soldiers and vehicles */
    Count
};

inline constexpr int kSurfaceKindCount = static_cast<int>(SurfaceKind::Count);

inline constexpr std::size_t indexOf(SurfaceKind kind)
{
    return static_cast<std::size_t>(kind);
}

/* NOT EVERY VISIBLE SURFACE IS AN OCCLUDER.
 *
 * Glass is drawn, and glass transmits. The core's RayCaster has always known
 * this — sunlight and sight both pass through the window band — but the shadow
 * map only knows what was rasterised into it, so emitting the glass as
 * geometry made a window shade the room behind it like a bricked-up wall.
 * Filtering the shadow pass is what puts the two back in agreement.
 *
 * The portal pad is excluded for a different reason: it is a gameplay marker
 * lying flush on the floor, not something that physically intercepts light. */
inline constexpr bool castsSunShadow(SurfaceKind kind)
{
    return kind != SurfaceKind::Window && kind != SurfaceKind::Portal;
}

/* Surfaces you can see THROUGH, which have to be drawn after everything else.
 * A blended surface reads whatever is already in the colour buffer, so if it
 * is drawn before the wall behind it there is nothing to see through to — it
 * just looks like a tinted solid. */
inline constexpr bool isTransparent(SurfaceKind kind)
{
    return kind == SurfaceKind::Window;
}

/* Used for the asset filenames — see MaterialLibrary. */
inline const char* nameOf(SurfaceKind kind)
{
    switch (kind) {
        case SurfaceKind::Floor:  return "floor";
        case SurfaceKind::Road:   return "road";
        case SurfaceKind::Grass:  return "grass";
        case SurfaceKind::Wall:   return "wall";
        case SurfaceKind::Window: return "window";
        case SurfaceKind::Cover:  return "cover";
        case SurfaceKind::Ramp:   return "ramp";
        case SurfaceKind::Block:  return "block";
        case SurfaceKind::Canopy: return "canopy";
        case SurfaceKind::Portal: return "portal";
        case SurfaceKind::Ladder: return "ladder";
        case SurfaceKind::Body:   return "body";
        default:                  return "unknown";
    }
}

}  // namespace cromwell
