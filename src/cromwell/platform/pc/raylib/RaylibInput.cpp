#include "cromwell/platform/pc/raylib/RaylibInput.hpp"

#include "cromwell/diag/Logger.hpp"

#include "raylib.h"

namespace cromwell {
namespace {

constexpr int kUnmapped = -1;

int index(Key key)           { return static_cast<int>(key); }
int index(MouseButton b)     { return static_cast<int>(b); }
int index(GamepadButton b)   { return static_cast<int>(b); }

}  // namespace

RaylibInput::RaylibInput()
{
    keyCodes_.fill(kUnmapped);
    mouseCodes_.fill(kUnmapped);
    padCodes_.fill(kUnmapped);

    auto bind = [this](Key key, int code) { keyCodes_[index(key)] = code; };

    bind(Key::A, KEY_A); bind(Key::B, KEY_B); bind(Key::C, KEY_C); bind(Key::D, KEY_D);
    bind(Key::E, KEY_E); bind(Key::F, KEY_F); bind(Key::G, KEY_G); bind(Key::H, KEY_H);
    bind(Key::I, KEY_I); bind(Key::J, KEY_J); bind(Key::K, KEY_K); bind(Key::L, KEY_L);
    bind(Key::M, KEY_M); bind(Key::N, KEY_N); bind(Key::O, KEY_O); bind(Key::P, KEY_P);
    bind(Key::Q, KEY_Q); bind(Key::R, KEY_R); bind(Key::S, KEY_S); bind(Key::T, KEY_T);
    bind(Key::U, KEY_U); bind(Key::V, KEY_V); bind(Key::W, KEY_W); bind(Key::X, KEY_X);
    bind(Key::Y, KEY_Y); bind(Key::Z, KEY_Z);

    bind(Key::Num0, KEY_ZERO);  bind(Key::Num1, KEY_ONE);   bind(Key::Num2, KEY_TWO);
    bind(Key::Num3, KEY_THREE); bind(Key::Num4, KEY_FOUR);  bind(Key::Num5, KEY_FIVE);
    bind(Key::Num6, KEY_SIX);   bind(Key::Num7, KEY_SEVEN); bind(Key::Num8, KEY_EIGHT);
    bind(Key::Num9, KEY_NINE);

    bind(Key::F1, KEY_F1);   bind(Key::F2, KEY_F2);   bind(Key::F3, KEY_F3);
    bind(Key::F4, KEY_F4);   bind(Key::F5, KEY_F5);   bind(Key::F6, KEY_F6);
    bind(Key::F7, KEY_F7);   bind(Key::F8, KEY_F8);   bind(Key::F9, KEY_F9);
    bind(Key::F10, KEY_F10); bind(Key::F11, KEY_F11); bind(Key::F12, KEY_F12);

    bind(Key::Escape, KEY_ESCAPE);      bind(Key::Enter, KEY_ENTER);
    bind(Key::Tab, KEY_TAB);            bind(Key::Backspace, KEY_BACKSPACE);
    bind(Key::Space, KEY_SPACE);        bind(Key::Delete, KEY_DELETE);
    bind(Key::Insert, KEY_INSERT);      bind(Key::Home, KEY_HOME);
    bind(Key::End, KEY_END);            bind(Key::PageUp, KEY_PAGE_UP);
    bind(Key::PageDown, KEY_PAGE_DOWN);

    bind(Key::Left, KEY_LEFT); bind(Key::Right, KEY_RIGHT);
    bind(Key::Up, KEY_UP);     bind(Key::Down, KEY_DOWN);

    bind(Key::LeftShift, KEY_LEFT_SHIFT);     bind(Key::RightShift, KEY_RIGHT_SHIFT);
    bind(Key::LeftControl, KEY_LEFT_CONTROL); bind(Key::RightControl, KEY_RIGHT_CONTROL);
    bind(Key::LeftAlt, KEY_LEFT_ALT);         bind(Key::RightAlt, KEY_RIGHT_ALT);
    bind(Key::LeftSuper, KEY_LEFT_SUPER);     bind(Key::RightSuper, KEY_RIGHT_SUPER);

    bind(Key::Grave, KEY_GRAVE);              bind(Key::Minus, KEY_MINUS);
    bind(Key::Equals, KEY_EQUAL);             bind(Key::LeftBracket, KEY_LEFT_BRACKET);
    bind(Key::RightBracket, KEY_RIGHT_BRACKET); bind(Key::Backslash, KEY_BACKSLASH);
    bind(Key::Semicolon, KEY_SEMICOLON);      bind(Key::Apostrophe, KEY_APOSTROPHE);
    bind(Key::Comma, KEY_COMMA);              bind(Key::Period, KEY_PERIOD);
    bind(Key::Slash, KEY_SLASH);

    bind(Key::KeypadO, KEY_KP_0); bind(Key::Keypad1, KEY_KP_1); bind(Key::Keypad2, KEY_KP_2);
    bind(Key::Keypad3, KEY_KP_3); bind(Key::Keypad4, KEY_KP_4); bind(Key::Keypad5, KEY_KP_5);
    bind(Key::Keypad6, KEY_KP_6); bind(Key::Keypad7, KEY_KP_7); bind(Key::Keypad8, KEY_KP_8);
    bind(Key::Keypad9, KEY_KP_9);
    bind(Key::KeypadAdd, KEY_KP_ADD);           bind(Key::KeypadSubtract, KEY_KP_SUBTRACT);
    bind(Key::KeypadMultiply, KEY_KP_MULTIPLY); bind(Key::KeypadDivide, KEY_KP_DIVIDE);
    bind(Key::KeypadEnter, KEY_KP_ENTER);       bind(Key::KeypadDecimal, KEY_KP_DECIMAL);

    bind(Key::CapsLock, KEY_CAPS_LOCK);   bind(Key::NumLock, KEY_NUM_LOCK);
    bind(Key::ScrollLock, KEY_SCROLL_LOCK); bind(Key::PrintScreen, KEY_PRINT_SCREEN);
    bind(Key::Pause, KEY_PAUSE);

    mouseCodes_[index(MouseButton::Left)]   = MOUSE_BUTTON_LEFT;
    mouseCodes_[index(MouseButton::Right)]  = MOUSE_BUTTON_RIGHT;
    mouseCodes_[index(MouseButton::Middle)] = MOUSE_BUTTON_MIDDLE;
    mouseCodes_[index(MouseButton::Extra1)] = MOUSE_BUTTON_SIDE;
    mouseCodes_[index(MouseButton::Extra2)] = MOUSE_BUTTON_EXTRA;

    /* BY POSITION, NOT BY PRINTED LETTER — see IInput.hpp. raylib names these
     * after a PlayStation pad's shapes, so RIGHT_FACE_DOWN is the bottom of the
     * diamond whatever the controller prints on it, which is exactly the
     * mapping the engine wants. */
    padCodes_[index(GamepadButton::FaceDown)]  = GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
    padCodes_[index(GamepadButton::FaceRight)] = GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
    padCodes_[index(GamepadButton::FaceLeft)]  = GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
    padCodes_[index(GamepadButton::FaceUp)]    = GAMEPAD_BUTTON_RIGHT_FACE_UP;

    padCodes_[index(GamepadButton::LeftShoulder)]  = GAMEPAD_BUTTON_LEFT_TRIGGER_1;
    padCodes_[index(GamepadButton::RightShoulder)] = GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
    padCodes_[index(GamepadButton::LeftTrigger)]   = GAMEPAD_BUTTON_LEFT_TRIGGER_2;
    padCodes_[index(GamepadButton::RightTrigger)]  = GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
    padCodes_[index(GamepadButton::LeftStick)]     = GAMEPAD_BUTTON_LEFT_THUMB;
    padCodes_[index(GamepadButton::RightStick)]    = GAMEPAD_BUTTON_RIGHT_THUMB;

    padCodes_[index(GamepadButton::DpadUp)]    = GAMEPAD_BUTTON_LEFT_FACE_UP;
    padCodes_[index(GamepadButton::DpadDown)]  = GAMEPAD_BUTTON_LEFT_FACE_DOWN;
    padCodes_[index(GamepadButton::DpadLeft)]  = GAMEPAD_BUTTON_LEFT_FACE_LEFT;
    padCodes_[index(GamepadButton::DpadRight)] = GAMEPAD_BUTTON_LEFT_FACE_RIGHT;

    padCodes_[index(GamepadButton::Start)]  = GAMEPAD_BUTTON_MIDDLE_RIGHT;
    padCodes_[index(GamepadButton::Select)] = GAMEPAD_BUTTON_MIDDLE_LEFT;
    padCodes_[index(GamepadButton::Guide)]  = GAMEPAD_BUTTON_MIDDLE;

    /* THE TABLE IS CHECKED, WHICH IS THE REASON IT IS A TABLE.
     *
     * A Key added to the enum and forgotten here is otherwise a key that
     * silently does nothing — the hardest kind of input bug to find, because
     * every other key works and the binding screen looks correct. This turns it
     * into a line in the startup log naming the index.
     *
     * Warning rather than fatal: a genuinely unmappable key (a console backend
     * with no keyboard at all) should not stop the game starting. */
    for (int i = 0; i < kKeyCount; i++) {
        if (i == static_cast<int>(Key::Unknown)) continue;
        if (keyCodes_[i] == kUnmapped)
            LOGGER.warn("RaylibInput: Key index {} has no raylib mapping", i);
    }
}

void RaylibInput::poll()
{
    /* PREVIOUS FRAME FIRST. Every edge below is "changed since the last time
     * you asked", which is the only definition that survives a frame the game
     * skipped or a fixed step that polls twice. */
    keyWas_   = keyNow_;
    mouseWas_ = mouseNow_;

    for (int i = 0; i < kKeyCount; i++)
        keyNow_[i] = keyCodes_[i] != kUnmapped && IsKeyDown(keyCodes_[i]);

    for (int i = 0; i < kMouseCount; i++)
        mouseNow_[i] = mouseCodes_[i] != kUnmapped && IsMouseButtonDown(mouseCodes_[i]);

    modifiers_ = ModNone;
    if (keyNow_[index(Key::LeftShift)] || keyNow_[index(Key::RightShift)])
        modifiers_ |= ModShift;
    if (keyNow_[index(Key::LeftControl)] || keyNow_[index(Key::RightControl)])
        modifiers_ |= ModControl;
    if (keyNow_[index(Key::LeftAlt)] || keyNow_[index(Key::RightAlt)])
        modifiers_ |= ModAlt;
    if (keyNow_[index(Key::LeftSuper)] || keyNow_[index(Key::RightSuper)])
        modifiers_ |= ModSuper;

    /* IN SURFACE PIXELS, matching ISurface::size — raylib reports the pointer
     * in logical units, which differ on a high-DPI display. Handing the logical
     * value to a UI laid out in pixels puts every hit test out by the scale
     * factor, which reads as "buttons only work near the top-left". */
    const int logicalWidth = GetScreenWidth();
    const float scale = logicalWidth > 0
                            ? static_cast<float>(GetRenderWidth()) / static_cast<float>(logicalWidth)
                            : 1.0f;

    const Vector2 position = GetMousePosition();
    const Vector2 delta    = GetMouseDelta();
    const Vector2 wheel    = GetMouseWheelMoveV();

    pointer_      = Vec2{ position.x * scale, position.y * scale };
    pointerDelta_ = Vec2{ delta.x * scale, delta.y * scale };
    wheel_        = Vec2{ wheel.x, wheel.y };

    for (int pad = 0; pad < kMaxGamepads; pad++) {
        PadState& state = pads_[pad];
        state.was = state.now;
        state.connected = IsGamepadAvailable(pad);

        for (int i = 0; i < kPadButtonCount; i++) {
            state.now[i] = state.connected && padCodes_[i] != kUnmapped
                        && IsGamepadButtonDown(pad, padCodes_[i]);
        }
    }

    /* Text entry, satisfied immediately on desktop but through the same state
     * machine a console drives — see the header. */
    if (textState_ == TextEntryState::Active) {
        for (int codepoint = GetCharPressed(); codepoint > 0; codepoint = GetCharPressed()) {
            if (static_cast<int>(textBuffer_.size()) >= textMaxLength_) break;

            /* ASCII only for now: the buffer is a std::string and the UI's text
             * stack is what would have to agree about encoding. A console's
             * system keyboard returns UTF-8, so this is the narrower case and
             * widening it later does not change the interface. */
            if (codepoint >= 32 && codepoint < 127)
                textBuffer_.push_back(static_cast<char>(codepoint));
        }

        if (IsKeyPressed(KEY_BACKSPACE) && !textBuffer_.empty()) textBuffer_.pop_back();

        if (IsKeyPressed(KEY_ESCAPE))                     textState_ = TextEntryState::Cancelled;
        else if (IsKeyPressed(KEY_ENTER) && !textMultiline_) textState_ = TextEntryState::Committed;
    }
}

bool RaylibInput::down(Key key) const     { return keyNow_[index(key)]; }
bool RaylibInput::pressed(Key key) const  { return keyNow_[index(key)] && !keyWas_[index(key)]; }
bool RaylibInput::released(Key key) const { return !keyNow_[index(key)] && keyWas_[index(key)]; }

bool RaylibInput::down(MouseButton b) const    { return mouseNow_[index(b)]; }
bool RaylibInput::pressed(MouseButton b) const { return mouseNow_[index(b)] && !mouseWas_[index(b)]; }
bool RaylibInput::released(MouseButton b) const { return !mouseNow_[index(b)] && mouseWas_[index(b)]; }

bool RaylibInput::gamepadConnected(int pad) const
{
    return pad >= 0 && pad < kMaxGamepads && pads_[pad].connected;
}

bool RaylibInput::down(int pad, GamepadButton button) const
{
    if (!gamepadConnected(pad)) return false;
    return pads_[pad].now[index(button)];
}

bool RaylibInput::pressed(int pad, GamepadButton button) const
{
    if (!gamepadConnected(pad)) return false;
    return pads_[pad].now[index(button)] && !pads_[pad].was[index(button)];
}

bool RaylibInput::released(int pad, GamepadButton button) const
{
    if (!gamepadConnected(pad)) return false;
    return !pads_[pad].now[index(button)] && pads_[pad].was[index(button)];
}

float RaylibInput::axis(int pad, GamepadAxis which) const
{
    if (!gamepadConnected(pad)) return 0.0f;

    int code = -1;
    switch (which) {
        case GamepadAxis::LeftX:        code = GAMEPAD_AXIS_LEFT_X; break;
        case GamepadAxis::LeftY:        code = GAMEPAD_AXIS_LEFT_Y; break;
        case GamepadAxis::RightX:       code = GAMEPAD_AXIS_RIGHT_X; break;
        case GamepadAxis::RightY:       code = GAMEPAD_AXIS_RIGHT_Y; break;
        case GamepadAxis::LeftTrigger:  code = GAMEPAD_AXIS_LEFT_TRIGGER; break;
        case GamepadAxis::RightTrigger: code = GAMEPAD_AXIS_RIGHT_TRIGGER; break;
        default: return 0.0f;
    }

    const float raw = GetGamepadAxisMovement(pad, code);

    /* TRIGGERS ARE REMAPPED, STICKS ARE NOT. raylib reports a trigger as -1
     * released to +1 fully pulled; IInput specifies 0..1, because a trigger has
     * a rest position rather than a centre and "half of nothing is -0.5" is a
     * sentence no caller should have to reason about.
     *
     * NO DEAD ZONE ON EITHER — see IInput.hpp. The right one differs between a
     * twin-stick aim and a menu cursor, and applying one here would make the
     * small ones unreachable. */
    const bool trigger = which == GamepadAxis::LeftTrigger || which == GamepadAxis::RightTrigger;
    return trigger ? (raw + 1.0f) * 0.5f : raw;
}

void RaylibInput::setRumble(int pad, float lowFrequency, float highFrequency)
{
    /* raylib has no rumble API. A no-op rather than an error: IInput specifies
     * that rumble is absent where the platform has none, and a game calling it
     * should not have to know which platform it is on. */
    (void)pad;
    (void)lowFrequency;
    (void)highFrequency;
}

bool RaylibInput::beginTextEntry(const TextEntryRequest& request)
{
    if (textState_ == TextEntryState::Active) return false;

    textBuffer_    = request.initialText != nullptr ? request.initialText : "";
    textMaxLength_ = request.maxLength > 0 ? request.maxLength : 256;
    textMultiline_ = request.multiline;
    textState_     = TextEntryState::Active;

    /* DRAIN THE QUEUE. Whatever the player typed to OPEN the field — the key
     * that triggered it — is still sitting in raylib's character queue and
     * would otherwise appear as the first character of the new text. */
    while (GetCharPressed() > 0) {}

    return true;
}

const char* RaylibInput::takeTextEntry()
{
    textState_ = TextEntryState::Idle;
    return textBuffer_.c_str();
}

void RaylibInput::cancelTextEntry()
{
    textState_ = TextEntryState::Cancelled;
}

}  // namespace cromwell
