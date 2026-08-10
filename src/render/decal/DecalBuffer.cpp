#include "render/decal/DecalBuffer.hpp"

#include "rlgl.h"

namespace xcom {
namespace {

/* Allocated once for the whole program and never unloaded — it outlives any
 * individual buffer and costs four bytes. */
Texture2D gEmpty{};

}  // namespace

DecalBuffer::~DecalBuffer()
{
    destroy();
}

void DecalBuffer::destroy()
{
    if (!framebuffer_) return;

    rlUnloadTexture(albedo_.id);
    rlUnloadTexture(normal_.id);
    rlUnloadTexture(surface_.id);
    rlUnloadFramebuffer(framebuffer_);

    framebuffer_ = 0;
    albedo_  = Texture2D{};
    normal_  = Texture2D{};
    surface_ = Texture2D{};
    width_ = height_ = 0;
}

void DecalBuffer::resize(int width, int height)
{
    destroy();
    if (width < 1 || height < 1) return;

    framebuffer_ = rlLoadFramebuffer();
    if (!framebuffer_) {
        TraceLog(LOG_WARNING, "DECAL: could not create the DBuffer FBO - decals are off");
        return;
    }

    rlEnableFramebuffer(framebuffer_);

    /* NO DEPTH ATTACHMENT, and it is not an omission. The pass draws the BACK
     * faces of each projector box with the depth test off and rejects fragments
     * in the shader against the box's own bounds, so the hardware depth test
     * has nothing left to decide. Attaching one would also mean sharing the
     * prepass's depth texture across two framebuffers, which is exactly the
     * kind of aliasing that turns into a driver-specific bug. */
    struct Plane {
        Texture2D* texture;
        int        attachment;
    };
    const Plane planes[] = {
        { &albedo_,  RL_ATTACHMENT_COLOR_CHANNEL0 },
        { &normal_,  RL_ATTACHMENT_COLOR_CHANNEL1 },
        { &surface_, RL_ATTACHMENT_COLOR_CHANNEL2 },
    };

    for (const Plane& plane : planes) {
        plane.texture->id = rlLoadTexture(nullptr, width, height,
                                          PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        plane.texture->width   = width;
        plane.texture->height  = height;
        plane.texture->format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        plane.texture->mipmaps = 1;

        rlFramebufferAttach(framebuffer_, plane.texture->id, plane.attachment,
                            RL_ATTACHMENT_TEXTURE2D, 0);

        /* CLAMP, not repeat. The lit pass looks this up by screen position, and
         * a fragment half a texel outside the buffer must read the edge rather
         * than wrap round and paint a decal from the far side of the screen. */
        SetTextureWrap(*plane.texture, TEXTURE_WRAP_CLAMP);
        SetTextureFilter(*plane.texture, TEXTURE_FILTER_BILINEAR);
    }

    /* Every attachment has to be a declared draw buffer before the completeness
     * check, or the incomplete-attachment case reports as complete on some
     * drivers and writes nowhere on others. */
    rlActiveDrawBuffers(3);

    if (!rlFramebufferComplete(framebuffer_)) {
        TraceLog(LOG_WARNING, "DECAL: DBuffer FBO incomplete - decals are off");
        rlActiveDrawBuffers(1);
        rlDisableFramebuffer();
        destroy();
        return;
    }

    rlActiveDrawBuffers(1);
    rlDisableFramebuffer();

    width_  = width;
    height_ = height;
}

Texture2D DecalBuffer::emptyTexture()
{
    if (gEmpty.id == 0) {
        /* (0, 0, 0, 255): no decal ink, base material fully intact. rlgl's
         * shared default is 1x1 WHITE, which decodes as a fully opaque white
         * decal over the entire screen — the single worst fallback available,
         * so this one is allocated rather than borrowed. */
        Image image = GenImageColor(1, 1, kEmpty);
        gEmpty = LoadTextureFromImage(image);
        UnloadImage(image);
    }
    return gEmpty;
}

Texture2D DecalBuffer::albedo()  const { return valid() ? albedo_  : emptyTexture(); }
Texture2D DecalBuffer::normal()  const { return valid() ? normal_  : emptyTexture(); }
Texture2D DecalBuffer::surface() const { return valid() ? surface_ : emptyTexture(); }

DecalBuffer::Scope::Scope(const DecalBuffer& buffer)
{
    if (!buffer.valid()) return;
    active_ = true;

    /* Through a RenderTexture2D shim so BeginTextureMode does the viewport, the
     * framebuffer-size bookkeeping and the projection reset — all of which
     * BeginMode3D then reads to build its aspect ratio. Only attachment 0 is
     * named here; rlActiveDrawBuffers below is what brings the other two in. */
    RenderTexture2D shim{};
    shim.id      = buffer.framebuffer_;
    shim.texture = buffer.albedo_;
    BeginTextureMode(shim);

    rlActiveDrawBuffers(3);
}

DecalBuffer::Scope::~Scope()
{
    if (!active_) return;

    /* BACK TO ONE BEFORE THE FRAMEBUFFER CHANGES. glDrawBuffers is framebuffer
     * state, but rlgl caches nothing about it, and leaving three enabled means
     * the next pass to bind a single-attachment target inherits a draw-buffer
     * list naming attachments it does not have. */
    rlActiveDrawBuffers(1);
    EndTextureMode();
}

}  // namespace xcom
