/* UIStateTests.cpp — headless verification of the UI state machine.
 *
 *   1. a transition publishes ui.state_changed carrying the new tag
 *   2. setting the state it is already in publishes nothing
 *   3. ui.ready re-pushes the current state, so a screen that loads late syncs
 *   4. the direct callback fires with (old, new), after the bus
 *
 * Point 3 is the one worth a test: a screen created after the last transition
 * never hears it, and that failure is indistinguishable from a broken screen.
 */
#include "cromwell/events/Crier.hpp"
#include "cromwell/events/Events.hpp"
#include "game/ui/state/UIStateMachine.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace game;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* Records every tag pushed over the bus, in order. */
struct TagRecorder {
    std::vector<std::string> tags;

    explicit TagRecorder(const char* id) : id_(id)
    {
        cromwell::crier().onVoid(cromwell::events::kUIStateChanged, id_, 0,
            [this](const cromwell::EventArgs& args) {
                tags.push_back(args.empty() ? std::string() : args[0].asText());
            });
    }
    ~TagRecorder() { cromwell::crier().off(cromwell::events::kUIStateChanged, id_); }

private:
    const char* id_;
};

void transitionPublishesTag()
{
    TagRecorder recorder("test.transition");
    UIStateMachine machine;

    machine.setState(UIState::MainMenu);

    CHECK(recorder.tags.size() == 1, "expected one push, got %zu", recorder.tags.size());
    CHECK(!recorder.tags.empty() && recorder.tags[0] == "mainmenu",
          "expected tag 'mainmenu', got '%s'",
          recorder.tags.empty() ? "" : recorder.tags[0].c_str());
    CHECK(machine.state() == UIState::MainMenu, "state did not move");
}

void repeatedStateIsSilent()
{
    TagRecorder recorder("test.repeat");
    UIStateMachine machine;

    machine.setState(UIState::InGame);
    machine.setState(UIState::InGame);
    machine.setState(UIState::InGame);

    CHECK(recorder.tags.size() == 1,
          "a repeated state must not re-publish; got %zu pushes", recorder.tags.size());
}

void readyRePushesCurrentState()
{
    UIStateMachine machine;
    machine.setState(UIState::Options);

    /* The screen subscribes AFTER the transition it missed, exactly as a
     * late-created UI layer would. */
    TagRecorder latecomer("test.latecomer");
    CHECK(latecomer.tags.empty(), "nothing should have arrived yet");

    cromwell::crier().call(cromwell::events::kUIReady);

    CHECK(latecomer.tags.size() == 1, "ui.ready should re-push exactly once, got %zu",
          latecomer.tags.size());
    CHECK(!latecomer.tags.empty() && latecomer.tags[0] == "options",
          "late screen should receive 'options'");
}

void callbackCarriesBothStates()
{
    UIStateMachine machine;
    machine.setState(UIState::MainMenu);

    UIState seenOld = UIState::None, seenNew = UIState::None;
    int calls = 0;
    machine.onStateChanged = [&](UIState from, UIState to) {
        seenOld = from; seenNew = to; calls++;
    };

    machine.setState(UIState::InGame);

    CHECK(calls == 1, "callback should fire once, fired %d", calls);
    CHECK(seenOld == UIState::MainMenu, "old state wrong");
    CHECK(seenNew == UIState::InGame, "new state wrong");
}

void tagsAreStable()
{
    CHECK(std::string(toTag(UIState::SplashScreen)) == "splashscreen", "splash tag");
    CHECK(std::string(toTag(UIState::MainMenu))     == "mainmenu",     "menu tag");
    CHECK(std::string(toTag(UIState::Options))      == "options",      "options tag");
    CHECK(std::string(toTag(UIState::InGame))       == "ingame",       "ingame tag");
    CHECK(std::string(toTag(UIState::None))         == "none",         "none tag");
}

}  // namespace

int main()
{
    transitionPublishesTag();
    repeatedStateIsSilent();
    readyRePushesCurrentState();
    callbackCarriesBothStates();
    tagsAreStable();

    if (g_failures == 0) std::printf("UI state tests passed\n");
    else                 std::printf("%d UI state check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
