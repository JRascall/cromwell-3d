/* DepthDump.hpp — write a depth texture to a viewable image.
 *
 * SINGLE RESPONSIBILITY: read a GL depth texture back and write it out as a
 * greyscale BMP a human can open.
 *
 * ======================== WHY THIS EXISTS ================================
 *
 * TEMPORARY DIAGNOSTIC, for comparing the two renderers' shadow maps directly
 * rather than arguing about their outputs. Delete it once the device path
 * reaches parity — or keep it, if it earns its place the next time a shadow
 * looks wrong, which is the usual fate of tools like this.
 *
 * A DEPTH TEXTURE CANNOT GO THROUGH IRenderDevice::readTexture, which attaches
 * its argument as colour attachment zero and reads that. Depth is not a colour
 * attachment, so the framebuffer would be incomplete. glGetTexImage needs no
 * framebuffer at all and works on any texture, which is exactly what a
 * diagnostic wants.
 *
 * BMP RATHER THAN PNG because it needs no encoder and no library: a 24-bit BMP
 * is a fixed header and a pixel array, and every image viewer on every platform
 * opens one. This has to stay out of raylib's way — it sits under cromwell and
 * is called from the device backend, which may not name raylib.
 *
 * NORMALISED, NOT RAW. Depth in an orthographic shadow map occupies a narrow
 * slice of 0..1 and printed raw it is a uniform grey rectangle. The written
 * image stretches whatever range is actually present to full black-to-white,
 * and the caller is told what that range was — so two dumps are comparable only
 * once their reported ranges are, which is the first thing worth checking.
 */
#pragma once

#include <cstdint>

namespace cromwell::diag {

/* Reads `glTexture` (a GL_TEXTURE_2D depth texture, `size` square) and writes a
 * normalised greyscale BMP to `path`. Returns false and logs if the read or the
 * write failed. `minimum` and `maximum` report the depth range found, which is
 * the number worth comparing between two dumps before looking at the pictures. */
bool dumpDepthTexture(uint32_t glTexture, int size, const char* path,
                      float& minimum, float& maximum);

}  // namespace cromwell::diag
