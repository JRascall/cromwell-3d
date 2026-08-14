/* UiFontAssets.hpp — where cromwell's typeface lives, for whoever draws the UI.
 *
 * SINGLE RESPONSIBILITY: resolve the engine's font files to paths that exist.
 * Nothing here rasterises, uploads or draws — it answers "where is Inter" and
 * "where is the icon face", and there are three separate things in this
 * project that need to ask.
 *
 * WHY THIS IS NOT A FONT MANAGER. Text gets drawn three unrelated ways here:
 * this ui/ widget kit through the painter, ImGui in the dev panel, and
 * Chromium in the browser surface. Each rasterises differently and none of
 * them can use the others' font objects, so the only thing they can share is
 * the files — and the icon codepoints, which is FontAwesomeIcons.hpp. A class
 * that tried to own ImFont and a CSS rule and an atlas would be three classes
 * wearing a coat.
 *
 * cromwell also deliberately does not link ImGui (see the note on the cromwell
 * target in CMakeLists.txt), so the ImGui binding could not live here even if
 * it wanted to; it is game/render/dev/DevFonts. The browser's is a generated
 * stylesheet. The widget kit's will be ui/text/UiFontSet, and this is what it
 * will be built on.
 *
 * THE FILES ARE NOT IN GIT. They are licensed binaries — Inter under the OFL,
 * Font Awesome Pro under a paid licence that permits embedding in a shipped
 * build and forbids redistributing the faces on their own. Every accessor here
 * therefore returns an empty string on a fresh checkout rather than asserting,
 * and every caller is expected to fall back to something legible rather than
 * fail. See assets/fonts/README.md for what to drop in.
 */
#pragma once

#include "cromwell/ui/core/UiText.hpp"

#include <string>

namespace cromwell::ui {

class UiFontAssets {
public:
    /* Every weight in FontWeight has a file here, and that is not a
     * coincidence to be maintained by hand — the widget kit's enum IS Inter's
     * weight list, so a second enumeration in this header would be a second
     * place for the two to disagree. When ui/text/UiFontSet arrives it asks
     * this the same question a widget already asks the layout: "SemiBold",
     * not a filename.
     *
     * Returns an empty string if the font pack is not installed. */
    static std::string inter(FontWeight weight);

    /* The icon faces. Solid carries the whole classic set; brands is company
     * logos — Steam, Discord — and is a separate file because Font Awesome
     * ships it separately, not because it is optional. A UI merging icons
     * wants both, in this order: 70 codepoints appear in both faces, and the
     * solid reading is the one FontAwesomeIcons.hpp documents. */
    static std::string iconSolid();
    static std::string iconBrands();

    /* file:// URL of the generated stylesheet that gives a CEF page the same
     * typeface and the same icon names, or empty if it has not been generated.
     * Produced by tools/fonts/gen_fa_icons.py, which also emits FontAwesomeIcons.hpp
     * — one script, so the two UIs cannot drift onto different codepoints.
     *
     * A URL rather than a path because the only consumer is WebSurface, and a
     * Windows path with backslashes and a drive letter is not a URL. */
    static std::string webStylesheetUrl();

    /* True when the faces resolved. Cheap; the paths are resolved once and
     * cached, because this is asked at UI setup and never in a frame. */
    static bool installed();
};

}  // namespace cromwell::ui
