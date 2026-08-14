/* ISurface.hpp — the thing the engine draws into, whatever kind of thing that is.
 *
 * SINGLE RESPONSIBILITY: own the drawable surface's lifetime and size, and
 * report the handful of properties a renderer and a UI layer genuinely need.
 *
 * ===================== WHY "SURFACE" AND NOT "WINDOW" =====================
 *
 * Because on half the targets there is no window.
 *
 * A console application does not create one, cannot be resized, has no title
 * bar, no minimise, no cursor and often no clipboard. It is handed a display
 * surface at a fixed resolution, plus a SAFE AREA it must keep its interface
 * inside because the panel may overscan. Naming this type `IWindow` would bake
 * the desktop's assumptions into every call site, and the console backend would
 * spend its life returning polite lies from methods that mean nothing.
 *
 * So the type is named for what every target has — somewhere to draw — and the
 * desktop-only affordances are grouped, asked about before use, and honestly
 * absent rather than faked.
 *
 * ==================== THE SAFE AREA IS NOT AN AFTERTHOUGHT =================
 *
 * `safeArea()` returns the whole surface on desktop, so desktop code that
 * honours it costs nothing and is already correct. Console submission
 * requirements are explicit about interface elements staying inside it, and the
 * failure mode of ignoring it — a HUD with its corners cut off on a television
 * — is found at certification, which is the most expensive place to find
 * anything. Laying it out against `safeArea()` from the start is free; going
 * back through a finished HUD is not.
 *
 * ====================== SIZE IS IN PIXELS, ALWAYS ==========================
 *
 * `size()` is the DRAWABLE size in pixels, which on a high-DPI display is not
 * the logical size the OS reports and not what a mouse coordinate arrives in.
 * Conflating the two is the classic "everything renders into the bottom-left
 * quarter of the window" bug on a Retina display. The scale factor is reported
 * separately so a UI layer can lay out in logical units and still rasterise at
 * device resolution — which is what cromwell's text stack needs to stay crisp
 * (see the note on hinting and whole-pixel placement in the UI kit).
 */
#pragma once

#include <cstdint>

namespace cromwell {

/* What this surface's platform is actually capable of. Asked, not assumed —
 * every one of these is false on at least one shipping target. */
struct SurfaceCapabilities {
    bool resizable      = false;
    bool fullscreen     = false;   /* meaningless where there is no desktop */
    bool cursor         = false;   /* a console has no pointer to show or hide */
    bool clipboard      = false;
    bool title          = false;   /* nothing displays it without a title bar */
    bool multipleMonitors = false;
};

/* Pixels. Signed, because a difference of two of these is a legitimate value
 * and an unsigned subtraction that goes negative is a very large number. */
struct SurfaceRect {
    int x = 0, y = 0, width = 0, height = 0;
};

class ISurface {
public:
    virtual ~ISurface() = default;

    virtual const SurfaceCapabilities& capabilities() const = 0;

    /* ---- the two sizes, and they are different ---------------------------*/

    /* DRAWABLE pixels — what a render target should be sized to. */
    virtual void size(int& width, int& height) const = 0;

    /* Device pixels per logical unit: 1.0 on an ordinary display, 2.0 on a
     * Retina one, fractional on a scaled Windows desktop. A UI that lays out in
     * logical units and rasterises at `size()` needs both. */
    virtual float scaleFactor() const = 0;

    /* WHERE INTERFACE MAY SAFELY GO. The whole surface on desktop; inset on a
     * television. See the header note — honouring this from the start is free
     * and retrofitting it is not. */
    virtual SurfaceRect safeArea() const = 0;

    /* ---- lifetime -------------------------------------------------------*/

    /* True once the platform has asked the application to stop — a close
     * button, a system shutdown request, a console suspend that will not
     * resume. The application decides what to do about it; this only reports
     * that it was asked. */
    virtual bool closeRequested() const = 0;

    /* Acknowledge and clear the request — for a "really quit?" prompt that the
     * player might cancel. Without this, a declined prompt would be re-asked
     * every frame forever. */
    virtual void cancelCloseRequest() = 0;

    /* Pump the platform's own message queue. Separate from input polling
     * because on Windows this is what keeps the window responsive during a
     * modal drag or resize, and it has to run even on frames the game is not
     * simulating — see platform/ModalLoopPump.hpp, which exists entirely
     * because of that. */
    virtual void pumpEvents() = 0;

    /* SHOW WHAT WAS DRAWN — swap the back buffer to the front.
     *
     * ON THE SURFACE RATHER THAN THE RENDER DEVICE, and the split is worth
     * stating because most APIs blur it. Presenting is a WINDOW-SYSTEM
     * operation — wglSwapBuffers, glXSwapBuffers, eglSwapBuffers, and a
     * console's own equivalent — not a graphics-API one. The device's own
     * `present()` submits and flushes what was recorded; this puts the result
     * on the display. Keeping them apart is what lets a graphics backend be
     * replaced without touching windowing, which is the whole reason
     * rhi/pc/opengl/ and platform/pc/raylib/ are different directories.
     *
     * CALL IT ONCE PER FRAME AND ONLY ONCE. While the renderer still draws
     * through raylib, raylib's own EndDrawing already swaps — so during the
     * migration this is called only on the frames the device drew. See
     * IPlatform::endFrame. */
    virtual void present() = 0;

    /* True when the surface changed size since the last call, and clears. A
     * poll rather than a callback because every consumer of it — the render
     * targets, the UI layout — wants to react once, at a defined point in the
     * frame, not re-entrantly from inside an OS message handler. */
    virtual bool takeResized() = 0;

    /* Whether the surface is currently visible and receiving input. False when
     * minimised, occluded, or when a console has been suspended to the system
     * menu — all of which are reasons to stop rendering and, importantly, to
     * stop trusting the frame clock. */
    virtual bool active() const = 0;

    /* ---- desktop affordances, absent where they mean nothing -------------
     *
     * ALL OF THESE ARE NO-OPS when the matching capability is false, and that
     * is the deliberate contract: a caller should not have to branch on the
     * platform to set a window title. Ask capabilities() when you want to hide
     * a settings row; otherwise just call and let it do nothing. */
    virtual void setTitle(const char* title) = 0;
    virtual void setFullscreen(bool fullscreen) = 0;
    virtual bool fullscreen() const = 0;
    virtual void setCursorVisible(bool visible) = 0;

    /* SHOW THE SURFACE, once there is something worth looking at.
     *
     * A desktop process spends its first second or two compiling shaders,
     * building meshes and baking light, and a window that exists throughout is
     * an empty grey rectangle that reads as a hang. So it is created hidden
     * (PlatformDesc::startHidden) and revealed after the first frame has
     * actually been presented — the first thing on screen is then the splash
     * rather than a blank frame that becomes the splash.
     *
     * A no-op where a surface cannot be hidden, which is every console: there
     * is no window to withhold, and the platform shows its own boot screen
     * until the application presents. That the desktop trick and the console
     * behaviour collapse to the same call is the point — the caller reveals
     * after its first present either way and never asks which it is on. */
    virtual void setVisible(bool visible) = 0;

    /* Empty string when there is no clipboard, rather than a null nobody
     * checks. */
    virtual const char* clipboardText() const = 0;
    virtual void        setClipboardText(const char* text) = 0;
};

}  // namespace cromwell
