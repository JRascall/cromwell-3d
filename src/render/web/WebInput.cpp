#include "render/web/WebInput.hpp"

#include "render/web/WebSurface.hpp"

#include <array>
#include <cmath>

namespace xcom {
namespace {

/* Windows' default double-click window. Chromium decides "select the word"
 * versus "place the caret" from the click count it is handed, so it has to be
 * worked out here — raylib reports presses, not gestures. */
constexpr double kDoubleClickSeconds = 0.5;
constexpr float  kDoubleClickSlop    = 4.0f;

/* One scroll notch. Chromium's wheel handling is written against the
 * WHEEL_DELTA that Windows sends, and a raw 1.0 from raylib scrolls by a pixel. */
constexpr float kWheelNotch = 120.0f;

constexpr std::array<int, 3> kMouseButtons = {
    MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE
};

/* Keys with no character of their own, which therefore have to travel as key
 * events. Printable keys are left to GetCharPressed, which already knows the
 * layout and the modifier state.
 *
 * ESCAPE IS NOT HERE ON PURPOSE. It is the one key a user presses to get OUT
 * of a text field, and routing it into the page would be the one thing that
 * could genuinely trap them. It falls through to the game, which closes the
 * panel. */
constexpr std::array<int, 14> kNavigationKeys = {
    KEY_BACKSPACE, KEY_TAB,   KEY_ENTER,   KEY_KP_ENTER,
    KEY_DELETE,    KEY_LEFT,  KEY_RIGHT,   KEY_UP,        KEY_DOWN,
    KEY_HOME,      KEY_END,   KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_F5
};

/* Only meaningful with control held: unmodified, these already arrive as
 * characters and sending both would type the letter twice. */
constexpr std::array<int, 6> kShortcutKeys = {
    KEY_A, KEY_C, KEY_V, KEY_X, KEY_Z, KEY_L
};

}  // namespace

WebInputClaim routeWebInput(WebSurface& surface, Rectangle panel, WebInputState& state,
                            bool mouseAvailable, bool keyboardAvailable)
{
    WebInputClaim claim;
    if (!surface.valid() || panel.width <= 0.0f || panel.height <= 0.0f) return claim;

    const Vector2 mouse = GetMousePosition();

    /* A layer above has the pointer, so as far as this surface is concerned the
     * pointer is elsewhere — which also runs the mouseLeave below and drops any
     * hover state the page was holding. */
    const bool inside = mouseAvailable && CheckCollisionPointRec(mouse, panel);

    /* Screen pixels to surface texels. The panel is not necessarily the same
     * size as the surface — the window is resizable and the browser is not
     * re-laid-out every time it changes — so this is a scale, not an offset. */
    const float scaleX = static_cast<float>(surface.width())  / panel.width;
    const float scaleY = static_cast<float>(surface.height()) / panel.height;
    const int   sx = static_cast<int>((mouse.x - panel.x) * scaleX);
    const int   sy = static_cast<int>((mouse.y - panel.y) * scaleY);

    if (inside) {
        surface.mouseMove(sx, sy);

        const Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.x != 0.0f || wheel.y != 0.0f)
            surface.mouseWheel(sx, sy, wheel.x * kWheelNotch, wheel.y * kWheelNotch);

        for (const int button : kMouseButtons) {
            if (IsMouseButtonPressed(button)) {
                int clickCount = 1;
                if (button == MOUSE_BUTTON_LEFT) {
                    const double now = GetTime();
                    const bool nearby =
                        std::fabs(static_cast<float>(sx - state.lastClickX)) <= kDoubleClickSlop &&
                        std::fabs(static_cast<float>(sy - state.lastClickY)) <= kDoubleClickSlop;
                    if (nearby && now - state.lastClickTime <= kDoubleClickSeconds) clickCount = 2;

                    state.lastClickTime = (clickCount == 2) ? 0.0 : now;
                    state.lastClickX = sx;
                    state.lastClickY = sy;
                }
                /* FOCUS FIRST, THEN THE CLICK, AND THE ORDER IS THE WHOLE
                 * POINT. Chromium will not give a text field the caret from a
                 * click delivered to a browser it believes is unfocused, so
                 * setting focus after the click means the very first click on
                 * a search box lands as a plain click and focuses nothing.
                 *
                 * This is also what claims the keyboard, now that the gate is
                 * click-to-focus rather than caret-in-a-field. */
                state.pageFocused = true;
                surface.setFocused(true);

                surface.mouseButton(sx, sy, button, true, clickCount);
            }
            if (IsMouseButtonReleased(button)) surface.mouseButton(sx, sy, button, false, 1);
        }
    } else {
        if (state.hovering) surface.mouseLeave();

        /* A release that began inside still has to be delivered, or the page is
         * left believing the button is held — dragging out of the panel would
         * otherwise stick a selection on forever. */
        for (const int button : kMouseButtons)
            if (IsMouseButtonReleased(button)) surface.mouseButton(sx, sy, button, false, 1);

        for (const int button : kMouseButtons)
            if (IsMouseButtonPressed(button)) state.pageFocused = false;
    }

