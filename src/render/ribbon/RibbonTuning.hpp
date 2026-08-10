/* RibbonTuning.hpp — the ribbon numbers, as live values rather than constants.
 *
 * SINGLE RESPONSIBILITY: carry the dials. RibbonConstants still states what
 * XCOM authored and why; this is a copy of those numbers that the dev panel is
 * allowed to move.
 *
 * TWO KINDS OF FIELD, and the difference matters to the caller. `width` and
 * `lift` are baked into vertices at build time, so changing either means
 * rebuilding the strips — Application watches for that. Everything else is a
 * uniform pushed per frame and takes effect on the next one.
 */
#pragma once

#include "raylib.h"

#include "render/ribbon/RibbonConstants.hpp"
#include "render/style/Palette.hpp"

namespace xcom {

struct RibbonTuning {
    /* ---- geometry: a change here needs the meshes rebuilt --------------- */
    Color moveColour   = palette::kRingMove;
    Color sprintColour = palette::kRingSprint;
    float width = kRibbonWidth;
    float lift  = kRibbonLift;

    /* ---- shading: uniforms, live on the next frame ---------------------- */
    float emissive    = kRibbonEmissive;
    int   glowSteps   = kRibbonGlowSteps;
    float glowGain    = kRibbonGlowGain;
    float glowFalloff = kRibbonGlowFalloff;
    float panSpeed    = kRibbonPanSpeed;

    /* Whether two tunings would produce different VERTICES. Colour counts:
     * it is per-strip metadata written when the strip is appended. */
    bool geometryDiffers(const RibbonTuning& other) const
    {
        return width != other.width || lift != other.lift ||
               moveColour.r != other.moveColour.r ||
               moveColour.g != other.moveColour.g ||
               moveColour.b != other.moveColour.b ||
               moveColour.a != other.moveColour.a ||
               sprintColour.r != other.sprintColour.r ||
               sprintColour.g != other.sprintColour.g ||
               sprintColour.b != other.sprintColour.b ||
               sprintColour.a != other.sprintColour.a;
    }
};

}  // namespace xcom
