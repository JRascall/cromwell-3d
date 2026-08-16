/* RenderFilter.hpp — how THIS GAME spends the renderer's filter bits.
 *
 * SINGLE RESPONSIBILITY: define what each bit of cromwell::FilterFlags means
 * here, and turn a CutawayView into the mask a cromwell::View hides with.
 *
 * ===================== THE ENGINE NEVER LEARNS WHAT A BIT IS ==============
 *
 * That is the whole design. Every renderable carries 32 game-defined flags,
 * every view carries 32 game-defined "hide these", and the engine draws where
 * the two do not intersect — one AND per renderable, with no idea that a storey
 * or a wall facing exists. This file is the entire vocabulary, it is the game's,
 * and a different project on cromwell would write a completely different one.
 *
 * See cromwell/render/Renderable.hpp for why the sense is HIDE-IF-ANY-MATCH
 * rather than the obvious show-if-any-match. The short version is that this
 * game has two independent cuts — a storey and a facing — a surface must pass
 * BOTH, and only the hide sense composes two axes in one AND.
 *
 * ==================== WHY THE BUDGET IS A BUILD ERROR =====================
 *
 * `1u << storey` is undefined past the field's width and misbehaves QUIETLY: no
 * error, no warning, just a storey whose bit landed on top of another one's, so
 * hiding the fifth floor also hides the ground. rhi/MIGRATION.md §4.12 lists
 * this as one of the five problems its design review found, and the answer it
 * asks for is exactly the static_assert below — a taller map becomes a build
 * error rather than a rendering mystery.
 *
 * THE RUNTIME CHECK BESIDE IT IS NOT REDUNDANT. Lattice's extents are runtime
 * values (a map is free to be any size), so the assert bounds the BUDGET and
 * the check catches a map that exceeds it. One is a promise about this file,
 * the other is a fact about the world that was loaded.
 */
#pragma once

#include "cromwell/render/Renderable.hpp"
#include "game/render/DrawLayers.hpp"
#include "cromwell/style/SurfaceFacing.hpp"
#include "game/render/scene/CutawayView.hpp"

