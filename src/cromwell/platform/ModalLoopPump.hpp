/* ModalLoopPump.hpp — frames keep presenting while the OS owns the thread.
 *
 * THE PROBLEM. On Windows, grabbing the title bar or a resize border parks
 * this thread inside the OS's modal move/size loop: DefWindowProc answers
 * WM_SYSCOMMAND by running its OWN message pump and does not return until the
 * mouse button is released. A single-threaded frame loop is stuck wherever it
 * happened to be — inside glfwPollEvents, at the tail of EndDrawing — so no
 * frame is presented for the whole drag. The compositor keeps moving the
 * window using the last frame it was ever given, and the white that shows at
 * the edges is the window background bleeding through where DWM outran the
 * stalled renderer.
 *
 * THE FIX IS NOT PORTABLE, BECAUSE THE STALL IS NOT PORTABLE. What each
 * platform needs:
 *
 *   Windows   the modal loop announces itself (WM_ENTERSIZEMOVE) and will
 *             deliver WM_TIMER from inside itself — so a timer is hooked in
 *             and each tick renders a frame. This is the classic fix, and
 *             there is no other single-threaded one: moving a window exposes
 *             no new content, so no paint message ever arrives to piggyback
 *             on.
 *   macOS     Cocoa's live-resize runs its own event-tracking loop, but it
 *             DOES emit damage — GLFW turns that into the window refresh
 *             callback, which is wired here already. Untested until there is
 *             a mac build; title-bar moves never stalled there to begin with.
 *   Linux     nothing. The window manager moves the window; the app's loop
 *             never blocks, and this class compiles down to the refresh
 *             callback answering the occasional X11 expose.
 *
 * WHY THE FRAME MUST BE A CALLABLE. Both hooks fire from INSIDE the stuck
 * call — the wndproc and the refresh callback are invoked underneath
 * glfwPollEvents while the previous frame's EndDrawing is still on the stack.
 * Re-entering the frame there is safe precisely because of where the stall
 * sits: the old frame has already swapped, every GPU zone is closed, and the
 * only thing the outer frame has left to do is return. A loop written as a
 * block cannot be entered from a callback; a loop written as "while (...)
 * pump.tick()" over a callable can.
 *
 * THE DEPTH GUARD, and why the limit is two. Entry one is the main loop's own
 * call. Entry two is a nested tick fired from inside entry one's event poll —
 * the entire point of the class. Entry three would be a tick nested inside
 * the NESTED tick's poll, and that way lies unbounded recursion down the
 * stack of a machine that is merely dragging a window. tick() refuses it, so
 * a drag renders one frame at a time exactly as the main loop does.
 *
 * A CAPTURED DRAG SHOWS NESTED "frame" ROWS in the profiler, one inside the
 * other. That is the truth of what happened — a frame was rendered from
 * inside another frame's event poll — and it is confined to the drag.
 *
 * LIFETIME. Construct AFTER the window exists (the constructor reads the
 * current GL context to find it); destroy BEFORE the window closes (the
 * destructor unhooks from a window that must still be alive). A scoped block
 * around the frame loop is the natural shape. Constructed with no window at
 * all — a headless run — it degrades to tick() calling the frame directly.
 */
#pragma once

#include <functional>

namespace cromwell {

class ModalLoopPump {
public:
    using TickFn = std::function<void()>;

    explicit ModalLoopPump(TickFn tick);
    ~ModalLoopPump();

    /* ONE PER PROCESS, like the window it hooks. The Win32 wndproc and the
     * GLFW callback are free functions with no closure to carry an instance,
     * so the state behind this class is file-static in the .cpp — a second
     * pump would fight the first for the hook chain, and nothing needs one. */
    ModalLoopPump(const ModalLoopPump&) = delete;
    ModalLoopPump& operator=(const ModalLoopPump&) = delete;

    /* The main loop calls the frame THROUGH this rather than directly, so
     * that every entry — the loop's and the hooks' — passes the same depth
     * guard. See the header comment on why the limit is two. */
    void tick();
};

}  // namespace cromwell
