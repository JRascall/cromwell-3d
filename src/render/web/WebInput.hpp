/* WebInput.hpp — raylib's input devices, pointed at a browser surface.
 *
 * SINGLE RESPONSIBILITY: turn screen-space mouse and keyboard state into
 * surface-space CEF events, and report what it took.
 *
 * POSITIONAL, NOT MODAL. The pointer belongs to whichever layer is under it
 * this frame and to nothing else — move off the panel and the camera orbits
 * again on the very next frame, with no click needed to let go. There is no
 * "the web UI is open so the web UI owns the mouse" state anywhere in here,
 * because that state is exactly what makes layered UI feel stuck.
 *
 * THE KEYBOARD IS GATED ON AN EDITABLE FIELD, not on visibility and not on
 * hover. A page that is merely on screen has no claim on the keyboard; a
 * focused text field does, because it is the only thing with somewhere to put
 * the keystrokes. WebSurface::wantsKeyboard answers that from Chromium's own
 * focus tracking, so game hotkeys keep working over an open page right up
 * until the caret lands in a search box.
 *
 * KEPT OUT OF WebSurface because the mapping is a policy, not a fact. A
 * fullscreen panel maps the pointer with a rectangle; a diegetic screen will
 * map it with a ray-quad intersection and a UV, using the same pickers the
 * world already uses. Both end at the same handful of WebSurface calls, and
 * only the arithmetic in front of them differs.
 */
#pragma once

#include "raylib.h"

namespace xcom {

class WebSurface;

/* What the surface took this frame, for the caller to subtract from whatever
 * it was going to do with the same input. Mirrors DevView::wantsMouse and
 * wantsKeyboard so that Application::arbitrate can treat the two layers alike. */
struct WebInputClaim {
    bool mouse    = false;  // the pointer was over the surface
    bool keyboard = false;  // keystrokes went to the page this frame
    int  characters = 0;    // how many were actually forwarded
};

/* Owned by the caller: click-to-focus and double-click detection both need to
 * remember something between frames. Hover does not — that is recomputed from
 * the pointer position every time, which is what keeps the model non-sticky. */
struct WebInputState {
    bool   pageFocused = false;  // has the page been clicked into
    bool   hovering    = false;  // was the pointer over it last frame
    double lastClickTime = 0.0;
    int    lastClickX = 0;
    int    lastClickY = 0;
};

/* `panel` is where the surface is drawn, in screen pixels.
 *
 * The two availability flags are how a layer ABOVE this one declines to pass
 * something down — the dev panel sets them from ImGui's WantCapture bits. They
 * are separate because the layers above genuinely take one without the other:
 * a hovered ImGui window owns the pointer while the keyboard is still free. */
WebInputClaim routeWebInput(WebSurface& surface, Rectangle panel, WebInputState& state,
                            bool mouseAvailable = true, bool keyboardAvailable = true);

}  // namespace xcom
