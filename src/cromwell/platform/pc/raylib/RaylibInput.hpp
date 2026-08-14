/* RaylibInput.hpp — IInput, on desktop devices.
 *
 * SINGLE RESPONSIBILITY: translate raylib's input state into the engine's
 * vocabulary, once per frame, and hold the mapping tables that do it.
 *
 * ==================== THE MAPPING IS A TABLE, NOT A switch =================
 *
 * A switch over a hundred key codes compiles to a jump table anyway, but it
 * cannot be checked. A table can: the constructor verifies that every Key the
 * engine declares has an entry, so a key ADDED to the enum and forgotten here
 * is caught at startup with a name rather than discovered as a key that does
 * nothing. That check costs one loop at construction and it is the difference
 * between a port that is finished and a port that appears to be.
 *
 * ===================== WHY EDGES ARE COMPUTED HERE ========================
 *
 * raylib offers IsKeyPressed alongside IsKeyDown, so pressed/released could
 * simply forward. They are computed from the previous frame's snapshot instead,
 * for a reason worth stating: raylib's edge state is tied to ITS frame, which
 * is defined by its own event pump, and the engine's poll() may not line up
 * with that one-to-one — a frame the game skips, a modal loop that pumps
 * several times, a fixed-step update that polls twice. Computing edges from
 * the engine's own snapshots means "pressed" always means "since the last time
 * YOU asked", which is the only definition a caller can reason about.
 *
 * It also makes the gamepad path identical to the keyboard one rather than
 * two different mechanisms with two different edge cases.
 */
#pragma once

#include "cromwell/input/IInput.hpp"

#include <array>
#include <string>

namespace cromwell {

class RaylibInput final : public IInput {
public:
    RaylibInput();

    void poll() override;

    bool     down(Key key) const override;
    bool     pressed(Key key) const override;
    bool     released(Key key) const override;
    uint32_t modifiers() const override { return modifiers_; }
    bool     hasKeyboard() const override { return true; }

    bool down(MouseButton button) const override;
    bool pressed(MouseButton button) const override;
    bool released(MouseButton button) const override;
    Vec2 pointerPosition() const override { return pointer_; }
    Vec2 pointerDelta() const override { return pointerDelta_; }
    Vec2 wheelDelta() const override { return wheel_; }
    bool hasPointer() const override { return true; }

    bool  gamepadConnected(int pad) const override;
    bool  down(int pad, GamepadButton button) const override;
    bool  pressed(int pad, GamepadButton button) const override;
    bool  released(int pad, GamepadButton button) const override;
    float axis(int pad, GamepadAxis axis) const override;
    void  setRumble(int pad, float lowFrequency, float highFrequency) override;

    bool           beginTextEntry(const TextEntryRequest& request) override;
    TextEntryState textEntryState() const override { return textState_; }
    const char*    takeTextEntry() override;
    void           cancelTextEntry() override;

private:
    static constexpr int kKeyCount  = static_cast<int>(Key::Count);
    static constexpr int kMouseCount = static_cast<int>(MouseButton::Count);
    static constexpr int kPadButtonCount = static_cast<int>(GamepadButton::Count);

    /* Engine key -> raylib key code. -1 for a key raylib does not express. */
    std::array<int, kKeyCount> keyCodes_{};
    std::array<int, kMouseCount> mouseCodes_{};
    std::array<int, kPadButtonCount> padCodes_{};

    /* Two snapshots, so an edge is "since the last poll" — see the header. */
    std::array<bool, kKeyCount> keyNow_{};
    std::array<bool, kKeyCount> keyWas_{};
    std::array<bool, kMouseCount> mouseNow_{};
    std::array<bool, kMouseCount> mouseWas_{};

    struct PadState {
        std::array<bool, kPadButtonCount> now{};
        std::array<bool, kPadButtonCount> was{};
        bool connected = false;
    };
    std::array<PadState, kMaxGamepads> pads_{};

    uint32_t modifiers_ = ModNone;
    Vec2 pointer_{};
    Vec2 pointerDelta_{};
    Vec2 wheel_{};

    /* DESKTOP SATISFIES TEXT ENTRY IMMEDIATELY, by collecting raylib's
     * character queue while a request is active. The state machine is honoured
     * exactly as a console would drive it — Active until the player presses
     * Enter or Escape — so the call sites are already written for the
     * asynchronous case rather than being retrofitted for it later. */
    TextEntryState textState_ = TextEntryState::Idle;
    std::string    textBuffer_;
    int            textMaxLength_ = 256;
    bool           textMultiline_ = false;
};

}  // namespace cromwell