    /* ESCAPE LETS GO, AND IS NEVER FORWARDED. Now that focus rather than a
     * focused text field decides who gets the keyboard, the page holds it for
     * as long as it is focused — so there has to be a way out that does not
     * require finding somewhere else to click. Escape is the key everyone
     * already tries. It is absent from kNavigationKeys for the same reason:
     * the key that releases the page must never be swallowed by the page. */
    if (state.pageFocused && IsKeyPressed(KEY_ESCAPE)) state.pageFocused = false;

    state.hovering = inside;
    surface.setFocused(state.pageFocused);

    /* THE GATE. Not "is it visible" and not "is the pointer over it" — has the
     * page been clicked into, and has no layer above claimed the keyboard
     * first. It used to ask Chromium whether a caret was in a text field, which
     * is a better rule that could not be made to work; see
     * WebSurface::wantsKeyboard. */
    const bool typing = keyboardAvailable && surface.wantsKeyboard();

    if (typing) {
        /* GetCharPressed, AND IT IS SAFE HERE FOR A PRECISE REASON. rlImGui
         * does drain raylib's character queue — but only inside
         * `if (io.WantCaptureKeyboard)`, and typing above is false whenever
         * that is true. The two are exact complements: when ImGui wants the
         * keyboard it takes the characters and the page must not have them,
         * and when it does not, rlImGui never touches the queue and every
         * character is still sitting in it.
         *
         * Reading ImGui's InputQueueCharacters instead — which is what this
         * did first — cannot work for the same reason. That queue is only ever
         * filled by the drain above, so in the one case that matters it is
         * guaranteed empty, and typing silently did nothing.
         *
         * Key events are unaffected either way: raylib's key state is polled
         * rather than consumed. */
        while (const int codepoint = GetCharPressed()) {
            surface.character(codepoint);
            ++claim.characters;
        }

        for (const int key : kNavigationKeys) {
            if (IsKeyPressed(key) || IsKeyPressedRepeat(key)) surface.key(key, true);
            if (IsKeyReleased(key)) surface.key(key, false);
        }

        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
            for (const int key : kShortcutKeys) {
                if (IsKeyPressed(key))  surface.key(key, true);
                if (IsKeyReleased(key)) surface.key(key, false);
            }
        }
    } else {
        /* DRAINED AND THROWN AWAY. raylib's character queue is a small ring
         * that nothing else is emptying in this case either, so anything typed
         * while the page was not focused would still be sitting there and
         * would arrive all at once the moment it was — a sentence typed at the
         * game appearing in a search box several seconds later. */
        while (GetCharPressed()) {}
    }

    claim.mouse    = inside;
    claim.keyboard = typing;
    return claim;
}

}  // namespace xcom
