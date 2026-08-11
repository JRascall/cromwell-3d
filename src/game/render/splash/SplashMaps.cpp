#include "game/render/splash/SplashMaps.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"

#include <algorithm>
#include <vector>

namespace game {

using namespace cromwell;

namespace {

constexpr const char* kWaterPath      = "textures/cromwell_water_mask.png";
constexpr const char* kWaterDepthPath = "textures/cromwell_water_depth_mask.png";
constexpr const char* kChopPath       = "textures/cromwell_water_chop_mask.png";
constexpr const char* kSkyPath        = "textures/cromwell_sky_mask.png";
constexpr const char* kSkyDepthPath   = "textures/cromwell_sky_depth_mask.png";

/* How far the coverage edge is softened, as a fraction of the image width.
 * Around four pixels at 1920, which is enough to stop the ripple starting at a
 * line without letting it creep visibly onto a hull. */
constexpr float kFeatherFraction = 0.002f;

/* Reads a greyscale-ish mask into 0..1 coverage at the given size, or returns
 * an empty vector if the file is not there.
 *
 * COVERAGE IS THE MINIMUM OF LUMINANCE AND ALPHA, which covers both ways a
 * mask gets painted without asking which was used. White on black gives
 * alpha 1 everywhere, so the minimum is the luminance; white on a transparent
 * background gives luminance and alpha that agree inside the shape and an
 * alpha of zero outside it, where the colour channels hold whatever the editor
 * left behind. Taking the minimum reads both correctly.
 *
 * IT ALSO NORMALISES. A mask painted with a "white" that is actually 235, or
 * flattened through a layer at 92% opacity, would otherwise scale every effect
 * it gates by 0.92 for no reason anyone would ever find. The brightest pixel
 * present is taken as full coverage. */
std::vector<float> readMask(const char* relativePath, int width, int height, bool& found)
{
    found = false;

    const char* root = ShaderLibrary::rootContaining(relativePath);
    if (root == nullptr) return {};

    Image image = LoadImage(TextFormat("%s/%s", root, relativePath));
    if (image.data == nullptr) {
        LOGGER->warn("SPLASH: {}/{} could not be decoded", root, relativePath);
        return {};
    }

    /* Every mask is resampled to the first one's size, so a flow ramp painted
     * at quarter resolution — which is plenty for something this smooth — still
     * lines up with a full-resolution coverage mask. */
    if (image.width != width || image.height != height)
        ImageResize(&image, width, height);

    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const unsigned char* bytes = static_cast<const unsigned char*>(image.data);

    std::vector<float> coverage(static_cast<std::size_t>(width) * height, 0.0f);
    float brightest = 0.0f;

    for (std::size_t i = 0; i < coverage.size(); i++) {
        const unsigned char* pixel = bytes + i * 4;
        const float luminance = (0.2126f * pixel[0] + 0.7152f * pixel[1] +
                                 0.0722f * pixel[2]) / 255.0f;
        const float alpha = pixel[3] / 255.0f;

        coverage[i] = std::min(luminance, alpha);
        brightest = std::max(brightest, coverage[i]);
    }

    UnloadImage(image);

    if (brightest > 0.0f && brightest < 1.0f)
        for (float& value : coverage) value = std::min(value / brightest, 1.0f);

    found = true;
    return coverage;
}

/* Separable box blur, run twice. Two box passes approximate a Gaussian closely
 * enough for a feather, and a box is O(1) per pixel with a running sum — which
 * matters not at all here, but writing the naive one and then explaining why it
 * is slow is worse than just writing this. */
void feather(std::vector<float>& field, int width, int height, int radius)
{
    if (radius < 1) return;

    std::vector<float> scratch(field.size(), 0.0f);
    const float inverse = 1.0f / static_cast<float>(2 * radius + 1);

    for (int pass = 0; pass < 2; pass++) {
        /* horizontal */
        for (int y = 0; y < height; y++) {
            const std::size_t row = static_cast<std::size_t>(y) * width;
            float sum = 0.0f;
            for (int x = -radius; x <= radius; x++)
                sum += field[row + static_cast<std::size_t>(std::clamp(x, 0, width - 1))];

            for (int x = 0; x < width; x++) {
                scratch[row + static_cast<std::size_t>(x)] = sum * inverse;
                const int leaving  = std::clamp(x - radius, 0, width - 1);
                const int entering = std::clamp(x + radius + 1, 0, width - 1);
                sum += field[row + static_cast<std::size_t>(entering)] -
                       field[row + static_cast<std::size_t>(leaving)];
            }
        }

        /* vertical */
        for (int x = 0; x < width; x++) {
            float sum = 0.0f;
            for (int y = -radius; y <= radius; y++)
                sum += scratch[static_cast<std::size_t>(std::clamp(y, 0, height - 1)) * width +
                               static_cast<std::size_t>(x)];

            for (int y = 0; y < height; y++) {
                field[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] =
                    sum * inverse;
                const int leaving  = std::clamp(y - radius, 0, height - 1);
                const int entering = std::clamp(y + radius + 1, 0, height - 1);
                sum += scratch[static_cast<std::size_t>(entering) * width +
                               static_cast<std::size_t>(x)] -
                       scratch[static_cast<std::size_t>(leaving) * width +
                               static_cast<std::size_t>(x)];
            }
        }
    }
}

/* Derives a depth ramp from a coverage mask: 0 at the far edge, 1 at the near
 * edge, per column. `nearAtTop` flips which end is which — a river is nearest
 * at the BOTTOM of frame and a sky is nearest at the TOP, and they are
 * otherwise the same problem.
 *
 * THE ASSUMPTION IS THAT DEPTH RUNS VERTICALLY, which is true of any river or
 * sky seen from the ground and false the moment a river bends away or a cloud
 * bank runs across. That is the whole limitation of deriving this, and it is
 * why a painted depth mask overrides it.
 *
 * PER COLUMN, AND SPANNING THE HOLES. The extent is the first and last water
 * row in a column, so the barges and boats punched out of the mask sit inside
 * the span rather than cutting it into pieces — the water on both sides of a
 * hull gets the ramp it would have had if the hull were not there, which is
 * what makes the wave crests run straight past a boat instead of stepping
 * around it.
 *
 * The extents are then smoothed across columns. Without that, one mast or one
 * mooring post touching the top of the water region drags the far edge down for
 * its own column only, and the ramp — and so every wave crest crossing it —
 * kinks around it. */
std::vector<float> deriveDepth(const std::vector<float>& coverage, int width, int height,
                               bool nearAtTop = false)
{
    std::vector<float> topRow(static_cast<std::size_t>(width), -1.0f);
    std::vector<float> bottomRow(static_cast<std::size_t>(width), -1.0f);

    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            if (coverage[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] > 0.5f) {
                if (topRow[static_cast<std::size_t>(x)] < 0.0f)
                    topRow[static_cast<std::size_t>(x)] = static_cast<float>(y);
                bottomRow[static_cast<std::size_t>(x)] = static_cast<float>(y);
            }
        }
    }