namespace game {

/* ---- THE DEV PANEL'S CATEGORY SWITCHES ---------------------------------
 *
 * One bit per draw layer this game declares — statics, props, units, overlays,
 * movement rings, ring glow. See game/render/DrawLayers.hpp, which is where
 * they are named and where a new project replaces the lot.
 *
 * WHY THEY LIVE IN THE FILTER WORD AT ALL. Because the engine already draws
 * where a renderable's flags and a view's "hide these" do not intersect, and
 * "do not draw the units" is exactly that sentence. Inventing a second
 * mechanism — a per-layer list on the scene, or a `visible` bool the panel
 * walks the renderables to flip — would be a second answer to a question this
 * one already answers, and it is the answer the engine's own design note calls
 * the game EXTENDING the vocabulary rather than re-implementing it.
 *
 * THEY ARE HIDDEN IN EVERY DERIVED VIEW AND THE CUTAWAY IS NOT. See
 * View::withAlwaysHiddenFlags: a unit switched off must be out of the shadow
 * map too, or the switch answers "not the units" while a unit-shaped shadow is
 * still lying on the floor. */
inline constexpr int kDrawLayerBits = 6;

/* HOW MANY STOREYS THE CUTAWAY CAN ADDRESS — whatever is left after the facings
 * and the draw layers.
 *
 * SPENDING THE SPARE BITS ON STOREYS RATHER THAN LEAVING THEM SPARE is
 * deliberate: a bit reserved for nothing is a bit claimed by whoever needs one
 * next, and then the storey ceiling moves without anybody deciding it should.
 * This file said a future category "has to shrink this explicitly and say why",
 * and the draw layers are the first one to do it: 27 storeys became 21. That is
 * still seven times the demo map and seven times XCOM 2's WORLD_TotalLevels. */
inline constexpr int kMaxStoreys =
    cromwell::kFilterFlagBits - cromwell::kSurfaceFacingCount - kDrawLayerBits;

static_assert(kMaxStoreys + cromwell::kSurfaceFacingCount + kDrawLayerBits
                  <= cromwell::kFilterFlagBits,
              "the storey, facing and draw-layer bits must fit in one filter word - "
              "see cromwell::kFilterFlagBits");

/* WHICH STOREY THIS RENDERABLE IS ON. Out of range returns no bit at all, which
 * makes the renderable unhideable by the storey cut rather than aliasing onto
 * somebody else's bit — the failure that is visible rather than the one that
 * looks like a different bug. */
inline constexpr cromwell::FilterFlags storeyFlag(int storey)
{
    return (storey >= 0 && storey < kMaxStoreys)
         ? (cromwell::FilterFlags{ 1 } << storey) : cromwell::FilterFlags{ 0 };
}

/* WHICH WAY THIS SURFACE FACES, in the bits above the storeys. `None` gets a
 * bit like any other facing and is never cut, because a cutaway's facing mask
 * always contains it — see CutawayView. */
inline constexpr cromwell::FilterFlags facingFlag(cromwell::SurfaceFacing facing)
{
    return cromwell::FilterFlags{ 1 } << (kMaxStoreys + static_cast<int>(facing));
}

/* WHICH DRAW LAYER THIS RENDERABLE BELONGS TO, in the bits above the facings.
 *
 * A layer outside the budget gets no bit, which makes it UNHIDEABLE rather than
 * aliased onto somebody else's — the same choice storeyFlag makes and for the
 * same reason: a switch that does nothing is a bug you can see, and a switch
 * that hides the wrong thing is one you attribute to the renderer. */
inline constexpr cromwell::FilterFlags layerFlag(cromwell::DrawLayerId layer)
{
    const int index = layer.index();
    return (index >= 0 && index < kDrawLayerBits)
         ? (cromwell::FilterFlags{ 1 } << (kMaxStoreys + cromwell::kSurfaceFacingCount + index))
         : cromwell::FilterFlags{ 0 };
}

/* ONE SURFACE'S WHOLE IDENTITY, as far as the cutaway is concerned. */
inline constexpr cromwell::FilterFlags surfaceFlags(int storey, cromwell::SurfaceFacing facing)
{
    return storeyFlag(storey) | facingFlag(facing);
}

/* AND THE SAME FOR A SURFACE THAT IS ALSO IN A CATEGORY. Statics are the only
 * producer that needs all three; bodies and overlays carry a layer bit alone,
 * because neither is cut by storey or by facing. */
inline constexpr cromwell::FilterFlags surfaceFlags(int storey, cromwell::SurfaceFacing facing,
                                                    cromwell::DrawLayerId layer)
{
    return storeyFlag(storey) | facingFlag(facing) | layerFlag(layer);
}

/* WHAT A VIEW WITH THESE LAYERS SWITCHED ON REFUSES TO DRAW — every declared
 * layer that is NOT in the mask.
 *
 * ENUMERATED OVER THE BUDGET RATHER THAN OVER THE MASK, because a mask's "off"
 * bits are the answer and `DrawLayerMask::all()` has thirty-two of them, only
 * six of which this game has named. Walking the budget is what keeps a bit
 * nobody declared from hiding renderables that never carried it. */
inline cromwell::FilterFlags hiddenByLayers(cromwell::DrawLayerMask drawn)
{
    cromwell::FilterFlags hidden = 0;

    for (int i = 0; i < kDrawLayerBits; i++) {
        const cromwell::DrawLayerId layer{ i };
        if (!drawn.has(layer)) hidden |= layerFlag(layer);
    }
    return hidden;
}

/* WHAT A VIEW OF THIS CUTAWAY REFUSES TO DRAW.
 *
 * The two cuts are complementary statements: the storey cut removes what is
 * ABOVE you, the facing cut removes what is IN FRONT of you, and between them
 * they open a building to the camera. CutawayView.hpp is where the policy lives
 * and why they travel together; this only converts its answer into bits.
 *
 * THE SUN AND THE PROBES NEVER CALL THIS, and after the render scene they
 * cannot: their views are derived by the engine and carry no hidden flags at
 * all. That used to be a rule a submitter had to remember, and forgetting it
 * made the lighting change when the player changed floor. */
inline cromwell::FilterFlags hiddenBy(const CutawayView& cutaway)
{
    cromwell::FilterFlags hidden = 0;

    for (int storey = 0; storey < kMaxStoreys; storey++)
        if (storey > cutaway.maxStorey) hidden |= storeyFlag(storey);

    for (int i = 0; i < cromwell::kSurfaceFacingCount; i++) {
        const auto facing = static_cast<cromwell::SurfaceFacing>(i);
        if (!cutaway.shows(facing)) hidden |= facingFlag(facing);
    }
    return hidden;
}

}  // namespace game
