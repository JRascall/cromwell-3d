/* ModalLoopPump.cpp — see the header for why this exists; what is documented
 * here is mechanism, all of it platform lore. */

#include "cromwell/platform/ModalLoopPump.hpp"

/* windows.h BEFORE glfw3.h, and the order is load-bearing: glfw3.h defines
 * APIENTRY itself whenever nothing has yet, and windows.h then redefines it —
 * a C4005 on every build. This way round, GLFW sees the definition already
 * present and skips its own. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/* GLFW is reached directly rather than through raylib. raylib neither exposes
 * its GLFWwindow* nor claims the window refresh callback (verified against the
 * vendored 5.5 — rcore_desktop_glfw.c never calls it), so the one handle this
 * file needs is recovered from the current GL context and the one callback it
 * wants is genuinely unowned. GLFW_INCLUDE_NONE because this file issues no
 * GL of its own. */
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace cromwell {
namespace {

/* File statics rather than members: the wndproc and the GLFW callback are
 * free functions with nowhere to carry an instance pointer — GLFW's window
 * user pointer belongs to whoever created the window, which is raylib, not
 * us. One window per process, so one pump; the class deletes its copies and
 * the header says why. */
std::function<void()> g_tick;
GLFWwindow*           g_window = nullptr;
int                   g_depth  = 0;

/* Every entry — the main loop's and the hooks' — comes through here. Depth 1
 * is the main loop; depth 2 is a nested tick fired from inside depth 1's
 * event poll, which is the entire point; depth 3 would be recursion down the
 * stack of a machine that is merely dragging a window, and is refused. */
void tickGuarded()
{
    if (!g_tick || g_depth >= 2) return;
    ++g_depth;
    g_tick();
    --g_depth;
}

/* GLFW's translation of "the OS says this window needs repainting" — Cocoa's
 * live-resize damage, an X11 expose, a Win32 WM_PAINT during a resize drag.
 * The documented use of this callback is to redraw from inside it, which is
 * exactly what happens. */
void onWindowRefresh(GLFWwindow* /*window*/) { tickGuarded(); }

#if defined(_WIN32)

WNDPROC g_chainProc = nullptr;

/* Checked on every WM_TIMER so somebody else's timer on this window is not
 * mistaken for ours. */
constexpr UINT_PTR kDragTimer = 0x4D4C50;  /* "MLP" */

/* Prepended to GLFW's own wndproc for exactly three messages; everything,
 * including those three, still reaches GLFW through the chain call at the
 * bottom, so nothing raylib relies on is swallowed. */
LRESULT CALLBACK pumpWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        /* The modal loop announces itself on the way in and out, which is
         * what makes the timer cost nothing outside a drag. USER_TIMER_MINIMUM
         * (10ms) sounds too fast and is not: the nested tick still pays
         * EndDrawing's frame pacing, so the timer sets the ceiling and
         * SetTargetFPS keeps setting the rate. */
        case WM_ENTERSIZEMOVE:
            SetTimer(window, kDragTimer, USER_TIMER_MINIMUM, nullptr);
            break;
        case WM_EXITSIZEMOVE:
            KillTimer(window, kDragTimer);
            break;
        /* WM_TIMER is the lowest-priority message there is — delivered only
         * when the queue is otherwise empty — which is exactly the property
         * wanted here: frames fill the gaps between drag messages rather
         * than competing with them. */
        case WM_TIMER:
            if (wParam == kDragTimer) tickGuarded();
            break;
        default:
            break;
    }
    return CallWindowProcW(g_chainProc, window, message, wParam, lParam);
}

#endif  /* _WIN32 */

}  // namespace

ModalLoopPump::ModalLoopPump(TickFn tick)
{
    g_tick = std::move(tick);

    /* raylib made the context current on this thread in InitWindow, which is
     * how the GLFWwindow* is reachable without raylib exposing it. Null means
     * there is no window — a headless run — and the pump degrades to tick()
     * calling the frame directly. */
    g_window = glfwGetCurrentContext();
    if (g_window == nullptr) return;

    glfwSetWindowRefreshCallback(g_window, onWindowRefresh);

#if defined(_WIN32)
    /* The W variant, deliberately: GLFW registers a Unicode window class, and
     * subclassing one with the A functions would route every message through
     * an ANSI conversion. */
    HWND handle = glfwGetWin32Window(g_window);
    /* NOLINT below: SetWindowLongPtrW returns the previous wndproc as a
     * LONG_PTR — there is no pointer-typed form of this API to prefer. */
    g_chainProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(  // NOLINT(performance-no-int-to-ptr)
        handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&pumpWndProc)));
#endif
}

ModalLoopPump::~ModalLoopPump()
{
    if (g_window != nullptr) {
#if defined(_WIN32)
        if (g_chainProc != nullptr) {
            SetWindowLongPtrW(glfwGetWin32Window(g_window), GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_chainProc));
            g_chainProc = nullptr;
        }
#endif
        glfwSetWindowRefreshCallback(g_window, nullptr);
        g_window = nullptr;
    }
    g_tick = nullptr;
}

void ModalLoopPump::tick() { tickGuarded(); }

}  // namespace cromwell
