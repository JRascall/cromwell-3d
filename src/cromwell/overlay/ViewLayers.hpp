/* ViewLayers.hpp — what a camera draws, and how.
 *
 * SINGLE RESPONSIBILITY: carry one camera's rendering settings — a mask of the
 * GAME's draw layers, and a switch per rendering feature the ENGINE implements.
 * Nothing here decides anything; the renderer reads it and skips.
 *
 * ================= THE LINE BETWEEN THE TWO, AND IT IS CHECKABLE ===========
 *
 * IF cromwell OWNS THE PASS, IT IS A FEATURE. IF THE GAME OWNS IT, IT IS A DRAW
 * LAYER.
 *
 * That rule is the whole design. Shadows, reflections, occlusion, decals, the
 * sky, the custom-depth target and debug geometry are all implemented in this
 * engine — a project embedding cromwell gets them whether or not it has ever
 * heard of a soldier. They are named here because they exist here.
 *
 * Everything else is not the engine's business. This file used to carry
 * `units`, `props`, `statics`, `movementRings` and `glow`, and that was simply
 * wrong: cromwell is meant to be lifted into an RTS, an FPS and a
 * third-person game, and none of those has movement rings. An engine that
 * shipped a `units` switch would have decided that every game embedding it has
 * units.
 *
 * So the categories of thing a game draws are the GAME's, declared the same way
 * collision layers already were — 32 anonymous slots, named by the project. See
 * math/Mask.hpp for the mechanism and the game's own draw-layer header for a
 * worked example. The dev panel enumerates whatever was registered, so a new
 * project's layers appear in it without anybody editing a panel.
 *
 * ========================= FEATURES ARE PER CAMERA =========================
 *
 * All of them cost something and not every camera needs them. Two — ambient
 * occlusion and decals — are SCREEN SPACE, reconstructed from a depth prepass
 * rendered from one viewpoint at one resolution, so they belong to a camera
 * rather than to the frame. A camera rendering to its own texture allocates its
 * own prepass when its features ask for one; see `needsDepthPrepass` and
 * camera/Camera.hpp.
 *
 * A switch turned off is off in EVERY pass, including the shadow map: a unit
 * hidden from the camera but still laying a shadow across the floor would be a
 * worse debugging tool than no switch at all.
 *
 * NO HUD SWITCHES HERE either. Whether a minimap plate is composited over the
 * finished frame is not a property of any camera; it lives with the game's
 * interface settings.
 */
#pragma once

#include "cromwell/math/Mask.hpp"

namespace cromwell {

/* Draw layers are their own KIND of 32-category mask — an id from the collision
 * set is meaningless here and the compiler says so. See math/Mask.hpp. */
struct DrawLayerTag {};

using DrawLayerId = MaskId<DrawLayerTag>;
using DrawLayerMask = Mask<DrawLayerTag>;
using DrawLayerNames = MaskNames<DrawLayerTag>;

/* The rendering features cromwell itself implements.
 *
 * A NAMED STRUCT AND NOT A MASK, deliberately: unlike draw layers these are a
 * FIXED set that the engine knows the meaning of, so a field with a name is
 * better than a bit with a registered label. A project cannot add one without
 * adding the pass that implements it, at which point it is editing this file
 * anyway. */
struct RenderFeatures {
    /* The analytic sky. Drawn before the 3D mode, under everything. */
    bool sky = true;

    /* The sun's shadow map, sampled and drawn. World-space, so it is as valid
     * from a second camera as from the first. */
    bool shadows = true;

    /* Reflection probes, CAPTURE AND SAMPLING ALIKE. Gating only the capture
     * would freeze the last cubemaps rather than disable them — a switch that
     * cannot answer "is this the reflections?" is worse than no switch, because
     * it answers "no" wrongly. Also world-space. */
    bool reflections = true;

    /* SCREEN SPACE. Reconstructed from a depth prepass, so it belongs to one
     * viewpoint at one resolution — a camera rendering to a texture needs its
     * own prepass for this to mean anything, which asking for it allocates.
     * Off, the lit shader samples 1x1 white and never has to know the effect is
     * missing. */
    bool ambientOcclusion = true;

    /* SCREEN SPACE, same story: the DBuffer is written by unprojecting the
     * prepass depth. Both halves gated, for the same reason `reflections` gates
     * both. */
    bool decals = true;

    /* Tagged objects rasterised into their own depth/stencil target, each with
     * an id, for outlines and anything else that needs to single objects out. */
    bool customDepth = true;

    /* Whatever any system queued into DebugDraw this frame. On by default and
     * that is safe: an empty queue costs one branch, and nothing is drawn
     * unless some code explicitly asked for it. */
    bool debugDraw = true;

    /* The filmic curve in the resolve. OFF MEANS RAW: the camera's linear
     * radiance is blitted to its output unchanged (clamped by the format),
     * with the supersample collapse still applied — for a capture feeding a
     * shader that wants radiance rather than display colour, or a project
     * that grades its own image. Per camera like everything else here: one
     * pane can be graded while another is raw. */
    bool toneMap = true;
};

struct ViewLayers {
    /* WHICH OF THE GAME'S CATEGORIES THIS CAMERA DRAWS. Defaults to all of
     * them, so a project that never registers a layer gets everything and the
     * system stays out of its way until it wants it. */
    DrawLayerMask draw = DrawLayerMask::all();

    /* How what it draws is shaded. */
    RenderFeatures features;

    /* Reads better at a call site than `layers.draw.has(...)` and is the form
     * every pass uses. */
    bool drawing(DrawLayerId layer) const { return draw.has(layer); }

    ViewLayers& show(DrawLayerId layer) { draw = draw.with(layer); return *this; }
    ViewLayers& hide(DrawLayerId layer) { draw = draw.without(layer); return *this; }

    /* Everything the engine can draw, and every category the game has. The
     * player's view, and the only preset the ENGINE can offer — anything more
     * specific ("the world without the interface") needs to know what the
     * game's layers mean, so those presets live with the game's declarations. */
    static ViewLayers all() { return ViewLayers{}; }

    /* No shading features at all, every layer still drawn. For a silhouette or
     * a mask pass — the cheapest thing that still renders the world, and raw:
     * a mask wants the values it wrote, not a graded picture of them.
     *
     * BY NAME, NOT BY POSITION — this was a brace list of seven `false`s, and
     * every field added to RenderFeatures silently meant something different
     * by its position in it. */
    static ViewLayers unshaded()
    {
        ViewLayers layers;
        layers.features.sky = false;
        layers.features.shadows = false;
        layers.features.reflections = false;
        layers.features.ambientOcclusion = false;
        layers.features.decals = false;
        layers.features.customDepth = false;
        layers.features.debugDraw = false;
        layers.features.toneMap = false;
        return layers;
    }

    /* True when this camera needs a depth prepass of its own — which is exactly
     * "does it want either of the screen-space features".
     *
     * DERIVED RATHER THAN A SEPARATE SWITCH, and that is the whole point. There
     * used to be a `withScreenSpaceEffects` beside the feature flags, which
     * meant asking for occlusion and silently not getting it was one forgotten
     * call away. Camera reads this to decide what to allocate, so ASKING is what
     * makes it work — there is no second thing to remember. */
    bool needsDepthPrepass() const
    {
        return features.ambientOcclusion || features.decals;
    }
};

}  // namespace cromwell
