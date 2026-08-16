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

    /* ---- NOT PART OF THE WORLD -----------------------------------------
     *
     * The interface drawn INTO the world: a visibility plate, a cover shield, a
     * hover marker, a path preview. It is geometry and it needs a material, so
     * it is a surface kind - but it is the only one here that describes nothing
     * the world is MADE of.
     *
     * IT EARNS ITS OWN KIND RATHER THAN BORROWING Portal's, which is the
     * nearest fit and would have been wrong twice over: a portal pad is real
     * geometry a player walks onto, and giving it the blend mode an overlay
     * needs would have turned every pad translucent to serve a marker. A
     * material is authored per kind, so two things wanting different materials
     * are two kinds.
     *
     * WHAT IT IS WAITING FOR is an UNLIT shading model. An overlay is a flat
     * colour a designer picked, and today every surface goes through one PBR
     * shader - so a plate lying on the ground picks up the sun. It reads
     * correctly and it is not what was authored. See rhi/MIGRATION.md 4.7:
     * `shading unlit` in the .mat is exactly this case, and it is the next job
     * rather than this one. */
    Overlay,

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
    /* The overlay joins the portal pad here for the same reason: it is not a
     * thing that physically intercepts light. A selection marker that cast a
     * shadow would be a soldier's own interface darkening the floor. */
    return kind != SurfaceKind::Window && kind != SurfaceKind::Portal
        && kind != SurfaceKind::Overlay;
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
        case SurfaceKind::Overlay: return "overlay";
        default:                  return "unknown";
    }
}

}  // namespace cromwell
