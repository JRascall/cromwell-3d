#include "cromwell/sdf/MsdfFont.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace cromwell::sdf {

namespace {

/* The shader search path resolves any relative asset against both trees at
 * every depth the app can be launched from — see the note on kRoots in
 * ShaderLibrary.cpp. Fonts want exactly that and a second copy of the list
 * would be a second thing to keep in step. */
std::string resolve(const std::string& relativePath)
{
    const char* root = ShaderLibrary::rootContaining(relativePath.c_str());
    if (root == nullptr) return {};
    return std::string{ root } + "/" + relativePath;
}

/* The directory part of a path, so the atlas named inside a .cwfont resolves
 * beside the .cwfont rather than against the working directory. */
std::string directoryOf(const std::string& path)
{
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string{} : path.substr(0, cut + 1);
}

}  // namespace

MsdfFont::~MsdfFont()
{
    unload();
}

void MsdfFont::unload()
{
    if (atlas_.id != 0) {
        UnloadTexture(atlas_);
        atlas_ = Texture2D{};
    }
    for (MsdfGlyph& glyph : glyphs_) glyph = MsdfGlyph{};
    pxRange_ = 0.0f;
}

bool MsdfFont::load(const std::string& cwfontPath)
{
    unload();

    const std::string resolved = resolve(cwfontPath);
    if (resolved.empty()) {
        LOGGER->warn("msdf: no such font table: {}", cwfontPath);
        return false;
    }

    std::ifstream file(resolved);
    if (!file) {
        LOGGER->warn("msdf: could not open {}", resolved);
        return false;
    }

    /* Line-at-a-time with a leading keyword, so an unknown record is skipped
     * rather than desynchronising the parse. That matters because this format
     * is generated: a newer baker adding a section should not stop an older
     * engine from reading the glyphs it does understand. */
    std::string imageName;
    int declaredGlyphs = 0;
    int loadedGlyphs = 0;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string keyword;
        if (!(fields >> keyword)) continue;

        if (keyword == "cwfont") {
            int version = 0;
            fields >> version;
            if (version != 1) {
                LOGGER->warn("msdf: {} is version {}, expected 1", resolved, version);
                return false;
            }
        } else if (keyword == "image") {
            fields >> imageName;
        } else if (keyword == "atlas") {
            fields >> atlasWidth_ >> atlasHeight_ >> pxRange_;
        } else if (keyword == "metrics") {
            fields >> lineHeight_ >> ascender_ >> descender_;
        } else if (keyword == "glyphs") {
            fields >> declaredGlyphs;
        } else if (keyword == "g") {
            int codepoint = 0;
            MsdfGlyph glyph;
            fields >> codepoint >> glyph.advance
                   >> glyph.planeLeft >> glyph.planeTop
                   >> glyph.planeRight >> glyph.planeBottom
                   >> glyph.atlasLeft >> glyph.atlasTop
                   >> glyph.atlasRight >> glyph.atlasBottom;

            const int index = codepoint - kFirstCodepoint;
            if (index >= 0 && index < kCodepointCount) {
                glyphs_[index] = glyph;
                ++loadedGlyphs;
            }
        }
        /* "kerning" and its "k" records are parsed by nothing yet — the bake
         * emits none for Inter, whose pairs live in GPOS where FreeType cannot
         * see them (see tools/bake_msdf.py). Falling through here rather than
         * erroring is what lets that change without touching this file. */
    }

    if (loadedGlyphs == 0 || atlasWidth_ <= 0.0f || pxRange_ <= 0.0f) {
        LOGGER->warn("msdf: {} carried no usable glyphs", resolved);
        return false;
    }
    if (declaredGlyphs != loadedGlyphs) {
        /* Not fatal — the missing ones simply will not draw — but it means the
         * file was truncated or the codepoint range moved, and both are worth
         * seeing before somebody reports a font with holes in it. */
        LOGGER->warn("msdf: {} declared {} glyphs, read {}", resolved,
                     declaredGlyphs, loadedGlyphs);
    }

    const std::string imagePath = directoryOf(resolved) + imageName;
    atlas_ = LoadTexture(imagePath.c_str());
    if (atlas_.id == 0) {
        LOGGER->warn("msdf: atlas image missing: {}", imagePath);
        unload();
        return false;
    }

    /* BILINEAR AND CLAMPED, AND BOTH MATTER.
     *
     * Bilinear is not a quality preference here the way it is for a glyph
     * bitmap — it is what makes the technique work at all. The shader
     * reconstructs a sharp edge by interpolating DISTANCE, so switching to
     * point sampling would quantise the field to texel centres and give back
     * exactly the blocky edge distance fields exist to avoid.
     *
     * No mipmaps, deliberately. A mip level is an average of distances, which
     * is not the distance to the averaged shape, so minified text goes muddy
     * and thin stems wander. Text small enough to want a mip should be drawn
     * by the UI path instead.
     *
     * Clamped because glyph cells sit right against each other: a wrapped
     * sample at a cell edge pulls in the neighbouring letter's field and puts
     * a sliver of the wrong glyph on the boundary. */
    SetTextureFilter(atlas_, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(atlas_, TEXTURE_WRAP_CLAMP);

    LOGGER->info("msdf: {} - {} glyphs, {}x{}, range {}", imageName, loadedGlyphs,
                 static_cast<int>(atlasWidth_), static_cast<int>(atlasHeight_),
                 pxRange_);
    return true;
}

const MsdfGlyph* MsdfFont::glyphFor(char character) const
{
    const int index = static_cast<unsigned char>(character) - kFirstCodepoint;
    if (index < 0 || index >= kCodepointCount) return nullptr;

    /* A codepoint inside the range that never loaded has a zero advance, which
     * would silently stack every following glyph on top of it. Treated as
     * absent instead. */
    const MsdfGlyph& glyph = glyphs_[index];
    return glyph.advance > 0.0f ? &glyph : nullptr;
}

float MsdfFont::measure(std::string_view text) const
{
    float width = 0.0f;
    for (const char character : text) {
        const MsdfGlyph* glyph = glyphFor(character);
        if (glyph != nullptr) width += glyph->advance;
    }
    return width;
}

void MsdfFont::layout(std::string_view text, std::vector<MsdfQuad>& outQuads) const
{
    if (!loaded()) return;

    /* Reserved once for the whole run rather than grown per glyph. Appending,
     * so the existing contents of a shared batch vector survive. */
    outQuads.reserve(outQuads.size() + text.size());

    const float uScale = 1.0f / atlasWidth_;
    const float vScale = 1.0f / atlasHeight_;

    float pen = 0.0f;
    for (const char character : text) {
        const MsdfGlyph* glyph = glyphFor(character);
        if (glyph == nullptr) continue;

        if (glyph->hasGeometry()) {
            MsdfQuad quad;
            quad.left   = pen + glyph->planeLeft;
            quad.top    = glyph->planeTop;
            quad.right  = pen + glyph->planeRight;
            quad.bottom = glyph->planeBottom;
            quad.u0 = glyph->atlasLeft   * uScale;
            quad.v0 = glyph->atlasTop    * vScale;
            quad.u1 = glyph->atlasRight  * uScale;
            quad.v1 = glyph->atlasBottom * vScale;
            outQuads.push_back(quad);
        }
        pen += glyph->advance;
    }
}

}  // namespace cromwell::sdf
