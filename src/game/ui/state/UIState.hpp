/* UIState.hpp — the discrete modes the front-end can be in.
 *
 * SINGLE RESPONSIBILITY: name the states, and give each a stable lowercase tag.
 *
 * Ported from PO's EUIState. Widened as screens are added; each value maps to a
 * tag pushed over Crier (events::kUIStateChanged) so a screen can bind to a
 * state without holding a reference to the machine that owns it.
 */
#pragma once

namespace game {

enum class UIState {
    None,

    /* App boot: the logo, shown once before the menu. */
    SplashScreen,

    MainMenu,
    Options,

    /* The tactical board. The only state that renders a world. */
    InGame,
};

/* The tag pushed over the bus. Lowercase and stable — it is a wire format, so
 * renaming one of these breaks every listener that spelled it out. */
inline const char* toTag(UIState state)
{
    switch (state) {
        case UIState::SplashScreen: return "splashscreen";
        case UIState::MainMenu:     return "mainmenu";
        case UIState::Options:      return "options";
        case UIState::InGame:       return "ingame";
        default:                    return "none";
    }
}

}  // namespace game
