/* Glow.hpp — the emissive halo the bright UI wears.
 *
 * SINGLE RESPONSIBILITY: append, under a shape, the soft geometry that makes it
 * read as if it had bloomed.
 *
 * WHY A FAKE AT ALL. UI is composited after the scene's bloom, so a bright
 * accent-coloured ring gets none of the spill that a bright object in the world
 * would. Without something standing in for it, emissive-looking UI reads as
 * flat stickers laid over a lit scene. Post-processing the UI layer for real
 * would mean a separate render target, a downsample chain and a blur per frame
 * — for a halo around a handful of small shapes, geometry is a fraction of the
 * cost and needs no target at all.
 *
 * HOW IT IS BUILT. Three concentric rings offset along the outline's normals —
 * at the edge, at 40% of the glow radius, and at the full radius — quaded into
 * two bands whose alpha falls from the peak to zero. Two bands rather than one
 * because a single linear ramp reads as a flat grey skirt: real bloom falls off
 * fast near the source and then trails, and two segments approximate that
 * closely enough that nobody looks twice.
 *
 * ALWAYS APPEND IT BEFORE THE CRISP SHAPE. The draw list is painter-ordered, so
 * the halo has to go down first or it paints over the thing it is meant to be
 * spilling from.
 *
 * STRENGTH SATURATES rather than scaling linearly: at the kit's default of 1.5
 * the peak is about a fifth of the shape's own alpha, and turning the dial to
 * its maximum approaches, but never reaches, an opaque skirt. A linear dial
 * spends its top half in territory that looks like a bug.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"
#include "cromwell/ui/shape/Outline.hpp"

#include <algorithm>

namespace cromwell::ui::glow {

/* Peak halo alpha at the shape's edge — about 0.2 at strength 0.6, saturating
 * toward 1, scaled by the shape's own opacity so a fading control's halo fades
 * with it. */
inline float edgeAlpha(float strength, float shapeAlpha)
{
    return std::min(0.35f * strength, 1.0f) * shapeAlpha;
}

/* Appends the halo for a closed outline. No-ops when the strength, the radius
 * or the shape's alpha make it invisible, so callers can pass their dials
 * straight through without guarding each one. */
void addClosedOutline(UiDrawList& drawList, const Outline& outline,
                      float glowRadius, const UiColor& shapeColour, float strength);

}  // namespace cromwell::ui::glow
