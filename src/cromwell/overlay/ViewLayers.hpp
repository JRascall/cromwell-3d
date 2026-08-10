/* ViewLayers.hpp — which parts of the frame are drawn at all.
 *
 * SINGLE RESPONSIBILITY: carry one bool per renderable layer. Nothing here
 * decides anything; Application::drawFrame reads it and skips.
 *
 * SEPARATE FROM THE VIEW MODES on purpose. Cutaway, flat view and line of
 * sight change WHAT THE PICTURE MEANS and have keyboard shortcuts, side
 * effects and HUD readouts, so they travel as requests through applyInput.
 * These are plain "is this pass submitted", nothing else observes them, and
 * the dev panel edits them in place — a request round-trip for a bool that
 * only drawFrame ever reads would be ceremony with no payer.
 *
 * A layer switched off is off in EVERY pass, including the shadow map: a unit
 * hidden from the camera but still laying a shadow across the floor would be
 * a worse debugging tool than no switch at all.
 */
#pragma once

namespace cromwell {

struct ViewLayers {
    /* the world */
    bool sky      = true;
    bool statics  = true;   /* the lattice: floors, walls, ramps        */
    bool props    = true;
    bool units    = true;

    /* the lighting the world is drawn with */
    bool shadows  = true;   /* the sun's shadow map, sampled and drawn  */
    /* The reflection probes, CAPTURE AND SAMPLING ALIKE. It used to gate the
     * capture pass only, which made switching it off freeze the last cubemaps
     * rather than disable them — a toggle that cannot answer "is this the
     * reflections?" is worse than no toggle, because it answers "no" wrongly. */
    bool reflections = true;

    /* The custom depth/stencil pass — tagged objects rasterised into their own
     * target, each with an id and its own depth, for outlines and any other
     * effect that needs to single objects out. Nothing consumes it yet; it is
     * on because a capability nobody can see is one nobody will trust, and it
     * costs one pass over the units and props alone. */
    bool customDepth = true;

    /* Decals — the projector pass AND the lit shader's read of what it wrote.
     * Both halves, for the same reason `reflections` above gates both: gating
     * only the pass would leave the last frame's DBuffer bound and still
     * sampled, so the switch would freeze the decals instead of removing them,
     * and could never answer "is this the decals?" */
    bool decals = true;

    /* what is drawn on top of it */
    bool overlays = true;   /* LOS tint, cover shields, hover, path, blasts */
    bool ribbons  = true;   /* the movement rings                       */
    bool glow     = true;   /* their bloom, which is a separate pass    */
    bool hudText  = true;   /* the raylib text panel, not this one      */
};

}  // namespace cromwell
