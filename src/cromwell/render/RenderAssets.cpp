#include "cromwell/render/RenderAssets.hpp"

#include "cromwell/assets/IImageDecoder.hpp"
#include "cromwell/diag/Logger.hpp"
#include "cromwell/platform/IFileSystem.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

namespace cromwell {

RenderAssets::RenderAssets(rhi::IRenderDevice& device, IFileSystem& files,
                           IImageDecoder& images)
    : device_(device), files_(files), images_(images), materials_(device)
{
}

RenderAssets::~RenderAssets()
{
    for (auto& entry : textures_)
        if (entry.second.valid()) device_.destroy(entry.second);
    textures_.clear();

    if (flatNormal_.valid()) device_.destroy(flatNormal_);
    if (white_.valid())      device_.destroy(white_);
}

bool RenderAssets::initialise()
{
    /* THE TWO STAND-INS FIRST, because anything that fails to load a map falls
     * back to one and a fallback that does not exist yet is a black texture —
     * which for a normal map is a surface facing away from everything. */
    const std::uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    /* (0.5, 0.5, 1) IS "NO PERTURBATION" in a tangent-space normal map, and it
     * is not white. A white stand-in decodes to (1, 1, 1) normalised, which
     * tilts every fragment 45 degrees toward the tangent and reads as the
     * lighting being wrong everywhere rather than as a missing map. */
    const std::uint8_t flatPixel[4] = { 128, 128, 255, 255 };

    rhi::TextureDesc desc;
    desc.name   = "white";
    desc.width  = 1;
    desc.height = 1;
    desc.format = rhi::TextureFormat::RGBA8;
    desc.usage  = rhi::TextureUsageSampled;

    white_ = device_.createTexture(desc);
    if (white_.valid()) device_.updateTexture(white_, whitePixel);

    desc.name  = "flat normal";
    flatNormal_ = device_.createTexture(desc);
    if (flatNormal_.valid()) device_.updateTexture(flatNormal_, flatPixel);

    /* THE LOOP SHAPE IS DELIBERATE: every future member here — the mesh cache,
     * the shader library — needs the device and can fail, and a caller that has
     * to learn a new initialise call per addition is a caller that eventually
     * misses one. */
    return materials_.initialise();
}

rhi::TextureHandle RenderAssets::upload(const char* label, const DecodedImage& image)
{
    rhi::TextureDesc desc;
    desc.name   = label;
    desc.width  = static_cast<uint32_t>(image.width);
    desc.height = static_cast<uint32_t>(image.height);

    /* RGBA8Srgb, AND THE CHOICE IS PER TEXTURE RATHER THAN PER SAMPLE.
     *
     * IImageDecoder promises 8-bit RGBA and says nothing about colour space,
     * because a decoder cannot know: the same bytes are an albedo (encoded) or
     * a normal map (data) depending on what asked for them. Declaring the
     * format sRGB makes the hardware decode on every fetch, for free and with
     * correct filtering — which a pow() in the shader does NOT give you, since
     * that decodes after the bilinear blend rather than before it.
     *
     * SO WHY sRGB HERE, FOR EVERY TEXTURE THIS FUNCTION MAKES. Because its one
     * caller today is the decal albedo, which is colour. The moment a normal or
     * a packed map comes through, this needs to be told which it is — and the
     * right shape for that is an argument, not a guess. Getting it wrong on a
     * normal map is the classic "lighting is subtly wrong everywhere and very
     * hard to trace", which assets/materials/README.md already warns about. */
    desc.format = rhi::TextureFormat::RGBA8Srgb;
    desc.usage  = rhi::TextureUsageSampled;

    const rhi::TextureHandle handle = device_.createTexture(desc);
    if (!handle.valid()) {
        LOGGER.error("assets: could not create a {}x{} texture for '{}'",
                     image.width, image.height, label);
        return {};
    }

    /* LAYER AND MIP DEFAULT TO ZERO AND ARE NOT WIDTH AND HEIGHT — the trap at
     * the top of rhi/MIGRATION.md §5, which cost an hour on the ImGui backend.
     * The size comes from the texture; leave these alone. */
    device_.updateTexture(handle, image.pixels.data());
    return handle;
}

rhi::TextureHandle RenderAssets::texture(const char* name)
{
    if (name == nullptr || name[0] == '\0') return {};

    const std::string key(name);

    /* A HIT IS A HIT EVEN WHEN IT IS INVALID. See the header: caching the
     * failure is what stops a missing file costing a filesystem probe and a
     * decode attempt on every frame that asks for it. */
    const auto found = textures_.find(key);
    if (found != textures_.end()) return found->second;

    std::vector<std::uint8_t> bytes;
    const StorageResult read = files_.read(StorageKind::Asset, name, bytes);

    if (read != StorageResult::Ok || bytes.empty()) {
        /* AT DEBUG, NOT WARNING. An absent optional map is routine — every
         * material in this project is missing most of its maps — and a warning
         * per missing file at startup is a log nobody reads. A caller for which
         * the absence is NOT routine says so itself; the decal pass does. */
        LOGGER.debug("assets: no texture at '{}'", name);
        textures_.emplace(key, rhi::TextureHandle{});
        return {};
    }

    DecodedImage image;
    if (images_.decode(bytes.data(), bytes.size(), image) != ImageDecodeResult::Ok
        || !image.valid()) {
        LOGGER.warn("assets: '{}' is {} bytes and did not decode", name, bytes.size());
        textures_.emplace(key, rhi::TextureHandle{});
        return {};
    }

    const rhi::TextureHandle handle = upload(name, image);
    textures_.emplace(key, handle);

    if (handle.valid())
        LOGGER.info("assets: loaded {}x{} texture '{}'", image.width, image.height, name);

    return handle;
}

}  // namespace cromwell
