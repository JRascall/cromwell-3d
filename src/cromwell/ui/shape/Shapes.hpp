/* Shapes.hpp — the antialiased primitives every widget in the kit is built from.
 *
 * SINGLE RESPONSIBILITY: turn a described shape into triangles with a soft
 * edge, and nothing else. No state, no styling decisions, no knowledge of what
 * is being drawn.
 *
 * WHY EXACT GEOMETRY RATHER THAN A ROUNDED-BOX SHADER. This is the single most
 * important decision carried over from the PO widgets, and it was arrived at
 * the hard way. A signed-distance rounded-box shader is the obvious way to draw
 * a capsule or a ring, and it falls apart at the sizes UI actually uses:
 *
 *   - Its antialiasing spread is fixed in the shape's LOCAL space, so a 3px
 *     spoke gets the same absolute softening as a 300px panel and turns to
 *     mush.
 *   - Rotated shapes render visibly slimmer than axis-aligned ones, because the
 *     distance field is evaluated on a quad that no longer aligns with it. A
 *     ring of eight spokes shows this immediately: the diagonal ones look
 *     thinner and their caps look square.
 *
 * Exact vertices with a ONE-PIXEL FEATHER have neither problem. The geometry is
 * identical at every angle by construction, and the softening is one screen
 * pixel because that is what it is measured in. It costs more vertices and
 * nothing else — these are a few hundred triangles a frame.
 *
 * THE FEATHER IS ONE PIXEL, AND HERE IS WHY THERE IS NO SCALE DIVISION. The
 * Slate originals divide every dimension by the geometry scale, because UMG
 * applies a DPI transform between the widget's coordinates and the screen, and
 * a "1 pixel" feather authored in widget space would be 1.5 pixels at 150%
 * scaling. This kit has no such transform: the painter takes screen pixels and
 * draws screen pixels. So the division disappears and the constant below IS one
 * physical pixel. A game that wants DPI-scaled UI should scale the SPECS it
 * passes in — never the feather, which must stay one pixel to antialias.
 *
 * COLOURS: every builder takes the shape's colour and derives its own
 * transparent edge colour from it (see UiColor::toEdge, and the note there on
 * why fading to transparent-black would dirty every edge).
 *
 * FALLOFF IS A PARAMETER, NOT A CONSTANT, because the same builders draw the
 * halo passes: a glow is the same band with its soft edge spread eight pixels
 * instead of one. That reuse is why the ring's halo follows the arc's curve
 * exactly rather than approximating it.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/shape/Outline.hpp"

namespace cromwell::ui::shapes {

/* One physical screen pixel of antialiasing. See the header. */
inline constexpr float kFeatherPx = 1.0f;

/* How many angular samples a full circle is worth. Everything curved here picks
 * its sample count as a fraction of this, so arcs, discs and rings are all
 * tessellated at the same angular density and a ring drawn over a disc has no
 * visible faceting mismatch. 64 is smooth past any size UI uses. */
inline constexpr int kCircleSamples = 64;

/* Sample count for an arc of `sweepRadians`, never below `minimum` — a very
 * short arc still needs enough columns for its ends not to be a triangle. */
int arcSamples(float sweepRadians, int minimum = 8);

/* An annular band between two angles: the ring, the arc, the gauge chip.
 *
 * Per angular sample there are four vertices radially — soft-inner (alpha 0),
 * inner core, outer core, soft-outer (alpha 0) — quaded into three strips: the
 * inner falloff, the solid body, and the outer falloff. */
void addAnnularBand(UiDrawList& drawList, Vec2 centre,
                    float innerRadius, float outerRadius,
                    float fromAngle, float toAngle, int samples,
                    const UiColor& colour, float falloff);

/* A rounded arc end: a semicircular fan bulging past `capCentre`, softened on
 * its curve by `falloff`. `bulgeSign` is +1 to bulge along the sweep direction
 * and -1 against it; the flat side butts exactly against the band's end so the
 * two read as one shape. */
void addRoundCap(UiDrawList& drawList, Vec2 capCentre, float capRadius,
                 float atAngle, float bulgeSign,
                 const UiColor& colour, float falloff);

/* A filled circle: a fan to `radius` plus a feather band beyond it. */
void addDisc(UiDrawList& drawList, Vec2 centre, float radius,
             const UiColor& colour, float falloff);

/* A solid convex shape from its outline: a fan over the outline points plus a
 * feather ring offset along their normals. This is how a spinner spoke and a
 * filled segment chip are drawn. */
void addConvexFill(UiDrawList& drawList, const Outline& outline,
                   const UiColor& colour, float falloff);

/* A stroke along a closed outline, drawn INWARD from it by `thickness` — four
 * concentric rings (outer feather, outer edge, inner edge, inner feather)
 * quaded into three bands with only the middle one solid. This is how an empty
 * segment chip is drawn, and how any hollow box would be. */
void addOutlineStroke(UiDrawList& drawList, const Outline& outline, float thickness,
                      const UiColor& colour, float falloff);

/* A hard-edged axis-aligned rectangle, with NO feather.
 *
 * Deliberately unfeathered: an axis-aligned rect lands exactly on pixel
 * boundaries, so softening it only makes a crisp plate look like it has a
 * hairline seam around it — which is exactly the artefact the rounded-box
 * shader produced on square panels and the reason the originals stopped using
 * it for them. Anything rotated or curved wants one of the builders above. */
void addRect(UiDrawList& drawList, const UiRect& rect, const UiColor& colour);

/* A rectangle with rounded corners, feathered. Falls through to addRect at
 * radius 0, for the reason above. */
void addRoundedRect(UiDrawList& drawList, const UiRect& rect, float cornerRadius,
                    const UiColor& colour, float falloff);

/* The same with a radius per corner (TL, TR, BR, BL) — see the note on
 * Outline::buildRect for why stacked cards need it. */
void addRoundedRect(UiDrawList& drawList, const UiRect& rect, const float cornerRadii[4],
                    const UiColor& colour, float falloff);

}  // namespace cromwell::ui::shapes