    /* Carry the extents sideways across columns that have no water at all, so
     * the smoothing below has something continuous to work with. */
    for (int x = 1; x < width; x++)
        if (topRow[static_cast<std::size_t>(x)] < 0.0f) {
            topRow[static_cast<std::size_t>(x)]    = topRow[static_cast<std::size_t>(x - 1)];
            bottomRow[static_cast<std::size_t>(x)] = bottomRow[static_cast<std::size_t>(x - 1)];
        }
    for (int x = width - 2; x >= 0; x--)
        if (topRow[static_cast<std::size_t>(x)] < 0.0f) {
            topRow[static_cast<std::size_t>(x)]    = topRow[static_cast<std::size_t>(x + 1)];
            bottomRow[static_cast<std::size_t>(x)] = bottomRow[static_cast<std::size_t>(x + 1)];
        }

    const int smoothing = std::max(width / 24, 1);
    auto smooth = [&](std::vector<float>& values) {
        std::vector<float> source = values;
        for (int x = 0; x < width; x++) {
            float sum = 0.0f;
            for (int offset = -smoothing; offset <= smoothing; offset++)
                sum += source[static_cast<std::size_t>(std::clamp(x + offset, 0, width - 1))];
            values[static_cast<std::size_t>(x)] = sum / static_cast<float>(2 * smoothing + 1);
        }
    };
    smooth(topRow);
    smooth(bottomRow);

    std::vector<float> depth(coverage.size(), 0.0f);
    for (int x = 0; x < width; x++) {
        const float top = topRow[static_cast<std::size_t>(x)];
        const float span = std::max(bottomRow[static_cast<std::size_t>(x)] - top, 1.0f);

        for (int y = 0; y < height; y++) {
            const float down = std::clamp((static_cast<float>(y) - top) / span, 0.0f, 1.0f);
            depth[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] =
                nearAtTop ? 1.0f - down : down;
        }
    }

