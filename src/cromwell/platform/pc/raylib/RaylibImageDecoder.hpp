/* RaylibImageDecoder.hpp — IImageDecoder, through raylib's bundled stb_image.
 *
 * SINGLE RESPONSIBILITY: decode a PNG or JPEG blob to RGBA8 using raylib's
 * image loader, and normalise everything it returns.
 *
 * ==================== WHAT NORMALISING ACTUALLY MEANS HERE =================
 *
 * raylib will happily hand back an image in any of a dozen pixel formats
 * depending on what the file contained — 8-bit grayscale, RGB without alpha,
 * 16-bit float, a compressed block format. IImageDecoder promises RGBA8,
 * tightly packed, and that promise is the entire value of the interface: every
 * consumer uploads the result to a texture, and a decoder that returned five
 * possible layouts would push the conversion into all of them.
 *
 * So this converts, and it converts even when it looks unnecessary, because
 * "this PNG happened to be RGBA already" is a property of one file rather than
 * of the format.
 *
 * ===================== THE SIZE LIMIT IS NOT PARANOIA =====================
 *
 * A malformed or hostile header can claim enormous dimensions, and stb will try
 * to allocate for them before it discovers the data is short. The avatar path
 * is the live example: those bytes come off the network from a URL the process
 * did not choose. A limit checked against the HEADER, before the decode, turns
 * that from an out-of-memory abort into a TooLarge the caller can report.
 */
#pragma once

#include "cromwell/assets/IImageDecoder.hpp"

namespace cromwell {

class RaylibImageDecoder final : public IImageDecoder {
public:
    /* 16384 on a side is beyond any texture this engine uses and beyond what
     * most hardware will sample, so anything larger is a broken or hostile
     * header rather than a picture somebody meant to ship. */
    static constexpr int kMaxDimension = 16384;

    ImageDecodeResult decode(const void* bytes, size_t length, DecodedImage& out) override;
    ImageDecodeResult probe(const void* bytes, size_t length, int& width, int& height) override;
    bool              recognises(const void* bytes, size_t length) const override;
};

}  // namespace cromwell
