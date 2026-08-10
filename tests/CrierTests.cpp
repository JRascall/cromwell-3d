/* CrierTests.cpp — headless verification of the event bus.
 *
 *   1. publish/subscribe basics, unknown events, hookCount
 *   2. weight ordering, and registration order breaking ties
 *   3. ID deduplication and off()
 *   4. the first non-empty return wins; void hooks run after value hooks
 *   5. re-entrancy: publishing, subscribing and unsubscribing mid-fire
 *   6. pipeline folding, and non-number returns passing through
 *
 * Runs against a locally constructed Crier rather than crier(), so the tests
 * cannot leak state into each other.
 */
#include "core/events/Crier.hpp"
#include "core/events/EventValue.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace xcom;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* --------------------------------------------------------------- test 1 */
void testBasics()
{
    std::printf("== basics ==\n");
    Crier bus;

    int heard = 0;
    double payload = 0.0;
    std::string label;

    bus.onVoid("test.ping", "listener", 0, [&](const EventArgs& args) {
        heard++;
        payload = args.empty() ? 0.0 : args[0].asNumber();
        label   = args.size() < 2 ? std::string() : args[1].asText();
    });

    bus.call("test.ping", { EventValue::number(7.5), EventValue::text("hello") });
    CHECK(heard == 1, "hook fired %d times, expected 1", heard);
    CHECK(payload == 7.5, "payload was %f, expected 7.5", payload);
    CHECK(label == "hello", "label was '%s', expected 'hello'", label.c_str());

    /* an event nobody listens to is a no-op, not a crash or an insertion */
    const EventValue nothing = bus.call("test.nobody_home");
    CHECK(nothing.isNone(), "an unheard event returned a value");
    CHECK(bus.hookCount("test.nobody_home") == 0, "an unheard event grew hooks");
    CHECK(bus.hookCount("test.ping") == 1, "hookCount = %d, expected 1",
          (int)bus.hookCount("test.ping"));

    /* off() for an ID that was never registered is legal */
    bus.off("test.ping", "never_registered");
    CHECK(bus.hookCount("test.ping") == 1, "off() of an unknown ID removed a hook");
}

/* --------------------------------------------------------------- test 2 */
void testOrdering()
{
    std::printf("== weight ordering ==\n");
    Crier bus;

    std::vector<std::string> order;
    auto record = [&order](std::string id) {
        return [&order, id](const EventArgs&) { order.push_back(id); };
    };

    /* registered out of weight order, and two share a weight */
    bus.onVoid("test.order", "late",   100, record("late"));
    bus.onVoid("test.order", "early", -100, record("early"));
    bus.onVoid("test.order", "tie_a",    0, record("tie_a"));
    bus.onVoid("test.order", "tie_b",    0, record("tie_b"));

    bus.call("test.order");

    CHECK(order.size() == 4, "%d hooks fired, expected 4", (int)order.size());
    if (order.size() == 4) {
        CHECK(order[0] == "early", "first was '%s', expected 'early'", order[0].c_str());
        CHECK(order[1] == "tie_a", "second was '%s', expected 'tie_a'", order[1].c_str());
        CHECK(order[2] == "tie_b", "third was '%s', expected 'tie_b'", order[2].c_str());
        CHECK(order[3] == "late",  "fourth was '%s', expected 'late'", order[3].c_str());
        std::printf("   fired: %s, %s, %s, %s\n",
                    order[0].c_str(), order[1].c_str(), order[2].c_str(), order[3].c_str());
    }
}

/* --------------------------------------------------------------- test 3 */
void testDedupeAndOff()
{
    std::printf("== id dedupe and off ==\n");
    Crier bus;

    int first = 0, second = 0, other = 0;

    bus.onVoid("test.dupe", "same_id", 0, [&](const EventArgs&) { first++; });
    bus.onVoid("test.dupe", "same_id", 0, [&](const EventArgs&) { second++; });
    bus.onVoid("test.dupe", "other",   0, [&](const EventArgs&) { other++; });

    CHECK(bus.hookCount("test.dupe") == 2, "re-registering an ID duplicated it (%d hooks)",
          (int)bus.hookCount("test.dupe"));

    bus.call("test.dupe");
    CHECK(first == 0, "the replaced hook still fired");
    CHECK(second == 1, "the replacement hook did not fire");

    /* off() clears both lists for that ID */
    bus.on("test.dupe", "other", 0, [&](const EventArgs&) { other++; return EventValue(); });
    CHECK(bus.hookCount("test.dupe") == 3, "value and void hooks share an ID slot");
    bus.off("test.dupe", "other");
    CHECK(bus.hookCount("test.dupe") == 1, "off() left %d hooks, expected 1",
          (int)bus.hookCount("test.dupe"));

    other = 0;
    bus.call("test.dupe");
    CHECK(other == 0, "an unsubscribed hook fired %d times", other);

    bus.clear();
    CHECK(bus.hookCount("test.dupe") == 0, "clear() left hooks behind");
}

