/* IInput.hpp — every way a player can say something, in the engine's vocabulary.
 *
 * SINGLE RESPONSIBILITY: report the state of keyboard, pointer and gamepads for
 * the frame being simulated, in enums no input library owns.
 *
 * ==================== GAMEPAD IS HERE BEFORE IT IS USED ====================
 *
 * Nothing in this codebase reads a gamepad today — the count is zero. It is in
 * this interface anyway, and deliberately, because CONSOLE IS GAMEPAD-FIRST and
 * an interface that gained gamepad support later would gain it as an
 * afterthought bolted to the side of a keyboard model.
 *
 * The specific thing being avoided: engines that add pads late end up mapping
 * buttons to synthetic key presses, because that is the cheapest way to reach
 * code already written against a keyboard. That works until the first thing
 * that needs an analogue value — a stick that steers, a trigger that varies —
 * and by then every consumer expects booleans. Analogue is in the interface
 * from the start for that reason, not because anything needs it yet.
 *
 * ============== TEXT ENTRY IS ASYNCHRONOUS, AND THAT IS NOT OPTIONAL =======
 *
 * On desktop, typing produces characters. On console it does NOT: the system
 * takes over the screen with its own on-screen keyboard, the player types
 * there, and the application gets a string back some seconds later — or gets
 * nothing, because they cancelled.
 *
 * That is a genuinely different shape and it cannot be hidden behind
 * `getCharPressed()`. So text entry is a REQUEST with a result polled for, and
 * the desktop backend satisfies it immediately from its own character queue.
 * Writing it the other way round — desktop's model as the interface, console
 * bolted on — is how a game ships with a text field nobody on console can type
 * into.
 *
 * ========================== STATE, NOT EVENTS ==============================
 *
 * `down` / `pressed` / `released` are the shape the whole codebase already uses
 * (see input/FrameInput.hpp), and they are a good fit for a game loop that
 * samples once a frame. `pressed` means "went down since the last poll" — edge
 * detection belongs to the backend, which is the only thing that sees every
 * transition including the ones that happened twice inside one frame.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"

#include <cstdint>

namespace cromwell {

/* ---- keyboard -------------------------------------------------------------
 *
 * PHYSICAL KEY POSITIONS, not the characters they produce. `Key::Q` is the key
 * where Q sits on a US layout, and it stays that key on AZERTY where it prints
 * an A — which is what movement bindings want, and is why WASD does not become
 * ZQSD for a French player unless the game chooses to rebind it. Characters
 * come from the text-entry path below, which is layout-aware; these do not. */
enum class Key : uint16_t {
    Unknown = 0,

    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    Escape, Enter, Tab, Backspace, Space, Delete, Insert,
    Home, End, PageUp, PageDown,
    Left, Right, Up, Down,

    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,
    LeftSuper, RightSuper,

    Grave, Minus, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon, Apostrophe, Comma, Period, Slash,

    KeypadO, Keypad1, Keypad2, Keypad3, Keypad4,
    Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadAdd, KeypadSubtract, KeypadMultiply, KeypadDivide, KeypadEnter, KeypadDecimal,

    CapsLock, NumLock, ScrollLock, PrintScreen, Pause,

    Count,
};

/* Held modifiers, as a mask, because a shortcut asks about several at once and
 * `ctrl && shift && !alt` reads worse than one comparison. */
enum KeyModifier : uint32_t {
    ModNone    = 0,
    ModShift   = 1u << 0,
    ModControl = 1u << 1,
    ModAlt     = 1u << 2,
    ModSuper   = 1u << 3,
};

/* ---- pointer -------------------------------------------------------------*/

enum class MouseButton : uint8_t { Left, Right, Middle, Extra1, Extra2, Count };

/* ---- gamepad -------------------------------------------------------------
 *
 * NAMED BY POSITION, NOT BY LETTER. `FaceDown` rather than `A` or `Cross`,
 * because the letter differs by platform AND the LAYOUT differs — Nintendo's A
 * is where Xbox's B sits. A codebase that says `A` has to remember which
 * vendor's A it meant at every call site, and the bug that produces is "confirm
 * and cancel are swapped", which is both a certification failure and the single
 * most irritating thing a port can get wrong.
 *
 * So: position here, and the one place that maps position to a printed glyph is
 * the UI's button-prompt code, which is the only thing that should care. */
