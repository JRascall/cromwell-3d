/* GBuffer.hpp — what the frame knows about its own surfaces.
 *
 * SINGLE RESPONSIBILITY: own the screen-space description of the visible
 * world — depth, world normal, roughness — and hand the channels out by name.
 *
 * THIS IS NOT DEFERRED SHADING, and the distinction matters. Lighting still
 * happens in the forward pass, where a material's real f0, its textures and
 * its blend mode are all in hand. What this buffer exists for is the passes
 * that CANNOT be forward, because they need to know about pixels other than
 * their own: ambient occlusion, screen-space reflections, contact shadows,
 * edge detection, fog. Source 2 and Unreal both run a depth/normal prepass for
 * exactly this reason; going fully deferred would additionally cost us cheap
 * transparency, which the glass pass depends on.
 *
 * IT ALREADY EXISTED, UNNAMED. This began as "the ribbon's depth prepass",
 * then quietly grew world normals for SSAO, and was still being described as a
 * depth target while two passes read it as a G-buffer. Naming it is most of
 * the change; the rest is spending the alpha channel that the normal write was
 * filling with a constant 1.0 and discarding.
 *
 * CHANNELS
 *   depth()            the depth TEXTURE, samplable — raylib's own render
 *                      textures attach depth as a renderbuffer, which cannot
 *                      be read, hence DepthTarget underneath.
 *   surface().rgb      world normal, encoded n * 0.5 + 0.5
 *   surface().a        linear roughness, 0 mirror to 1 fully diffuse
 *
 * WHAT IS DELIBERATELY ABSENT: albedo and metalness. Neither fits in the one
 * channel left, and adding a second colour attachment costs bandwidth for
 * every pixel every frame to serve passes that do not exist yet. The one
 * consequence today is that screen-space reflections treat every surface as a
 * dielectric, so metal under-reflects; there is one metal in the whole set.
 * When a decal or a deferred light needs albedo, this is the single place that
 * grows an attachment.
 *
 * A THROUGH-WALL SILHOUETTE DOES NOT BELONG HERE. Drawing a unit through
 * geometry is a depth-STATE decision — the same mesh, submitted again with the
 * test disabled — and what it needs is its own small mask target, not a
 * material channel. Keeping that separate is why this stays three channels
 * wide instead of becoming a dumping ground.
 */
#pragma once

#include "raylib.h"

#include "cromwell/gpu/target/DepthTarget.hpp"

namespace cromwell {

class GBuffer {
public:
    GBuffer() = default;

    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    void resize(int width, int height) { target_.resize(width, height); }

    bool valid() const { return target_.valid(); }

    Texture2D depth() const { return target_.depthTexture(); }
    Texture2D surface() const { return target_.colourTexture(); }

    int width()  const { return target_.width(); }
    int height() const { return target_.height(); }

    /* Renders into the buffer for the block's lifetime. */
    class Scope {
    public:
        explicit Scope(const GBuffer& buffer) : inner_(buffer.target_) {}

    private:
        DepthTarget::Scope inner_;
    };

private:
    DepthTarget target_;
};

}  // namespace cromwell
