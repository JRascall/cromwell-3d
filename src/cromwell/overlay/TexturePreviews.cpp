#include "cromwell/overlay/TexturePreviews.hpp"

#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"

namespace cromwell {

TexturePreviews::~TexturePreviews()
{
    for (Slot& slot : slots_)
        if (slot.target.id) UnloadRenderTexture(slot.target);

    if (shader_.id) UnloadShader(shader_);
}

bool TexturePreviews::load()
{
    /* Fragment stage only — raylib's default vertex shader already gives a
     * textured quad the right varyings, and this draws nothing else. */
    shader_ = ShaderLibrary::load(nullptr, "debug_preview.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "PREVIEW: no shader - the inspector will show raw buffers");
        return false;
    }

    locMode_       = GetShaderLocation(shader_, "uMode");
    locNear_       = GetShaderLocation(shader_, "uNear");
    locFar_        = GetShaderLocation(shader_, "uFar");
    locDepthScale_ = GetShaderLocation(shader_, "uDepthScale");
    return true;
}

void TexturePreviews::setDepthRange(float nearPlane, float farPlane, float scale)
{
    near_ = nearPlane;
    far_ = farPlane;
    depthScale_ = scale;
}

Texture2D TexturePreviews::render(int index, Texture2D source, Mode mode, Origin origin,
                                  int height)
{
    if (source.id == 0 || index < 0) return source;

    /* A MISSING SHADER SKIPS THE REMAP, NOT THE COPY. Returning `source`
     * straight back would hand the viewer a raw bottom-up framebuffer while
     * every other entry beside it came back top-down, so the fallback would be
     * upside down and nothing else would — the worst kind of inconsistency to
     * debug, because it looks like a property of the buffer rather than of the
     * path it took. Raw is the identity remap anyway; the copy still runs and
     * still supplies the flip, and only Depth, Stencil and Alpha degrade. */
    const bool remap = valid();

    if (index >= static_cast<int>(slots_.size())) slots_.resize(index + 1);
    Slot& slot = slots_[static_cast<std::size_t>(index)];

    /* Aspect preserved from the source, so a six-wide cubemap strip and a
     * square shadow map both stay readable at one height. */
    const float aspect = (source.height > 0)
                       ? static_cast<float>(source.width) / static_cast<float>(source.height)
                       : 1.0f;
    const int width = static_cast<int>(static_cast<float>(height) * aspect + 0.5f);

    if (slot.target.id == 0 || slot.width != width || slot.height != height) {
        if (slot.target.id) UnloadRenderTexture(slot.target);
        slot.target = LoadRenderTexture(width, height);
        slot.width = width;
        slot.height = height;
    }
    if (slot.target.id == 0) return source;

    BeginTextureMode(slot.target);

    /* A COPY, NOT A COMPOSITE — and it was a composite, which made this
     * inspector lie about the very buffers it exists to show.
     *
     * The Raw mode passes the source's alpha straight through, and the
     * G-buffer's alpha is ROUGHNESS. With raylib's default alpha blending and a
     * slot that was never cleared, a surface at roughness 0.75 was drawn at 75%
     * opacity over whatever that slot held previously — so the normal preview
     * showed the current frame's normals ghosted over stale contents, which
     * reads exactly like seeing geometry through an opaque wall. Anybody
     * diagnosing from it, including me, is then chasing an artefact of the
     * viewer.
     *
     * Clearing first and blending not at all makes the preview show what is in
     * the texture and nothing else. */
    ClearBackground(BLANK);
    rlDisableColorBlend();

    if (remap) {
        BeginShaderMode(shader_);

        const int modeValue = static_cast<int>(mode);
        SetShaderValue(shader_, locMode_, &modeValue, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, locNear_, &near_, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locFar_, &far_, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locDepthScale_, &depthScale_, SHADER_UNIFORM_FLOAT);
    }

    /* THE SIGN IS THE WHOLE CORRECTION, and counting the flips is the part that
     * keeps going wrong here, so it is spelled out.
     *
     * Writing into a slot is a framebuffer write, and a framebuffer write flips
     * what it stores. That flip is free and it is always there; the only
     * question is whether the source needs it.
     *
     *   Framebuffer source — stored bottom-up, so it needs exactly one flip.
     *     The copy already is that flip. POSITIVE rectangle, no correction.
     *   Image source — already top-down, so it needs none, and the copy is
     *     about to apply one anyway. NEGATIVE rectangle, to cancel it.
     *
     * Every entry used to take the negative branch. That is right for the two
     * lightmap textures, which come from LoadTextureFromImage, and wrong for
     * the twelve render targets, which is why those were upside down while the
     * lightmaps looked fine — a split that reads like a property of the buffers
     * and is really a property of this rectangle. Note that a negative height
     * is what a blit to the BACKBUFFER always wants (see ToneMapPass), because
     * the backbuffer supplies no second flip; that is a different case from
     * either of these two and is not a precedent for them. */
    const float sourceHeight = static_cast<float>(source.height);
    const Rectangle from{ 0.0f, 0.0f,
                          static_cast<float>(source.width),
                          origin == Origin::Framebuffer ? sourceHeight : -sourceHeight };
    const Rectangle to{ 0.0f, 0.0f,
                        static_cast<float>(width), static_cast<float>(height) };
    DrawTexturePro(source, from, to, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);

    if (remap) EndShaderMode();
    rlEnableColorBlend();
    EndTextureMode();

    return slot.target.texture;
}

}  // namespace cromwell