    return depth;
}

}  // namespace


namespace {

/* Packs up to three fields into one RGB image. Any field may be empty, in which
 * case that channel is left at zero. */
Image pack(const std::vector<float>& red, const std::vector<float>& green,
           const std::vector<float>& blue, int width, int height)
{
    Image packed = GenImageColor(width, height, BLACK);
    ImageFormat(&packed, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    unsigned char* bytes = static_cast<unsigned char*>(packed.data);

    const std::size_t count = static_cast<std::size_t>(width) * height;
    auto at = [count](const std::vector<float>& field, std::size_t i) {
        return field.size() == count ? std::clamp(field[i], 0.0f, 1.0f) : 0.0f;
    };

    for (std::size_t i = 0; i < count; i++) {
        unsigned char* pixel = bytes + i * 4;
        pixel[0] = static_cast<unsigned char>(at(red, i)   * 255.0f);
        pixel[1] = static_cast<unsigned char>(at(green, i) * 255.0f);
        pixel[2] = static_cast<unsigned char>(at(blue, i)  * 255.0f);
        pixel[3] = 255;
    }
    return packed;
}

/* The size a set of masks is built at, taken from the coverage mask because it
 * is the one that needs resolution — it has to follow the edge of a hull or a
 * spire. Depth and chop are smooth fields and are resampled to match, so they
 * can be painted at any size. Returns false if there is no coverage mask. */
bool sizeOf(const char* path, int& width, int& height)
{
    const char* root = ShaderLibrary::rootContaining(path);
    if (root == nullptr) return false;

    Image probe = LoadImage(TextFormat("%s/%s", root, path));
    if (probe.data == nullptr) {
        LOGGER->warn("SPLASH: {}/{} could not be decoded", root, path);
        return false;
    }

    width = probe.width;
    height = probe.height;
    UnloadImage(probe);
    return true;
}

}  // namespace

SplashMaps::Result SplashMaps::build()
{
    Result result;

    /* ---- water ---------------------------------------------------------- */
    int width = 0, height = 0;
    if (sizeOf(kWaterPath, width, height)) {
        bool found = false;
        std::vector<float> coverage = readMask(kWaterPath, width, height, found);
        if (found) {
            feather(coverage, width, height,
                    static_cast<int>(static_cast<float>(width) * kFeatherFraction));

            std::vector<float> depth = readMask(kWaterDepthPath, width, height, found);
            const bool depthPainted = found;
            if (!depthPainted) depth = deriveDepth(coverage, width, height);

            std::vector<float> chop = readMask(kChopPath, width, height, found);
            const bool chopPainted = found;

            LOGGER->info("SPLASH: water map {}x{} - depth {}, chop {}", width, height,
                         depthPainted ? "painted" : "derived from the mask",
                         chopPainted ? "painted" : "none");

            result.water = pack(coverage, depth, chop, width, height);
        }
    }

    /* ---- sky ------------------------------------------------------------ */
    if (sizeOf(kSkyPath, width, height)) {
        bool found = false;
        std::vector<float> coverage = readMask(kSkyPath, width, height, found);
        if (found) {
            /* Feathered like the water's, and for a sharper reason: the sky's
             * edge IS the silhouette, so a hard one puts a crawling rim on the
             * most recognisable shape in the picture. */
            feather(coverage, width, height,
                    static_cast<int>(static_cast<float>(width) * kFeatherFraction));

            std::vector<float> depth = readMask(kSkyDepthPath, width, height, found);
            const bool depthPainted = found;

            /* Derived the other way up from the water's. Sky is nearest
             * OVERHEAD and recedes DOWNWARDS to the horizon, so the ramp runs
             * from white at the top of the painted sky to black at its lowest
             * point — the exact mirror of a river, which is nearest at the
             * bottom of frame. */
            if (!depthPainted) depth = deriveDepth(coverage, width, height, true);

            LOGGER->info("SPLASH: sky map {}x{} - depth {}", width, height,
                         depthPainted ? "painted" : "derived from the mask");

            result.sky = pack(coverage, depth, std::vector<float>{}, width, height);
        }
    }

    return result;
}

}  // namespace game
