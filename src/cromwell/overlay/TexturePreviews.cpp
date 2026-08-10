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

Texture2D TexturePreviews::render(int index, Texture2D source, Mode mode, int height)
{
    if (!valid() || source.id == 0 || index < 0) return source;

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

    BeginShaderMode(shader_);

    const int modeValue = static_cast<int>(mode);
    SetShaderValue(shader_, locMode_, &modeValue, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, locNear_, &near_, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFar_, &far_, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locDepthScale_, &depthScale_, SHADER_UNIFORM_FLOAT);

    /* NEGATIVE SOURCE HEIGHT: the flip happens HERE, once, so that what this
     * function returns is an upright image and the viewer does not have to
     * know where it came from.
     *
     * Everything previewed lives in a framebuffer and is therefore stored
     * bottom-up. The inspector used to flip when it displayed, which was right
     * while it drew those buffers directly — and became one flip too many the
     * moment this copy sat in between, because the copy is itself a
     * framebuffer. Two flips is upside down again. Owning the correction at
     * the point the orientation is known is what stops that recurring. */
    const Rectangle from{ 0.0f, 0.0f,
                          static_cast<float>(source.width),
                          -static_cast<float>(source.height) };
    const Rectangle to{ 0.0f, 0.0f,
                        static_cast<float>(width), static_cast<float>(height) };
    DrawTexturePro(source, from, to, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);

    EndShaderMode();
    rlEnableColorBlend();
    EndTextureMode();

    return slot.target.texture;
}

}  // namespace cromwell
