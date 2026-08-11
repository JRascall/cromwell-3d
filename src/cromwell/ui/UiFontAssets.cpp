#include "cromwell/ui/UiFontAssets.hpp"

#include "cromwell/gpu/ShaderLibrary.hpp"

#include "raylib.h"

#include <array>
#include <string>

namespace cromwell::ui {

namespace {

/* Reusing the shader search path is deliberate, not lazy. It already probes
 * both asset trees at all three depths the app can be launched from, and a
 * second copy of that list would be a second thing to keep in step the next
 * time the staging layout moves. The function is named for shaders because
 * that was its first caller; what it does is resolve a relative path against
 * cromwell's roots, which is what a font needs too. */
std::string resolve(const char* relativePath)
{
    const char* root = ShaderLibrary::rootContaining(relativePath);
    if (root == nullptr) return {};
    return std::string{ root } + "/" + relativePath;
}

/* Regular is the default rather than a case, which is the fallback UiText.hpp
 * promises: a weight with no file behind it should make the UI look slightly
 * wrong, never make the text disappear. */
const char* fileFor(FontWeight weight)
{
    switch (weight) {
        case FontWeight::Medium:     return "fonts/Inter/Inter-Medium.ttf";
        case FontWeight::SemiBold:   return "fonts/Inter/Inter-SemiBold.ttf";
        case FontWeight::Bold:       return "fonts/Inter/Inter-Bold.ttf";
        case FontWeight::ExtraBold:  return "fonts/Inter/Inter-ExtraBold.ttf";
        case FontWeight::Regular:    break;
    }
    return "fonts/Inter/Inter-Regular.ttf";
}

/* Percent-encoding, for the two characters a Windows path realistically
 * contains that a URL may not. Not a general encoder: a path that needs more
 * than this is a path CEF will reject in a way that says so plainly, which
 * beats a half-correct encoder that produces a URL pointing somewhere else. */
std::string toFileUrl(const std::string& path)
{
    std::string url = "file:///";
    for (const char c : path) {
        if (c == '\\')      url += '/';
        else if (c == ' ')  url += "%20";
        else if (c == '#')  url += "%23";
        else                url += c;
    }
    return url;
}

}  // namespace

std::string UiFontAssets::inter(FontWeight weight)
{
    return resolve(fileFor(weight));
}

std::string UiFontAssets::iconSolid()
{
    return resolve("fonts/FontAwesome/FontAwesome-ProSolid.otf");
}

std::string UiFontAssets::iconBrands()
{
    return resolve("fonts/FontAwesome/FontAwesome-Brands.otf");
}

std::string UiFontAssets::webStylesheetUrl()
{
    const std::string path = resolve("web/cromwell_ui.css");
    if (path.empty()) return {};

    /* The probe returns a path relative to the working directory, and a
     * file:// URL cannot be. GetWorkingDirectory is raylib's, so this stays on
     * the one path API the rest of the engine already uses. */
    if (path.size() > 1 && path[1] == ':') return toFileUrl(path);
    return toFileUrl(std::string{ GetWorkingDirectory() } + "/" + path);
}

bool UiFontAssets::installed()
{
    /* Regular and the solid face are the two a UI cannot do without: one draws
     * the text, the other draws every icon. The other weights are style. */
    static const bool present =
        !inter(FontWeight::Regular).empty() && !iconSolid().empty();
    return present;
}

}  // namespace cromwell::ui
