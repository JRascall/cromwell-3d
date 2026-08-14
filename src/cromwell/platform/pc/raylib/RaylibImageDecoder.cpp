#include "cromwell/platform/pc/raylib/RaylibImageDecoder.hpp"

#include "raylib.h"

#include <cstring>

namespace cromwell {
namespace {

/* MAGIC BYTES, NOT AN EXTENSION. The blob may have arrived over HTTP with no
 * name at all — the Steam avatar does — and a server's content type is not
 * something to trust either. */
const char* sniffExtension(const void* bytes, size_t length)
{
    if (bytes == nullptr || length < 12) return nullptr;
    const auto* b = static_cast<const unsigned char*>(bytes);

    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return ".png";
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)              return ".jpg";
    if (b[0] == 'B' && b[1] == 'M')                                return ".bmp";
    if (std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return ".webp";
    if (std::memcmp(b, "qoif", 4) == 0)                            return ".qoi";

    return nullptr;
}

}  // namespace

bool RaylibImageDecoder::recognises(const void* bytes, size_t length) const
{
    return sniffExtension(bytes, length) != nullptr;
}

ImageDecodeResult RaylibImageDecoder::probe(const void* bytes, size_t length,
                                            int& width, int& height)
{
    const char* extension = sniffExtension(bytes, length);
    if (extension == nullptr) return ImageDecodeResult::UnknownFormat;

    /* raylib offers no header-only read, so this decodes and throws the pixels
     * away. Honest rather than fast: probe() is specified as cheap enough for
     * the main thread and this is not, but the alternative is bundling a second
     * image parser to answer a question with one caller. A console backend
     * using a system decoder gets the cheap version for free.
     *
     * Marked here rather than left as a surprise, because if probe() ever
     * acquires a hot caller this is what needs fixing. */
    Image image = LoadImageFromMemory(extension, static_cast<const unsigned char*>(bytes),
                                      static_cast<int>(length));
    if (image.data == nullptr) return ImageDecodeResult::Corrupt;

    width  = image.width;
    height = image.height;
    UnloadImage(image);

    if (width <= 0 || height <= 0) return ImageDecodeResult::Corrupt;
    if (width > kMaxDimension || height > kMaxDimension) return ImageDecodeResult::TooLarge;

    return ImageDecodeResult::Ok;
}

ImageDecodeResult RaylibImageDecoder::decode(const void* bytes, size_t length, DecodedImage& out)
{
    const char* extension = sniffExtension(bytes, length);
    if (extension == nullptr) return ImageDecodeResult::UnknownFormat;

    Image image = LoadImageFromMemory(extension, static_cast<const unsigned char*>(bytes),
                                      static_cast<int>(length));
    if (image.data == nullptr) return ImageDecodeResult::Corrupt;

    if (image.width <= 0 || image.height <= 0) {
        UnloadImage(image);
        return ImageDecodeResult::Corrupt;
    }

    if (image.width > kMaxDimension || image.height > kMaxDimension) {
        UnloadImage(image);
        return ImageDecodeResult::TooLarge;
    }

    /* ALWAYS CONVERTED, even when the source was already RGBA8 — see the
     * header. ImageFormat is a no-op in that case, and paying for the check is
     * cheaper than every consumer having to ask what it got. */
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (image.data == nullptr) {
        UnloadImage(image);
        return ImageDecodeResult::OutOfMemory;
    }

    const size_t pixels = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    const size_t total  = pixels * 4;

    /* ASSIGNED ONLY ON SUCCESS. IImageDecoder promises `out` is untouched when
     * a decode fails, so a caller cannot end up holding half an image that
     * looks valid — which is why the copy happens here at the end rather than
     * being written into progressively. */
    out.pixels.assign(static_cast<const uint8_t*>(image.data),
                      static_cast<const uint8_t*>(image.data) + total);
    out.width  = image.width;
    out.height = image.height;

    UnloadImage(image);
    return ImageDecodeResult::Ok;
}

}  // namespace cromwell
