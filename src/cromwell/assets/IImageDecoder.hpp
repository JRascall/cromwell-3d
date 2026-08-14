/* IImageDecoder.hpp — compressed image bytes in, pixels out.
 *
 * SINGLE RESPONSIBILITY: turn a PNG or JPEG blob into an RGBA buffer a texture
 * can be uploaded from, and say honestly when it cannot.
 *
 * ================= WHY THIS IS SEPARATE FROM THE RENDER DEVICE =============
 *
 * Because decoding is not a GPU operation and does not want to be on the render
 * thread. The bytes usually arrive from somewhere slow — a file read, or an
 * HTTP fetch, which is exactly where the Steam avatar comes from — and the
 * decode is pure CPU work over a buffer. Putting it on IRenderDevice would tie
 * it to a graphics API it has nothing to do with, and would mean a headless
 * tool that wants to inspect a texture has to create a device to do it.
 *
 * SO THE SPLIT IS: this produces pixels, IRenderDevice::createTexture and
 * updateTexture consume them. Everything that merely GENERATES pixels — a flat
 * normal map, a 1x1 white, a blank surface for the web view, a font atlas — has
 * no business here at all and should go straight to the device. Most of the
 * image calls in this codebase are that case, which is why this interface is as
 * small as it is.
 *
 * ==================== WHY IT IS AN INTERFACE AND NOT A FUNCTION ============
 *
 * Console SDKs ship hardware or system decoders, and using them is not
 * optional at scale — a software JPEG decode of a large texture is measured in
 * tens of milliseconds and there may be hundreds. Desktop uses stb_image or
 * whatever the backend already links. Same call site, very different
 * implementation.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cromwell {

enum class ImageDecodeResult : uint8_t {
    Ok = 0,
    UnknownFormat,   /* not an image this decoder recognises */
    Corrupt,
    TooLarge,        /* beyond the platform's or the caller's limit */
    OutOfMemory,
};

/* WHAT CAME OUT. Always 8-bit RGBA, top-left origin, tightly packed — one
 * layout rather than a format enum, because every consumer here uploads it to
 * a texture and a decoder that returned five possible layouts would push the
 * conversion into every call site instead of doing it once.
 *
 * TOP-LEFT ORIGIN IS STATED because it is the half of this that silently
 * inverts: a render target is bottom-up on GL and an uploaded image is not,
 * which is exactly the distinction TexturePreviews already has to make. */
struct DecodedImage {
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    /* Decode a whole blob. `out` is replaced on success and left untouched
     * otherwise, so a failed decode cannot leave a caller holding half an
     * image that looks valid. */
    virtual ImageDecodeResult decode(const void* bytes, size_t length,
                                     DecodedImage& out) = 0;

    /* Dimensions without decoding the pixels — reads the header only. For
     * deciding whether something fits a budget, or sizing a target before the
     * decode is scheduled. Cheap enough to call on the main thread. */
    virtual ImageDecodeResult probe(const void* bytes, size_t length,
                                    int& width, int& height) = 0;

    /* Whether this decoder handles a blob at all, by sniffing its magic bytes
     * rather than trusting a file extension there may not be. */
    virtual bool recognises(const void* bytes, size_t length) const = 0;
};

}  // namespace cromwell
