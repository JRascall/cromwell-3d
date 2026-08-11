/* SplitScreen.hpp — how a window divides into player panes.
 *
 * SINGLE RESPONSIBILITY: name the standard splitscreen layouts and answer
 * "where does pane N go" in pixels. Nothing here renders, owns a camera, or
 * knows what a player is — it is the arithmetic of dividing a rectangle,
 * shared so every consumer divides it the same way.
 *
 * ======================= NAMING, STATED TO BE SAFE =========================
 *
 * "HORIZONTAL" AND "VERTICAL" NAME THE SPLIT LINE, NOT THE PANES — the
 * console convention, and the one every couch player already knows:
 *
 *   TwoHorizontal   a horizontal line across the middle: TOP and BOTTOM.
 *                   The classic racing split — both panes stay wide.
 *   TwoVertical     a vertical line down the middle: LEFT and RIGHT.
 *                   Better for tall views; each pane keeps its height.
 *
 * Three and Four are the 2x2 grid. Three leaves the BOTTOM-RIGHT cell empty
 * rather than stretching anybody's pane — that is where shipped three-player
 * games put the shared map or the scoreboard, and a game that wants that
 * writes into the hole; paneRect never hands the empty cell out.
 *
 * ====================== WHY PANES ARE CAMERAS, NOT MODES ===================
 *
 * A pane is a Camera rendering to a texture the size of its rectangle, drawn
 * into place with drawTo — the same path a minimap or a PiP takes, because a
 * splitscreen pane IS a PiP that happens to tile the window. Panes that tile
 * the window shade about the same pixel count as one fullscreen view, so the
 * cost is the per-pane extras (a resolve each, screen-space buffers where a
 * pane's layers ask), not a doubling. Each pane keeps everything a Camera
 * has: its own layers, projection, schedule and clickable viewportAt.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

enum class SplitLayout : int { Single = 0, TwoHorizontal, TwoVertical, Three, Four };

inline int paneCount(SplitLayout layout)
{
    switch (layout) {
        case SplitLayout::TwoHorizontal:
        case SplitLayout::TwoVertical: return 2;
        case SplitLayout::Three:       return 3;
        case SplitLayout::Four:        return 4;
        default:                       return 1;
    }
}

/* Pane `index` (0-based, row-major: top-left first) of a `width` x `height`
 * window. An index past paneCount comes back empty rather than inventing a
 * pane — a caller iterating a stale count draws nothing instead of somewhere
 * wrong. */
inline Rectangle paneRect(SplitLayout layout, int index, float width, float height)
{
    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;

    switch (layout) {
        case SplitLayout::TwoHorizontal:
            if (index == 0) return Rectangle{ 0.0f, 0.0f, width, halfH };
            if (index == 1) return Rectangle{ 0.0f, halfH, width, halfH };
            break;
        case SplitLayout::TwoVertical:
            if (index == 0) return Rectangle{ 0.0f, 0.0f, halfW, height };
            if (index == 1) return Rectangle{ halfW, 0.0f, halfW, height };
            break;
        case SplitLayout::Three:
        case SplitLayout::Four:
            if (index >= 0 && index < paneCount(layout)) {
                const float column = static_cast<float>(index % 2);
                const float row = static_cast<float>(index / 2);
                return Rectangle{ column * halfW, row * halfH, halfW, halfH };
            }
            break;
        default:
            if (index == 0) return Rectangle{ 0.0f, 0.0f, width, height };
            break;
    }
    return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
}

}  // namespace cromwell
