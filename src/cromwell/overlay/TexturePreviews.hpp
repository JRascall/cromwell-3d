/* TexturePreviews.hpp — render targets, made readable for the inspector.
 *
 * SINGLE RESPONSIBILITY: turn one of the renderer's internal buffers into an
 * image the dev panel can display, applying whatever remap makes that buffer's
 * content visible.
 *
 * WHY A COPY RATHER THAN A SHADER ON THE IMGUI DRAW. rlImGui defers: the widget
 * records a texture id and the actual draw happens later, inside rlImGuiEnd.
 * A BeginShaderMode around the widget call therefore applies to nothing, and a
 * single shared scratch target would show every entry the last one's contents.
 * One small target per slot, rendered before the panel runs, is the version
 * that works.
 *
 * The targets are eight-bit and small on purpose. This is a thing to look at,
 * not to measure — a preview that faithfully preserved a 16-bit depth value
 * would still be a picture of a number.
 */
#pragma once

#include "raylib.h"

#include <vector>

namespace cromwell {

class TexturePreviews {
public:
    /* How to make a given buffer legible. */
    enum class Mode : int {
        Raw     = 0,   /* colour buffers, which already are            */
        Stencil = 1,   /* object ids to distinct hues, empty to grey   */
        Depth   = 2,   /* linearised and banded                        */
        Alpha   = 3,   /* the alpha channel alone, as grey             */
    };

    TexturePreviews() = default;
    ~TexturePreviews();

    TexturePreviews(const TexturePreviews&) = delete;
    TexturePreviews& operator=(const TexturePreviews&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    /* The scale, in world units, that the depth ramp spans. Set it from the
     * scene's own size rather than guessing — a board 34 units across and a
     * 1000-unit far plane want completely different ramps. */
    void setDepthRange(float nearPlane, float farPlane, float scale);

    /* Renders `source` through `mode` into slot `index` and returns the
     * result, or `source` unchanged if the shader is missing. Slots are
     * stable: call with the same index each frame for the same buffer. */
    Texture2D render(int index, Texture2D source, Mode mode, int height = 192);

private:
    struct Slot {
        RenderTexture2D target{};
        int width = 0;
        int height = 0;
    };

    Shader shader_ = { 0 };
    int    locMode_ = -1;
    int    locNear_ = -1;
    int    locFar_ = -1;
    int    locDepthScale_ = -1;

    float  near_ = 0.01f;
    float  far_ = 1000.0f;
    float  depthScale_ = 40.0f;

    std::vector<Slot> slots_;
};

}  // namespace cromwell
