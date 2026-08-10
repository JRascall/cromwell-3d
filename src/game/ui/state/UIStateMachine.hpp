/* UIStateMachine.hpp — owns which UI mode the game is in.
 *
 * SINGLE RESPONSIBILITY: hold the current UIState, and tell everyone when it
 * changes. It decides nothing about what a state MEANS — the renderer decides
 * what to draw, Application decides when to move.
 *
 * Ported from PO's UUIStateMachine, including the part that is easy to leave
 * out and expensive to add back: it listens for events::kUIReady and re-pushes
 * the current state when it hears one. A screen created after the last
 * transition would otherwise never receive the state it was born into, and
 * that failure looks exactly like a broken screen rather than a missed event.
 *
 * TWO CHANNELS, as PO has: a direct callback for the one listener that is
 * always there and needs it every frame (the renderer), and the Crier bus for
 * anything that should not hold a pointer to this object.
 */
#pragma once

#include "cromwell/events/Crier.hpp"
#include "game/ui/state/UIState.hpp"

#include <functional>

namespace game {

class UIStateMachine {
public:
    UIStateMachine();
    ~UIStateMachine();

    UIStateMachine(const UIStateMachine&) = delete;
    UIStateMachine& operator=(const UIStateMachine&) = delete;

    UIState state() const { return state_; }

    /* No-ops when the state is unchanged, so a caller may set it every frame
     * without producing a stream of transitions. */
    void setState(UIState next);

    /* The direct channel. Called after the bus has been pushed, with (old,
     * new) — same order as PO's OnStateChanged. */
    std::function<void(UIState, UIState)> onStateChanged;

private:
    void pushToUI() const;

    UIState state_ = UIState::None;
};

}  // namespace game
