/* OverlayShader.hpp — the unlit shader the gameplay overlays draw through.
 *
 * SINGLE RESPONSIBILITY: hold one trivial shader and scope it around a block
 * of immediate-mode drawing.
 *
 * The overlays are the one thing in the scene that must NOT be lit — a cover
 * shield or a LOS tint means the same thing whatever the sun is doing. But
 * they are drawn into the linear HDR target so that they still depth-test
 * against the world, and their palette entries are authored in sRGB. Without
 * this shader those authored values would be read as linear, come out far too
 * bright, and then be tonemapped on top of the error.
 *
 * It hooks raylib's immediate batch rather than replacing the draw calls: the
 * overlays are DrawCube, DrawSphere and DrawLine3D, and BeginShaderMode swaps
 * the program the batch flushes through without any of them changing.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class OverlayShader {
public:
    OverlayShader() = default;
    ~OverlayShader();

    OverlayShader(const OverlayShader&) = delete;
    OverlayShader& operator=(const OverlayShader&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    /* Draws through the unlit shader for the block's lifetime. A no-op if the
     * shader failed to load, which leaves the overlays visible but too bright
     * rather than absent. */
    class Scope {
    public:
        explicit Scope(const OverlayShader& shader) : active_(shader.valid())
        {
            if (active_) BeginShaderMode(shader.shader_);
        }
        ~Scope() { if (active_) EndShaderMode(); }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        bool active_;
    };

private:
    Shader shader_ = { 0 };
};

}  // namespace cromwell
