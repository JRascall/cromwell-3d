#include "game/ui/state/UIStateMachine.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/events/Events.hpp"

namespace game {

namespace {
/* The subscription id. Registering twice with the same id replaces rather than
 * accumulates — see Crier's rule 2 — so this is also what makes a re-created
 * machine safe. */
constexpr const char* kHookId = "UIStateMachine";
}  // namespace

UIStateMachine::UIStateMachine()
{
    cromwell::crier().onVoid(
        cromwell::events::kUIReady, kHookId, 0,
        [this](const cromwell::EventArgs&) {
            LOGGER->info("UI ready - re-pushing state: %s", toTag(state_));
            pushToUI();
        });
}

UIStateMachine::~UIStateMachine()
{
    cromwell::crier().off(cromwell::events::kUIReady, kHookId);
}

void UIStateMachine::setState(UIState next)
{
    if (next == state_) return;

    const UIState previous = state_;
    state_ = next;

    LOGGER->info("UI state: %s -> %s", toTag(previous), toTag(next));

    pushToUI();
    if (onStateChanged) onStateChanged(previous, next);
}

void UIStateMachine::pushToUI() const
{
    cromwell::crier().call(cromwell::events::kUIStateChanged,
                           { cromwell::EventValue::text(toTag(state_)) });
}

}  // namespace game
