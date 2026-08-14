#include "cromwell/input/InputHandler.hpp"

#include "cromwell/input/IInput.hpp"
#include "cromwell/platform/IClock.hpp"
#include "cromwell/platform/ISurface.hpp"

namespace cromwell {

FrameInput InputHandler::sample(const IInput& input, const IClock& clock, ISurface& surface,
                                std::optional<int> forcedMouseX,
                                std::optional<int> forcedMouseY) const
{
    FrameInput frame;

    /* THE CLOCK'S OWN DELTA, already clamped. This used to be
     * `GetFrameTime()` with a hand-rolled 0.05 ceiling right here — one of
     * several such ceilings scattered around, which is exactly the duplication
     * FrameClock exists to end. The clamp now lives in one place and every
     * consumer gets the safe value; see IClock.hpp on why a stray delta is the
     * most dangerous number in the loop. */
    frame.deltaSeconds = clock.delta();

    frame.setStoreyGround  = input.pressed(Key::Num1);
    frame.setStoreyMiddle  = input.pressed(Key::Num2);
    frame.setStoreyTop     = input.pressed(Key::Num3);
    frame.setStoreyDynamic = input.pressed(Key::Num0);
    frame.cycleRing        = input.pressed(Key::Tab);
    frame.toggleCutaway    = input.pressed(Key::C);
    frame.toggleLos        = input.pressed(Key::L);
    frame.toggleCover      = input.pressed(Key::V);
    frame.toggleGrenade    = input.pressed(Key::G);
    frame.toggleOcclusion  = input.pressed(Key::O);
    frame.toggleBake       = input.pressed(Key::B);
    frame.toggleFlatView   = input.pressed(Key::F);
    frame.toggleDevView    = input.pressed(Key::F1);
    frame.toggleUiGallery  = input.pressed(Key::F2);
    frame.toggleCapture    = input.pressed(Key::F9);
    frame.copyCamera       = input.pressed(Key::F3);
    frame.toggleViewTarget = input.pressed(Key::F5);
    frame.cycleSplitScreen = input.pressed(Key::F7);

    /* F6, and it was F5 until the view-target toggle took that key — reload is
     * reached for in bursts while iterating on one shader; the view switch is
     * the one a demo reaches for. */
    frame.reloadShaders = input.pressed(Key::F6);
    frame.resetWorld    = input.pressed(Key::R);

    frame.orbiting = input.down(MouseButton::Middle) || input.down(Key::LeftAlt);
    frame.mouseDelta = input.pointerDelta();

    if (input.down(Key::W)) frame.panForward += 1.0f;
    if (input.down(Key::S)) frame.panForward -= 1.0f;
    if (input.down(Key::D)) frame.panRight   += 1.0f;
    if (input.down(Key::A)) frame.panRight   -= 1.0f;
    frame.panFast = input.down(Key::LeftShift);

    /* The vertical axis only. IInput reports both because a horizontal wheel
     * and a trackpad's sideways scroll are real, and nothing here wants one. */
    frame.wheel = input.wheelDelta().y;

    if (input.down(Key::LeftBracket))  frame.sunAzimuthRate   -= 1.0f;
    if (input.down(Key::RightBracket)) frame.sunAzimuthRate   += 1.0f;
    if (input.down(Key::Minus))        frame.sunElevationRate -= 1.0f;
    if (input.down(Key::Equals))       frame.sunElevationRate += 1.0f;

    frame.mousePosition = input.pointerPosition();

    /* THE SCREENSHOT PIN, applied to the reported value rather than by moving
     * the OS cursor — see the note in the header. */
    if (forcedMouseX && forcedMouseY) {
        frame.mousePosition = Vec2{ static_cast<float>(*forcedMouseX),
                                    static_cast<float>(*forcedMouseY) };
    }

    frame.leftPressed  = input.pressed(MouseButton::Left);
    frame.leftDown     = input.down(MouseButton::Left);
    frame.leftReleased = input.released(MouseButton::Left);

    /* TAKEN, NOT PEEKED — the surface clears the flag, so the resize is
     * reported to exactly one frame. Peeking would rebuild every render target
     * on every frame after a single resize. */
    frame.windowResized = surface.takeResized();

    return frame;
}

}  // namespace cromwell
