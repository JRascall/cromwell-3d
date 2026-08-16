/* DeviceImGuiRenderer.hpp — Dear ImGui's draw data, through rhi::IRenderDevice.
 *
 * SINGLE RESPONSIBILITY: turn one frame of `ImDrawData` into device draws, and
 * own the textures ImGui asks for. It builds no panel and reads no input.
 *
 * ================= WHY THIS IS THE GAME'S AND NOT THE ENGINE'S ============
 *
 * Because the dev panel is. CMakeLists says it plainly where the imgui target
 * is declared: "imgui is deliberately NOT here — the dev panel is the game's,
 * and an engine that drags a UI toolkit into every project that embeds it has
 * made a choice on that project's behalf." cromwell must not link imgui, so its
 * backend cannot live there, even though a backend is engine-shaped work.
 *
 * The consequence is one file in `game/` that calls `rhi::IRenderDevice`, which
 * is allowed and unremarkable — the game may use the engine's interfaces. What
 * it must never become is a second way for the game to draw the WORLD; this
 * draws a tool, on top of a finished frame, in display colour.
 *
 * ============== WHY NOT rlImGui, WHICH ALREADY WORKS ====================
 *
 * rlImGui is both halves of a backend: it reads raylib's input AND draws
 * through rlgl. The second half is exactly what the port removes — it binds
 * shaders by raylib's naming convention and issues its own GL — so the device
 * path cannot use it and still be a device path.
 *
 * **THE INPUT HALF IS STILL rlImGui's, AND THAT IS DELIBERATE.** The window,
 * the mouse and the keyboard are still raylib's on this path (the platform
 * backend is `pc/raylib`), so `rlImGuiBegin()` is the working, tested answer to
 * "what did the user do", and rewriting it against `IInput` today would be a
 * second answer to a question that already has one. It moves when the platform
 * does; see the note at the top of MIGRATION.md on the two axes, platform and
 * library, changing independently. What must NOT happen is `rlImGuiEnd()`,
 * which renders — the caller calls `ImGui::Render()` and this class instead.
 *
 * ============ IMGUI 1.92 MANAGES TEXTURES, AND A BACKEND MUST TOO =========
 *
 * This is the part that would be got wrong by writing a backend from memory.
 * Since 1.92 a renderer backend sets `ImGuiBackendFlags_RendererHasTextures`
 * and services requests on `ImDrawData::Textures` — create, update, destroy —
 * rather than uploading one font atlas at startup and never thinking again.
 * The font atlas is then just the first texture ImGui asks for, and it can ask
 * again at any time: a font reload, a DPI change or a glyph that was not in the
 * atlas yet all arrive as `WantCreate` or `WantUpdates` mid-run.
 *
 * A backend that ignored this would show a panel that is correct on the first
 * frame and turns to garbage the first time a new glyph is used, which reads as
 * a font bug.
 *
 * =================== THE ONE PLACE THE RHI DOES NOT FIT ==================
 *
 * **ImGui's indices are 16-bit and this RHI draws 32-bit ones.** `ImDrawIdx` is
 * `unsigned short` and the GL backend issues `GL_UNSIGNED_INT`, so the indices
 * are WIDENED on the way into the buffer.
 *
 * The alternative was defining `ImDrawIdx` as `unsigned int` on the imgui
 * target, which is one line — and was rejected because that definition is
 * PUBLIC and would change the raylib path's imgui too, for the benefit of a
 * renderer that is not yet the shipping one. Widening costs one pass over the
 * indices of a dev panel.
 *
 * **AND THE WIDENING DOES DOUBLE DUTY**, which is what makes it cheap rather
 * than merely tolerable. `drawIndexed` takes a first INDEX but no base VERTEX,
 * so a draw list's indices — which are relative to its own vertex block — have
 * to be rebased anyway when several lists share one buffer. The offset is added
 * in the same pass that widens them, and 32 bits is what makes that offset safe
 * past 65,535 vertices.
 */
#pragma once

#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace cromwell::rhi { class IRenderDevice; }

namespace game {

class DeviceImGuiRenderer {
public:
    explicit DeviceImGuiRenderer(cromwell::rhi::IRenderDevice& device);
    ~DeviceImGuiRenderer();

    DeviceImGuiRenderer(const DeviceImGuiRenderer&) = delete;
    DeviceImGuiRenderer& operator=(const DeviceImGuiRenderer&) = delete;

    /* Loads the shader pair and builds the pipeline, and tells ImGui that this
     * backend manages its own textures. Call once, after an ImGui context
     * exists — the backend flag is set on the current context's IO.
     *
     * False means the panel cannot be drawn; it has logged which stage failed.
     * A dev tool that cannot start must not take the frame down with it. */
    bool initialise();

    /* ONE FRAME OF PANELS, onto the current target, in display colour. Call
     * after `ImGui::Render()`; `ImGui::GetDrawData()` is the argument.
     *
     * Opens its own pass and leaves nothing bound — the same contract every
     * other pass in this renderer has. Null or empty draw data is a no-op and
     * not even a pass, because an empty pass on a tiler still stores and
     * reloads the attachment. */
    void render(const ImDrawData* drawData);

    /* Destroys every texture ImGui asked for, plus the pipeline and buffers.
     * Safe to call twice; the destructor calls it. */
    void release();

private:
    /* Services one texture request — create, update or destroy. This is the
     * 1.92 contract; see the header. */
    void updateTexture(ImTextureData* texture);

    /* Grows the vertex and index buffers to hold at least this much, keeping
     * the high-water mark. Returns false when the device refused, which is the
     * one case that silently draws nothing. */
    bool ensureCapacity(uint32_t vertices, uint32_t indices);

    cromwell::rhi::IRenderDevice& device_;

    cromwell::rhi::ShaderHandle   shader_;
    cromwell::rhi::PipelineHandle pipeline_;
    cromwell::rhi::SamplerHandle  sampler_;

    cromwell::rhi::BufferHandle vertexBuffer_;
    cromwell::rhi::BufferHandle indexBuffer_;
    cromwell::rhi::MeshHandle   mesh_;

    uint32_t vertexCapacity_ = 0;
    uint32_t indexCapacity_ = 0;

    /* KEPT ACROSS FRAMES FOR THEIR CAPACITY, cleared rather than freed — the
     * same arrangement ScenePipeline's debug line scratch has. A panel is
     * rebuilt every frame from scratch, so these settle at the high-water mark
     * and then allocate nothing. */
    std::vector<std::uint8_t>  vertexScratch_;
    std::vector<std::uint32_t> indexScratch_;

    /* WHAT WE HANDED IMGUI, so release() can take them back. ImGui holds the
     * id in ImTextureData::TexID and hands it back on WantDestroy, so this list
     * exists only for the teardown path where ImGui is not asking. */
    std::vector<cromwell::rhi::TextureHandle> textures_;

    bool ready_ = false;
};

}  // namespace game
