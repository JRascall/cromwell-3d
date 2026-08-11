/* DevFonts.hpp — Inter and Font Awesome, bound to ImGui.
 *
 * SINGLE RESPONSIBILITY: stand in for rlImGuiSetup, and leave the context with
 * the engine's typeface installed instead of ImGui's built-in bitmap one.
 *
 * WHY THIS IS GAME-SIDE AND THE FONTS ARE NOT. cromwell owns the typeface —
 * cromwell/ui/UiFontAssets resolves the files, cromwell/ui/FontAwesomeIcons.hpp
 * names the glyphs, and both are toolkit-agnostic. cromwell does not link
 * ImGui on purpose (CMakeLists.txt says why), so the eighty lines that turn
 * those files into an ImFont live here, next to the only panel that draws
 * with them. A future cromwell project using a different toolkit reuses the
 * fonts and writes its own eighty lines; it does not inherit ImGui to get a
 * font it was going to load anyway.
 *
 * FALLBACK IS NORMAL, NOT EXCEPTIONAL. The font pack is licensed and therefore
 * not in git, so a fresh checkout has no fonts at all. That case falls through
 * to plain rlImGuiSetup and the panel renders in ProggyClean — smaller and
 * uglier, entirely usable, and nothing needs a guard at the call sites.
 */
#pragma once

struct ImFont;

namespace game {

class DevFonts {
public:
    /* Replaces rlImGuiSetup. Creates the ImGui context, installs Inter with
     * the icon faces merged in, and applies the theme. Safe to call when the
     * fonts are missing — it degrades to rlImGuiSetup(darkTheme).
     *
     * Returns true if Inter was installed, false if it fell back. The caller
     * uses this for the log line, not for control flow: everything below works
     * either way. */
    static bool setup(bool darkTheme);

    /* Call once per frame, before anything is drawn. Re-reads the display
     * scale into ImGui's style so text follows the window between monitors of
     * different DPI — with 1.92's dynamic fonts that is a style change, not an
     * atlas rebuild, so it is cheap enough to do unconditionally. */
    static void update();

    /* Inter Regular with icons merged, and the ImGui default — so ordinary
     * ImGui::Text already draws in it and this is only needed to push it back
     * over a heading. Null before setup() and when the pack is missing, which
     * ImGui::PushFont treats as "the default font", so an unchecked push is
     * harmless. */
    static ImFont* text();

    /* Inter SemiBold, same size, icons merged. For section headers and the
     * one-word labels that carry a panel — a heavier weight reads as hierarchy
     * at a glance where a larger size just wastes vertical space in a panel
     * that has none to spare. */
    static ImFont* heading();

    /* The size everything was rasterised at, in pixels, after display scaling.
     * Wanted by layout that has to reserve a row before drawing into it. */
    static float sizePixels();
};

}  // namespace game
