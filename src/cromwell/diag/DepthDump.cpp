#include "cromwell/diag/DepthDump.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/GL.hpp"

#include <cstdio>
#include <vector>

namespace cromwell::diag {
namespace {

void writeLittleEndian32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void writeLittleEndian16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

}  // namespace

bool dumpDepthTexture(uint32_t glTexture, int size, const char* path,
                      float& minimum, float& maximum)
{
    if (glTexture == 0 || size <= 0 || path == nullptr) return false;

    std::vector<float> depth(static_cast<size_t>(size) * static_cast<size_t>(size), 0.0f);

    glBindTexture(GL_TEXTURE_2D, glTexture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    /* THE RANGE THAT IS ACTUALLY THERE. A cleared-but-never-drawn map is all
     * 1.0 and reports a zero range, which says "nothing rendered into this"
     * far more clearly than a white picture does. */
    minimum = 1.0f;
    maximum = 0.0f;
    for (float value : depth) {
        if (value < minimum) minimum = value;
        if (value > maximum) maximum = value;
    }

    const float span = (maximum - minimum) > 1.0e-6f ? (maximum - minimum) : 1.0f;

    /* BMP rows are padded to four bytes and stored BOTTOM-UP, which suits a GL
     * texture: glGetTexImage hands back row 0 at the bottom too, so the two
     * conventions cancel and the image comes out the right way up. */
    const int rowBytes = size * 3;
    const int padding  = (4 - (rowBytes % 4)) % 4;
    const uint32_t pixelBytes = static_cast<uint32_t>((rowBytes + padding) * size);

    std::vector<uint8_t> file;
    file.reserve(54 + pixelBytes);

    file.push_back('B');
    file.push_back('M');
    writeLittleEndian32(file, 54 + pixelBytes);
    writeLittleEndian32(file, 0);
    writeLittleEndian32(file, 54);

    writeLittleEndian32(file, 40);                              /* header size  */
    writeLittleEndian32(file, static_cast<uint32_t>(size));
    writeLittleEndian32(file, static_cast<uint32_t>(size));
    writeLittleEndian16(file, 1);                               /* planes       */
    writeLittleEndian16(file, 24);                              /* bits         */
    writeLittleEndian32(file, 0);                               /* uncompressed */
    writeLittleEndian32(file, pixelBytes);
    writeLittleEndian32(file, 2835);
    writeLittleEndian32(file, 2835);
    writeLittleEndian32(file, 0);
    writeLittleEndian32(file, 0);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float normalised = (depth[static_cast<size_t>(y) * size + x] - minimum) / span;
            const auto grey = static_cast<uint8_t>(normalised * 255.0f + 0.5f);
            file.push_back(grey);
            file.push_back(grey);
            file.push_back(grey);
        }
        for (int p = 0; p < padding; p++) file.push_back(0);
    }

    std::FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        LOGGER.error("dumpDepthTexture: could not open {}", path);
        return false;
    }

    const size_t written = std::fwrite(file.data(), 1, file.size(), out);
    std::fclose(out);

    if (written != file.size()) {
        LOGGER.error("dumpDepthTexture: short write to {}", path);
        return false;
    }

    LOGGER.info("shadow map dumped to {} - {}x{}, depth range {:.6f}..{:.6f}",
                path, size, size, minimum, maximum);
    return true;
}

}  // namespace cromwell::diag
