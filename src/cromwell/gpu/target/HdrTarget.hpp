/* HdrTarget.hpp — an RGBA16F render target, optionally with depth.
 *
 * SINGLE RESPONSIBILITY: own one float framebuffer.
 *
 * raylib's LoadRenderTexture is fixed at RGBA8, and 8 bits per channel is
 * exactly what this renderer must not have. Two separate reasons, one format:
 *
 *   - the glow chain, which is what this class was written for: an overbright
 *     emissive clamps to 1 on the way in and there is nothing left to bloom;
 *   - the scene itself, which is now shaded in unbounded linear radiance where
 *     the sun is tens of times brighter than a lit wall. Eight bits cannot
 *     hold that, and tonemapping an already-clipped buffer only spreads the
 *     clipping out.
 *
 * DEPTH IS OPTIONAL because the two uses genuinely differ: the scene pass
 * needs a depth buffer to sort geometry, and the blur targets never depth-test
 * at all. A renderbuffer rather than a texture — nothing samples it, and the
 * ribbon reads its own prepass (see DepthTarget).
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class HdrTarget {
public:
    HdrTarget() = default;
    ~HdrTarget() { destroy(); }

    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    /* Returns false when the format is unsupported, in which case the caller
     * should disable whatever needed it. */
    bool create(int width, int height, bool withDepth = false);
    void destroy();

    bool valid() const { return target_.id != 0; }

    Texture2D texture() const { return target_.texture; }
    float width()  const { return static_cast<float>(target_.texture.width); }
    float height() const { return static_cast<float>(target_.texture.height); }

    class Scope {
    public:
        explicit Scope(const HdrTarget& target) { BeginTextureMode(target.target_); }
        ~Scope() { EndTextureMode(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

private:
    RenderTexture2D target_ = { 0 };
};

}  // namespace cromwell
