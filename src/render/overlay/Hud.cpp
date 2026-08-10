#include "render/overlay/Hud.hpp"

#include "core/lattice/Lattice.hpp"
#include "render/style/Palette.hpp"

namespace xcom {

void Hud::draw(const HudModel& model) const
{
    /* Every y below is a layout constant; the offset slides the whole panel
     * clear of whatever owns the top of the screen this frame. */
    const int top = model.topOffset;

    DrawRectangle(8, 8 + top, 470, 166, palette::kHudPanel);

    DrawText("xcom-c  |  XCOM 2 lattice: z = 64uu cell", 18, 16 + top, 12, RAYWHITE);

    DrawText(TextFormat("selected: %s at (%d,%d) storey %d",
                        model.selectedName.c_str(),
                        model.selectedCell.x, model.selectedCell.y,
                        Lattice::storeyOfZ(model.selectedCell.z) + 1),
             18, 34 + top, 11, SKYBLUE);

    DrawText(TextFormat("1/2/3 storey [%d]  TAB ring [%s]  C cutaway [%s]  F1 dev view",
                        model.isoLevel + 1, model.ringOverrideName,
                        model.softCutaway ? "on" : "off"),
             18, 50 + top, 10, GRAY);

    DrawText(TextFormat("L los [%s]  V cover [%s]  G grenade [%s]  R reset",
                        model.losMode ? "on" : "off",
                        model.showCover ? "on" : "off",
                        model.grenadeArmed ? "ARMED" : "off"),
             18, 64 + top, 10, model.grenadeArmed ? ORANGE : GRAY);

    DrawText("LMB move / select   WASD pan (SHIFT fast)   MMB or ALT orbit   wheel zoom",
             18, 78 + top, 10, GRAY);

    const bool bothUp = model.visibleRings == RingMask::both();
    const char* shown = bothUp ? "both"
                      : model.visibleRings.contains(Ring::Sprint) ? "sprint" : "move";

    DrawText(TextFormat("move ring %d loops / %d edges   sprint ring %d / %d   (%s shown, %s solid)",
                        model.moveLoops, model.moveEdges,
                        model.sprintLoops, model.sprintEdges,
                        shown,
                        model.solidRing == Ring::Sprint ? "sprint" : "move"),
             18, 94 + top, 10, GRAY);

    if (model.hoverCell) {
        DrawText(TextFormat("hover (%d,%d) cell z=%d  cost %s%s",
                            model.hoverCell->x, model.hoverCell->y, model.hoverCell->z,
                            model.hoverCost ? TextFormat("%.1f", *model.hoverCost) : "-",
                            model.hoverRestOk ? "" : "  [cannot stop here]"),
                 18, 110 + top, 11, model.hoverRestOk ? LIME : ORANGE);
    }

    DrawText(TextFormat("sun [ ] azim %.0f  - = elev %.0f  O ao [%s]  B sun [%s]  F debug [%s]",
                        static_cast<double>(model.sunAzimuth),
                        static_cast<double>(model.sunElevation),
                        model.occlusionActive ? "on" : "off",
                        model.bakedSun ? "BAKED" : "shadow map",
                        model.debugView == 1 ? "GEOMETRY ONLY"
                        : model.debugView == 2 ? "PROBES"
                        : model.debugView == 3 ? "ROOMS"
                        : model.debugView == 4 ? "ROUGHNESS"
                        : model.debugView == 5 ? "OCCLUSION" : "off"),
             18, 126 + top, 10, model.debugView ? ORANGE : GRAY);

    /* Verbatim, and long, because it is meant to be READ OFF THE SCREEN or
     * pasted from F3 — a rounded-off camera is a different camera, and the
     * whole point is that somebody else can reproduce this exact view. */
    DrawText(TextFormat("F3 copies camera:  %s", model.cameraArgs.c_str()),
             18, 140 + top, 10, palette::kHudStatus);

    if (!model.status.empty())
        DrawText(model.status.c_str(), 18, 154 + top, 10, palette::kHudStatus);

    DrawFPS(18, 168 + top);
}

}  // namespace xcom
