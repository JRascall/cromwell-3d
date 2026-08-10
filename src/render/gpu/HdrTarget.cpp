#include "render/gpu/HdrTarget.hpp"

#include "rlgl.h"

namespace xcom {

bool HdrTarget::create(int width, int height, bool withDepth)
{
    destroy();

    target_.id = rlLoadFramebuffer();
    if (!target_.id) return false;

    rlEnableFramebuffer(target_.id);
    target_.texture.id      = rlLoadTexture(nullptr, width, height,
                                            PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
    target_.texture.width   = width;
    target_.texture.height  = height;
    target_.texture.format  = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
    target_.texture.mipmaps = 1;

    rlFramebufferAttach(target_.id, target_.texture.id,
                        RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);

    if (withDepth) {
        /* true = RENDERBUFFER. Nothing samples this one, and rlUnloadFramebuffer
         * deletes whatever is attached as depth, so destroy() needs no branch. */
        target_.depth.id      = rlLoadTextureDepth(width, height, true);
        target_.depth.width   = width;
        target_.depth.height  = height;
        target_.depth.mipmaps = 1;
        rlFramebufferAttach(target_.id, target_.depth.id,
                            RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    }

    if (!rlFramebufferComplete(target_.id)) {
        rlDisableFramebuffer();
        rlUnloadTexture(target_.texture.id);
        rlUnloadFramebuffer(target_.id);
        target_ = RenderTexture2D{ 0 };
        return false;
    }

    rlDisableFramebuffer();
    SetTextureFilter(target_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(target_.texture, TEXTURE_WRAP_CLAMP);
    return true;
}

void HdrTarget::destroy()
{
    if (!target_.id) return;
    rlUnloadTexture(target_.texture.id);
    rlUnloadFramebuffer(target_.id);      /* takes the depth attachment with it */
    target_ = RenderTexture2D{ 0 };
}

}  // namespace xcom
