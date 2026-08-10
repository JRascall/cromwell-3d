/* DepthTarget.hpp — a render target whose depth can be SAMPLED.
 *
 * SINGLE RESPONSIBILITY: own one framebuffer with a colour texture and a
 * depth TEXTURE, and hand out both attachments.
 *
 * raylib's LoadRenderTexture attaches depth as a RENDERBUFFER, which cannot be
 * sampled. The ribbon shader compares PixelDepth against DestDepth, so it
 * needs the scene's depth as a texture — hence the hand-assembled FBO.
 *
 * THE COLOUR PLANE EXISTS EITHER WAY. OpenGL calls a framebuffer with no
 * colour attachment incomplete unless the draw buffer is set to NONE, which
 * rlgl cannot express — so every one of these carries a colour texture whether
 * it wants one or not. The scene prepass spends it on world normals, which is
 * what makes SSAO cost one extra fragment shader and no extra pass; the shadow
 * map genuinely wastes it.
 *
 * RAII: the destructor releases both textures and the FBO.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class DepthTarget {
public:
    DepthTarget() = default;
    DepthTarget(int width, int height) { create(width, height); }
    ~DepthTarget() { destroy(); }

    DepthTarget(const DepthTarget&) = delete;
    DepthTarget& operator=(const DepthTarget&) = delete;

    /* `colourFormat` exists because the colour plane is forced on us, not
     * wanted: the scene prepass spends it on world normals and needs all four
     * channels, while the shadow map never reads it at all. At 4096 square
     * that difference is 64 MB of RGBA against 16 MB of R8, which is the
     * whole reason a higher-resolution shadow map is affordable. */
    void resize(int width, int height,
                int colourFormat = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    {
        destroy();
        create(width, height, colourFormat);
    }

    bool valid() const { return target_.id != 0; }

    Texture2D depthTexture() const { return target_.depth; }

    /* Whatever the pass chose to write. For the scene prepass that is the
     * world normal, encoded n * 0.5 + 0.5 — see prepass.fs.glsl. */
    Texture2D colourTexture() const { return target_.texture; }

    int width()  const { return target_.texture.width; }
    int height() const { return target_.texture.height; }

    /* Scope guard: draws into this target for the block's lifetime. */
    class Scope {
    public:
        explicit Scope(const DepthTarget& target) { BeginTextureMode(target.target_); }
        ~Scope() { EndTextureMode(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

private:
    void create(int width, int height,
                int colourFormat = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    void destroy();

    RenderTexture2D target_ = { 0 };
};

}  // namespace cromwell
