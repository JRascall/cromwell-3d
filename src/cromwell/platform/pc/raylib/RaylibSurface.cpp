#include "cromwell/platform/pc/raylib/RaylibSurface.hpp"

#include "raylib.h"

namespace cromwell {

RaylibSurface::RaylibSurface()
{
    /* Every one of these is genuinely true on a desktop window, which is why
     * desktop is the easy target and why the interface's absences are all
     * console-shaped. */
    capabilities_.resizable        = true;
    capabilities_.fullscreen       = true;
    capabilities_.cursor           = true;
    capabilities_.clipboard        = true;
    capabilities_.title            = true;
    capabilities_.multipleMonitors = true;
}

void RaylibSurface::size(int& width, int& height) const
{
    /* GetRenderWidth, NOT GetScreenWidth.
     *
     * On a high-DPI display these differ by the scale factor: GetScreenWidth is
     * the logical size the OS reports and GetRenderWidth is the drawable size
     * in real pixels. ISurface::size is specified as pixels because that is
     * what a render target must be sized to, and using the logical one is the
     * classic bug where everything renders into the bottom-left quarter of a
     * Retina window. */
    width  = GetRenderWidth();
    height = GetRenderHeight();
}

float RaylibSurface::scaleFactor() const
{
    /* Derived from the two sizes rather than from GetWindowScaleDPI, because
     * this is the ratio that actually matters — the one between the coordinates
     * input arrives in and the pixels the renderer writes. A reported DPI scale
     * that disagreed with the real buffer ratio would put the pointer out of
     * step with the UI it is clicking on. */
    const int logical = GetScreenWidth();
    if (logical <= 0) return 1.0f;

    return static_cast<float>(GetRenderWidth()) / static_cast<float>(logical);
}

SurfaceRect RaylibSurface::safeArea() const
{
    /* THE WHOLE SURFACE. A monitor does not overscan, so there is nothing to
     * inset — and that is exactly what makes honouring safeArea() free on
     * desktop. UI laid out against it here is already correct on a television
     * without anybody revisiting it. */
    return SurfaceRect{ 0, 0, GetRenderWidth(), GetRenderHeight() };
}

void RaylibSurface::present()
{
    /* THIS IS raylib's EndDrawing, MINUS THE DRAWING — and it is deliberately a
     * mirror of it rather than a reimplementation.
     *
     * EndDrawing does FOUR things in one call: swaps the buffer, polls input
     * events, counts the frame and waits to hit the target frame rate. An
     * earlier version of this function did only the swap and moved the poll to
     * the top of the frame, which looked equivalent and was not:
     *
     *   - SetTargetFPS had no effect at all, because raylib applies it inside
     *     EndDrawing's WaitTime and nowhere else. The loop then ran unthrottled,
     *     which starves every per-frame input delta: the pointer moves a few
     *     pixels per second of real time spread over thousands of frames, so
     *     each frame sees almost nothing and an orbit drag barely registers.
     *
     *   - The wheel is worse. raylib zeroes the accumulator at the START of
     *     every poll, so at several thousand polls a second the one frame that
     *     sees a scroll event is swamped by the thousands that see zero.
     *
     * Anything else that presents — a console backend, a native GL window — has
     * the same four jobs to do at the same point. Doing them where the platform
     * already ends its frame is what keeps that true. */
    SwapScreenBuffer();
    PollInputEvents();

    if (targetFrameTime_ <= 0.0) return;

    /* WAIT OUT THE REMAINDER, exactly as EndDrawing does. Measured from the
     * last present rather than from the frame's start, so the pacing covers the
     * whole frame including the wait itself and cannot drift. */
    const double now = GetTime();
    const double elapsed = now - lastPresentTime_;

    if (elapsed < targetFrameTime_) {
        WaitTime(targetFrameTime_ - elapsed);
        lastPresentTime_ = GetTime();
    } else {
        lastPresentTime_ = now;
    }
}

void RaylibSurface::setTargetFrameRate(int framesPerSecond)
{
    targetFrameTime_ = framesPerSecond > 0 ? 1.0 / static_cast<double>(framesPerSecond) : 0.0;
    lastPresentTime_ = GetTime();
}

void RaylibSurface::pumpEvents()
{
    /* NO POLLING HERE. Input is pumped by whichever call ENDS the frame — raylib
     * EndDrawing on the old path, present() on the device path — because the
     * wheel and the pointer delta are per-poll accumulators and a second poll
     * zeroes whichever read came after it. See present(). */
    /* WindowShouldClose() is what drives raylib's event pump AND it consumes
     * the close request as a side effect of being asked. Both facts mean it
     * must be called exactly once per frame, here, with the answer latched —
     * see the note on closeRequested_. */
    if (WindowShouldClose()) closeRequested_ = true;

    if (IsWindowResized()) resized_ = true;
}

bool RaylibSurface::takeResized()
{
    const bool resized = resized_;
    resized_ = false;
    return resized;
}

bool RaylibSurface::active() const
{
    /* Not merely "has focus" — a window that is minimised or fully hidden is
     * one whose frame clock should not be trusted and whose rendering is
     * wasted. Focus alone would stop the game running behind a chat window,
     * which is not the same thing and is usually unwanted. */
    return !IsWindowMinimized() && !IsWindowHidden();
}

void RaylibSurface::setTitle(const char* title)
{
    if (title != nullptr) SetWindowTitle(title);
}

void RaylibSurface::setFullscreen(bool wanted)
{
    /* IDEMPOTENT, because raylib's is a TOGGLE and the interface's is a state.
     * Calling ToggleFullscreen unconditionally from a settings screen that
     * writes its whole state on apply would flip the window every time the
     * player pressed OK, whatever the checkbox said. */
    if (IsWindowFullscreen() == wanted) return;
    ToggleFullscreen();
}

bool RaylibSurface::fullscreen() const { return IsWindowFullscreen(); }

void RaylibSurface::setCursorVisible(bool visible)
{
    if (visible) ShowCursor();
    else         HideCursor();
}

void RaylibSurface::setVisible(bool visible)
{
    /* FLAG_WINDOW_HIDDEN is set at creation and cleared here — the reveal after
     * the first present. Idempotent, like setFullscreen and for the same
     * reason: these are states in the interface and flags in raylib, and a
     * caller writing its whole state on every settings apply must not toggle
     * anything. */
    if (!visible) {
        SetWindowState(FLAG_WINDOW_HIDDEN);
        return;
    }

    ClearWindowState(FLAG_WINDOW_HIDDEN);

    /* AND TAKE FOCUS, which unhiding does NOT do on its own.
     *
     * A window mapped from hidden appears without keyboard focus, so every key
     * goes to whatever the player was looking at before — the symptom is a game
     * that ignores WASD until you click on it once, which reads as broken input
     * rather than as a focus problem. The click that "fixes" it is the click
     * that focuses the window.
     *
     * This is why the reveal is a deliberate call rather than a side effect of
     * the first present: there is a second thing to do here. */
    SetWindowFocused();
}

const char* RaylibSurface::clipboardText() const
{
    /* NEVER NULL. raylib returns null when the clipboard is empty or holds
     * something that is not text, and ISurface promises a string — a caller
     * pasting into a text field should get nothing pasted, not a crash. */
    const char* text = GetClipboardText();
    return text != nullptr ? text : "";
}

void RaylibSurface::setClipboardText(const char* text)
{
    SetClipboardText(text != nullptr ? text : "");
}

}  // namespace cromwell
