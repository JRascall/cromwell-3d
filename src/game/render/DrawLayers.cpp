#include "game/render/DrawLayers.hpp"

namespace game {

using cromwell::ViewLayers;

cromwell::DrawLayerNames defaultDrawLayerNames()
{
    cromwell::DrawLayerNames names;
    names.name(drawLayer::kStatics, "world");
    names.name(drawLayer::kProps, "props");
    names.name(drawLayer::kUnits, "units");
    names.name(drawLayer::kOverlays, "overlays");
    names.name(drawLayer::kMovementRings, "rings");
    names.name(drawLayer::kRingGlow, "ring glow");
    return names;
}

ViewLayers allLayers()
{
    return ViewLayers::all();
}

ViewLayers worldOnly()
{
    ViewLayers layers;

    /* The whole annotations group at once, from the mask declared beside the
     * ids — so adding an annotation layer takes it out of every second camera
     * automatically, rather than needing this function edited too. That is the
     * failure a preset exists to prevent. */
    layers.draw = layers.draw & ~drawLayer::kAnnotations;

    /* Nothing consumes the custom-depth target off-screen; it exists for
     * full-screen outline effects on the player's view. */
    layers.features.customDepth = false;

    /* Debug geometry is a diagnostic for whoever is looking at the main view.
     * Drawing it into a security feed would put somebody's trace lines in a
     * picture nobody is debugging. */
    layers.features.debugDraw = false;

    return layers;
}

ViewLayers planView()
{
    ViewLayers layers = worldOnly();
    layers.features.sky = false;
    return layers;
}

}  // namespace game
