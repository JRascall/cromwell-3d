#include "cromwell/material/MaterialDefinition.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/material/PbrMaterial.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace cromwell {
namespace {

/* THE KEYS, AND THEY ARE PbrMaterial'S FIELD NAMES.
 *
 * Deliberately not prettier names. A material file and the struct it fills
 * should be greppable from one another — someone reading `edgeFalloff` in a
 * .mat should find its meaning by searching the header, and someone adding a
 * field to the header should find every file that sets it. A translation layer
 * of friendly names would be one more thing to keep in step and one more place
 * for a rename to go half-done. */
bool readVector3(std::istringstream& line, Vector3& out)
{
    Vector3 value;
    if (!(line >> value.x >> value.y >> value.z)) return false;
    out = value;
    return true;
}

bool readAlphaMode(std::istringstream& line, AlphaMode& out)
{
    std::string word;
    if (!(line >> word)) return false;

    /* THE NAMES A MATERIAL EDITOR WOULD USE, not the enum's spelling. This is
     * the one place a friendly name is worth it, because "blend translucent"
     * is what an artist means and `Blend` is what the code calls it. */
    if (word == "opaque")      { out = AlphaMode::Opaque; return true; }
    if (word == "masked")      { out = AlphaMode::Mask;   return true; }
    if (word == "translucent") { out = AlphaMode::Blend;  return true; }

    /* The enum's own spellings too, so a file written from the header rather
     * than from the docs still works. */
    if (word == "mask")  { out = AlphaMode::Mask;  return true; }
    if (word == "blend") { out = AlphaMode::Blend; return true; }
    return false;
}

}  // namespace

bool loadMaterialDefinition(const char* name, PbrMaterial& out)
{
    if (name == nullptr) return false;

    const std::string path =
        std::string(ShaderLibrary::assetRoot()) + "/materials/" + name + ".mat";

    std::ifstream file(path);
    if (!file) return false;   /* the ordinary case — defaults stand */

    int lineNumber = 0;
    int applied = 0;
    std::string text;

    while (std::getline(file, text)) {
        lineNumber++;

        /* Strip a trailing comment, then skip anything left blank. `#` mid-line
         * so a value can be annotated where it is set, which is where the
         * reason for a number belongs. */
        const std::size_t hash = text.find('#');
        if (hash != std::string::npos) text.erase(hash);

        std::istringstream line(text);
        std::string key;
        if (!(line >> key)) continue;

        bool ok = true;

        if      (key == "blend")          ok = readAlphaMode(line, out.alphaMode);
        else if (key == "roughness")      ok = static_cast<bool>(line >> out.roughness);
        else if (key == "metalness")      ok = static_cast<bool>(line >> out.metalness);
        else if (key == "normalStrength") ok = static_cast<bool>(line >> out.normalStrength);
        else if (key == "uvScale")        ok = static_cast<bool>(line >> out.uvScale);
        else if (key == "alphaCutoff")    ok = static_cast<bool>(line >> out.alphaCutoff);

        /* ---- opacity, and the Fresnel ramp over it ---------------------- */
        else if (key == "baseOpacity")    ok = static_cast<bool>(line >> out.baseOpacity);
        else if (key == "edgeThickness")  ok = static_cast<bool>(line >> out.edgeThickness);
        else if (key == "edgeFalloff")    ok = static_cast<bool>(line >> out.edgeFalloff);
        else if (key == "edgeMaxOpacity") ok = static_cast<bool>(line >> out.edgeMaxOpacity);
        else if (key == "opacityScale")   ok = static_cast<bool>(line >> out.opacityScale);
        else if (key == "edgeColour")     ok = readVector3(line, out.edgeColour);

        /* ---- light passed through from the far side --------------------- */
        else if (key == "transmissionColour") ok = readVector3(line, out.transmissionColour);
        else if (key == "transmissionAmount")
            ok = static_cast<bool>(line >> out.transmissionAmount);

        /* ---- what the SUN loses crossing the surface --------------------
         *
         * A DIFFERENT QUESTION FROM THE TWO ABOVE, and worth keeping straight.
         * `transmissionColour` and `transmissionAmount` are what the EYE sees
         * looking at a backlit surface. These are what the light itself becomes
         * on the far side, and they are read in the SHADOW pass — which is what
         * makes a window cast a coloured patch on the floor instead of a hole.
         *
         * Split into a pure hue and a scalar dimming because that is how
         * PbrMaterial carries it; the device packs their product. */
        else if (key == "transmissionTint")   ok = readVector3(line, out.transmissionTint);
        else if (key == "paneTransmittance")
            ok = static_cast<bool>(line >> out.paneTransmittance);

        else {
            LOGGER.warn("material '{}' line {}: unknown key '{}'", name, lineNumber, key);
            continue;
        }

        if (!ok) {
            LOGGER.warn("material '{}' line {}: '{}' could not be read", name, lineNumber, key);
            continue;
        }

        applied++;
    }

    LOGGER.info("material '{}' loaded from {} - {} value(s)", name, path, applied);
    return true;
}

}  // namespace cromwell