/* --------------------------------------------------------------- test 4 */
void testReturnValues()
{
    std::printf("== return values ==\n");
    Crier bus;

    std::vector<std::string> order;

    bus.on("test.answer", "silent", -10, [&](const EventArgs&) {
        order.push_back("silent");
        return EventValue();                       /* heard it, no answer */
    });
    bus.on("test.answer", "answers", 0, [&](const EventArgs&) {
        order.push_back("answers");
        return EventValue::text("first");
    });
    bus.on("test.answer", "also_answers", 10, [&](const EventArgs&) {
        order.push_back("also_answers");
        return EventValue::text("second");
    });
    bus.onVoid("test.answer", "observer", -100, [&](const EventArgs&) {
        order.push_back("observer");
    });

    const EventValue answer = bus.call("test.answer");
    CHECK(answer.asText() == "first", "answer was '%s', expected 'first'",
          answer.asText().c_str());

    /* every hook runs even after one answered... */
    CHECK(order.size() == 4, "%d hooks ran, expected 4", (int)order.size());
    /* ...and the void hook runs after the value hooks despite its lower weight */
    CHECK(!order.empty() && order.back() == "observer",
          "void hooks did not run last (last was '%s')",
          order.empty() ? "-" : order.back().c_str());
}

/* --------------------------------------------------------------- test 5 */
void testReentrancy()
{
    std::printf("== re-entrancy ==\n");
    Crier bus;

    int inner = 0, outerTail = 0, added = 0;

    bus.onVoid("test.inner", "inner", 0, [&](const EventArgs&) { inner++; });

    /* a hook that publishes from inside its own callback — the click ->
     * selected -> reach_changed chain, in miniature */
    bus.onVoid("test.outer", "publisher", 0, [&](const EventArgs&) {
        bus.call("test.inner");
    });
    bus.onVoid("test.outer", "tail", 10, [&](const EventArgs&) { outerTail++; });

    bus.call("test.outer");
    CHECK(inner == 1, "the nested event fired %d times, expected 1", inner);
    CHECK(outerTail == 1, "the outer hook after the nested publish did not run");

    /* a hook that unsubscribes itself and its neighbour mid-fire, and one that
     * subscribes a new hook mid-fire: the snapshot means this frame's list is
     * fixed, and the change lands on the next publish */
    Crier mutating;
    int selfRemoving = 0, neighbour = 0;

    mutating.onVoid("test.mutate", "self", 0, [&](const EventArgs&) {
        selfRemoving++;
        mutating.off("test.mutate", "self");
        mutating.off("test.mutate", "neighbour");
        mutating.onVoid("test.mutate", "added", 20, [&](const EventArgs&) { added++; });
    });
    mutating.onVoid("test.mutate", "neighbour", 10, [&](const EventArgs&) { neighbour++; });

    mutating.call("test.mutate");
    CHECK(selfRemoving == 1, "the self-removing hook fired %d times", selfRemoving);
    CHECK(neighbour == 1, "the snapshot did not protect a hook removed mid-fire");
    CHECK(added == 0, "a hook added mid-fire ran in the same publish");

    mutating.call("test.mutate");
    CHECK(selfRemoving == 1, "the self-removing hook fired again");
    CHECK(neighbour == 1, "the removed neighbour fired again");
    CHECK(added == 1, "the hook added mid-fire did not run on the next publish");
}

/* --------------------------------------------------------------- test 6 */
void testPipeline()
{
    std::printf("== pipeline ==\n");
    Crier bus;

    double sawAtDoubler = 0.0;
    int unitIndex = -1;

    bus.on("test.cost", "add_two", 0, [&](const EventArgs& args) {
        return EventValue::number(args[0].asNumber() + 2.0);
    });
    bus.on("test.cost", "double", 10, [&](const EventArgs& args) {
        sawAtDoubler = args[0].asNumber();
        unitIndex    = (int)(args.size() > 1 ? args[1].asNumber(-1.0) : -1.0);
        return EventValue::number(args[0].asNumber() * 2.0);
    });
    /* not a number: a pass-through, and the running value survives it */
    bus.on("test.cost", "observer", 20, [&](const EventArgs&) {
        return EventValue::text("ignored");
    });
    /* void hooks are not part of a fold at all */
    bus.onVoid("test.cost", "void_hook", 0, [](const EventArgs&) {});

    const double folded = bus.pipeline("test.cost", 1.0, { EventValue::number(3) });
    CHECK(folded == 6.0, "pipeline folded to %f, expected 6.0", folded);
    CHECK(sawAtDoubler == 3.0, "the second hook saw %f, expected 3.0", sawAtDoubler);
    CHECK(unitIndex == 3, "the extra argument arrived as %d, expected 3", unitIndex);
    std::printf("   1 -> +2 -> x2 = %g\n", folded);

    const double untouched = bus.pipeline("test.nobody_folds", 42.0);
    CHECK(untouched == 42.0, "an unheard pipeline returned %f, expected 42.0", untouched);
}

}  // namespace

int main()
{
    testBasics();
    testOrdering();
    testDedupeAndOff();
    testReturnValues();
    testReentrancy();
    testPipeline();

    if (g_failures) std::printf("\n%d FAILURE(S)\n", g_failures);
    else            std::printf("\nall crier checks passed\n");
    return g_failures ? 1 : 0;
}
