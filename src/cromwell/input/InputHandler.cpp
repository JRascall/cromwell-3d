#include "cromwell/input/InputHandler.hpp"

namespace cromwell {

FrameInput InputHandler::sample(std::optional<int> forcedMouseX,
                                std::optional<int> forcedMouseY) const
{
    FrameInput input;

    input.deltaSeconds = GetFrameTime();
    if (input.deltaSeconds > 0.05f) input.deltaSeconds = 0.05f;

    input.setStoreyGround = IsKeyPressed(KEY_ONE);
    input.setStoreyMiddle = IsKeyPressed(KEY_TWO);
    input.setStoreyTop    = IsKeyPressed(KEY_THREE);
    input.setStoreyDynamic = IsKeyPressed(KEY_ZERO);
    input.cycleRing       = IsKeyPressed(KEY_TAB);
    input.toggleCutaway   = IsKeyPressed(KEY_C);
    input.toggleLos       = IsKeyPressed(KEY_L);
    input.toggleCover     = IsKeyPressed(KEY_V);
    input.toggleGrenade   = IsKeyPressed(KEY_G);
    input.toggleOcclusion = IsKeyPressed(KEY_O);
    input.toggleBake      = IsKeyPressed(KEY_B);
    input.toggleFlatView  = IsKeyPressed(KEY_F);
    input.toggleDevView   = IsKeyPressed(KEY_F1);
    input.toggleUiGallery = IsKeyPressed(KEY_F2);
    input.toggleCapture   = IsKeyPressed(KEY_F9);
    input.copyCamera      = IsKeyPressed(KEY_F3);
    input.reloadShaders   = IsKeyPressed(KEY_F5);
    input.resetWorld      = IsKeyPressed(KEY_R);

    input.orbiting = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) || IsKeyDown(KEY_LEFT_ALT);
    input.mouseDelta = GetMouseDelta();

    if (IsKeyDown(KEY_W)) input.panForward += 1.0f;
    if (IsKeyDown(KEY_S)) input.panForward -= 1.0f;
    if (IsKeyDown(KEY_D)) input.panRight   += 1.0f;
    if (IsKeyDown(KEY_A)) input.panRight   -= 1.0f;
    input.panFast = IsKeyDown(KEY_LEFT_SHIFT);

    input.wheel = GetMouseWheelMove();

    if (IsKeyDown(KEY_LEFT_BRACKET))  input.sunAzimuthRate   -= 1.0f;
    if (IsKeyDown(KEY_RIGHT_BRACKET)) input.sunAzimuthRate   += 1.0f;
    if (IsKeyDown(KEY_MINUS))         input.sunElevationRate -= 1.0f;
    if (IsKeyDown(KEY_EQUAL))         input.sunElevationRate += 1.0f;

    if (forcedMouseX && forcedMouseY) SetMousePosition(*forcedMouseX, *forcedMouseY);
    input.mousePosition = GetMousePosition();
    input.leftPressed   = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    input.leftReleased  = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    input.windowResized = IsWindowResized();
    return input;
}

}  // namespace cromwell
