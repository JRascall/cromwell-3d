/* CustomDepthStencil.hpp — an off-screen pass that tags objects for later effects.
 *
 * SINGLE RESPONSIBILITY: own one target that a chosen subset of the world is
 * rendered into, recording each object's STENCIL VALUE and its DEPTH.
 *
 * A PORT OF UNREAL'S CUSTOM DEPTH / CUSTOM STENCIL. There, any primitive can
 * set bRenderCustomDepth and a CustomDepthStencilValue — an integer 0-255 —
 * and the pass writes both into a buffer that materials and post-process read
 * back. It is deliberately general: x-ray silhouettes, selection outlines,
 * masking a post effect to particular actors, depth-of-field and blur masks
 * are all the same buffer with different consumers. That generality is the
 * whole point of copying it rather than building a silhouette feature.
 *
 * AN INTEGER PER OBJECT, NOT A CATEGORY PER CHANNEL. Four colour channels
 * holding four fixed categories is the obvious design and it is the rigid one:
 * two soldiers cannot be told apart, and every new kind of view wants a new
 * channel. An arbitrary value per object subsumes it — a consumer asks for the
 * value it cares about, "all units" becomes a range test, and nothing in the
 * renderer has to agree in advance about what the categories are.
 *
 * NOT A REAL STENCIL, and that is forced. rlgl attaches depth as
 * GL_DEPTH_COMPONENT24 with no stencil bits and wraps no stencil functions, so
 * a genuine stencil would mean raw GL from outside raylib. The value lives in a
 * colour channel instead: same information, same uses, one small buffer.
 *
 * WHAT IT IS NOT: a renderer of anything. Nothing here draws an outline,
 * detects an edge or composites. This is the half that is awkward to retrofit
 * — a place for selected geometry to rasterise apart from the scene, with its
 * own depth. A silhouette is then one full-screen shader: read the value, ask
 * whether this depth is behind the G-buffer's, draw accordingly.
 *
 * CHANNELS
 *   stencil().r   the object's value, 0-255 encoded to 0-1
 *   stencil().a   coverage — 1 where something was drawn, so that a value of
 *                 zero is a real id rather than "nothing here"
 *   depth()       that object's depth, for comparing against the G-buffer's
 */
#pragma once

#include "raylib.h"

#include "render/gpu/DepthTarget.hpp"

namespace xcom {

class CustomDepthStencil {
public:
    /* Unreal's range, and for the same reason: it is a byte. */
    static constexpr int kMaxValue = 255;

    CustomDepthStencil() = default;
    ~CustomDepthStencil();

    CustomDepthStencil(const CustomDepthStencil&) = delete;
    CustomDepthStencil& operator=(const CustomDepthStencil&) = delete;

    /* False means the shader is missing; the caller should skip the pass
     * rather than draw the world into a target nothing can read. */
    bool load();
    void resize(int width, int height);

    bool valid() const { return shader_.id != 0 && target_.valid(); }

    /* Draw tagged geometry with this, inside a Scope. */
    const Material& material() const { return material_; }

    /* The value the next draws are tagged with. Push it before each object or
     * group, exactly as the lit pass pushes its material factors — an unset
     * value is the previous one's. Clamped to 0-255. */
    void setStencil(int value) const;

    Texture2D stencil() const { return target_.colourTexture(); }
    Texture2D depth() const { return target_.depthTexture(); }

    /* Renders into the buffer for the block's lifetime, with colour blending
     * OFF — see the constructor. */
    class Scope {
    public:
        explicit Scope(const CustomDepthStencil& buffer);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        DepthTarget::Scope inner_;
    };

private:
    DepthTarget target_;
    Shader      shader_ = { 0 };
    Material    material_ = { 0 };
    int         locStencil_ = -1;
};

}  // namespace xcom
