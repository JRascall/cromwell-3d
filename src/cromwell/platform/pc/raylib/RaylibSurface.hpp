/* RaylibSurface.hpp — ISurface, on a desktop window.
 *
 * SINGLE RESPONSIBILITY: satisfy ISurface using raylib's window functions, and
 * be one of the few files in the tree that is allowed to know raylib exists.
 *
 * ==================== WHERE THIS SITS, AND WHY ============================
 *
 * platform/pc/raylib/ — two axes, platform first, library second.
 *
 * `pc` is Windows, Linux and macOS together, because the platform question
 * ("what is a window, a save file, a gamepad") has the same answer on all
 * three. `raylib` is the third-party library currently answering it; a
 * hand-rolled GL layer would be platform/pc/opengl/ beside this, and a console
 * is platform/ps5/ with its own library folder inside.
 *
 * Both are selected by name in CMakeLists rather than by #ifdef, which matters
 * because a console build must not CONTAIN a desktop backend's code, not merely
 * avoid calling it. An #ifdef'd platform layer is also one where the console
 * path is only ever compiled by whoever is building for console, so every edit
 * to the desktop path silently breaks it until somebody notices weeks later.
 *
 * ================== WHAT THIS CANNOT HONESTLY PROVIDE =====================
 *
 * Very little, which is the point of desktop being the easy target: it has a
 * resizable window, a cursor, a clipboard and a title bar, so every capability
 * is true and every method does something. The one thing it invents is the safe
 * area, which is the whole surface here — correct, because a monitor does not
 * overscan, and it means desktop code that honours safeArea() is already
 * console-correct at no cost.
 */
#pragma once

#include "cromwell/platform/ISurface.hpp"

namespace cromwell {

class RaylibSurface final : public ISurface {
public:
    /* THE WINDOW IS ALREADY OPEN when this is constructed. RaylibPlatform calls
     * InitWindow itself, because the render device needs a live GL context and
     * therefore an existing window — see the construction order argument in
     * IPlatform.hpp. This class adopts that window rather than opening one, so
     * there is exactly one place that decides when a context comes into being. */
    RaylibSurface();

    /* Frame pacing, mirroring what SetTargetFPS does inside raylib EndDrawing —
     * which the device path never calls. 0 leaves the frame unthrottled. */
    void setTargetFrameRate(int framesPerSecond);

    const SurfaceCapabilities& capabilities() const override { return capabilities_; }

    void  size(int& width, int& height) const override;
    float scaleFactor() const override;
    SurfaceRect safeArea() const override;

    bool closeRequested() const override { return closeRequested_; }
    void cancelCloseRequest() override { closeRequested_ = false; }
    void pumpEvents() override;
    void present() override;
    bool takeResized() override;
    bool active() const override;

    void setTitle(const char* title) override;
    void setFullscreen(bool fullscreen) override;
    bool fullscreen() const override;
    void setCursorVisible(bool visible) override;
    void setVisible(bool visible) override;

    const char* clipboardText() const override;
    void        setClipboardText(const char* text) override;

private:
    SurfaceCapabilities capabilities_;

    /* LATCHED IN pumpEvents, NOT READ LIVE.
     *
     * raylib's WindowShouldClose() consumes the close request as a side effect
     * of asking, so calling it twice in a frame loses the answer. Latching it
     * once per pump is also what makes cancelCloseRequest possible at all — a
     * "really quit?" prompt the player declines has to be able to clear the
     * flag, which is impossible if the flag lives inside raylib. */
    bool closeRequested_ = false;
    bool resized_ = false;

    /* Seconds per frame, or 0 for unthrottled. See present(). */
    double targetFrameTime_ = 0.0;
    double lastPresentTime_ = 0.0;
};

}  // namespace cromwell