enum class GamepadButton : uint8_t {
    FaceDown, FaceRight, FaceLeft, FaceUp,     /* the diamond, clockwise from bottom */
    LeftShoulder, RightShoulder,
    LeftTrigger, RightTrigger,                 /* the digital sense of an analogue pull */
    LeftStick, RightStick,                     /* clicked in */
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Start, Select, Guide,
    Count,
};

enum class GamepadAxis : uint8_t {
    LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count,
};

/* ---- text entry ----------------------------------------------------------*/

enum class TextEntryState : uint8_t {
    Idle,        /* nothing requested */
    Active,      /* desktop: collecting. console: the system keyboard is up */
    Committed,   /* the player finished - read it, which returns to Idle */
    Cancelled,   /* they backed out. NOT the same as an empty string */
};

struct TextEntryRequest {
    const char* prompt      = nullptr;   /* shown by the system keyboard */
    const char* initialText = nullptr;
    int         maxLength   = 256;
    bool        password    = false;
    bool        multiline   = false;
};

class IInput {
public:
    virtual ~IInput() = default;

    /* Sample the platform once for this frame. Everything below reports the
     * state as of the last call, so a frame sees one consistent snapshot rather
     * than values that change under it mid-update. */
    virtual void poll() = 0;

    /* ---- keyboard -------------------------------------------------------*/
    virtual bool down(Key key) const = 0;
    virtual bool pressed(Key key) const = 0;
    virtual bool released(Key key) const = 0;
    virtual uint32_t modifiers() const = 0;

    /* True when a keyboard exists at all. False on a console with none
     * attached, which is a reason to offer the on-screen keyboard rather than a
     * reason to disable a text field. */
    virtual bool hasKeyboard() const = 0;

    /* ---- pointer --------------------------------------------------------*/
    virtual bool down(MouseButton button) const = 0;
    virtual bool pressed(MouseButton button) const = 0;
    virtual bool released(MouseButton button) const = 0;

    /* IN SURFACE PIXELS, matching ISurface::size() — not logical units. The two
     * differ on a high-DPI display, and a pointer tested against a UI rectangle
     * in the wrong one of them is off by the scale factor, which reads as
     * "buttons only work near the top-left". */
    virtual Vec2 pointerPosition() const = 0;
    virtual Vec2 pointerDelta() const = 0;
    virtual Vec2 wheelDelta() const = 0;

    virtual bool hasPointer() const = 0;

    /* ---- gamepad --------------------------------------------------------*/
    static constexpr int kMaxGamepads = 8;

    virtual bool gamepadConnected(int pad) const = 0;
    virtual bool down(int pad, GamepadButton button) const = 0;
    virtual bool pressed(int pad, GamepadButton button) const = 0;
    virtual bool released(int pad, GamepadButton button) const = 0;

    /* -1..1 for sticks, 0..1 for triggers. RAW — no dead zone applied, because
     * the right dead zone is a game decision that differs per use (a twin-stick
     * aim wants a small radial one, a menu cursor wants a large one) and a
     * backend that applied its own would make the good ones unreachable. */
    virtual float axis(int pad, GamepadAxis axis) const = 0;

    /* 0..1 per motor. A no-op where there is no rumble; console certification
     * generally requires it be switchable, which is the game's setting to
     * honour, not this layer's. */
    virtual void setRumble(int pad, float lowFrequency, float highFrequency) = 0;

    /* ---- text entry, asynchronous by design -----------------------------*/
    virtual bool           beginTextEntry(const TextEntryRequest& request) = 0;
    virtual TextEntryState textEntryState() const = 0;

    /* Valid only when the state is Committed; returns it and resets to Idle. */
    virtual const char* takeTextEntry() = 0;
    virtual void        cancelTextEntry() = 0;
};

}  // namespace cromwell
