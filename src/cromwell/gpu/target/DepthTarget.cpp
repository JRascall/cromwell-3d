#include "cromwell/gpu/target/DepthTarget.hpp"

#include "rlgl.h"

namespace cromwell {
namespace {

constexpr int kDepthComponent24 = 19;

}  // namespace

void DepthTarget::create(int width, int height, int colourFormat)
{
    target_ = RenderTexture2D{ 0 };
    target_.id = rlLoadFramebuffer();
    if (!target_.id) {
        TraceLog(LOG_WARNING, "RIBBON: could not create FBO");
        return;
    }

    rlEnableFramebuffer(target_.id);

    target_.texture.id      = rlLoadTexture(nullptr, width, height, colourFormat, 1);
    target_.texture.width   = width;
    target_.texture.height  = height;
    target_.texture.format  = colourFormat;
    target_.texture.mipmaps = 1;

    target_.depth.id      = rlLoadTextureDepth(width, height, false);   /* false = TEXTURE */
    target_.depth.width   = width;
    target_.depth.height  = height;
    target_.depth.format  = kDepthComponent24;
    target_.depth.mipmaps = 1;

    rlFramebufferAttach(target_.id, target_.texture.id,
                        RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(target_.id, target_.depth.id,
                        RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);

    if (!rlFramebufferComplete(target_.id)) TraceLog(LOG_WARNING, "RIBBON: FBO incomplete");
    rlDisableFramebuffer();
}

void DepthTarget::destroy()
{
    if (!target_.id) return;
    rlUnloadTexture(target_.texture.id);
    rlUnloadTexture(target_.depth.id);        /* a texture, not a renderbuffer */
    rlUnloadFramebuffer(target_.id);
    target_ = RenderTexture2D{ 0 };
}

}  // namespace cromwell
