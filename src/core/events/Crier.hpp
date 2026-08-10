/* Crier.hpp — the global event mediator.
 *
 * SINGLE RESPONSIBILITY: route namespaced string events from whoever raised
 * them to whoever asked to hear them, in a defined order, without either side
 * knowing the other exists.
 *
 * Ported from PO's UCrier (Source/PO/Core/Subsystems). The rules are the same
 * three that make it safe to use everywhere:
 *
 *   1. Hooks are WEIGHT-SORTED — lower weight fires first, registration order
 *      breaks ties. Ordering is data, not a side effect of construction order.
 *   2. Hooks are ID-DEDUPLICATED — registering an ID twice for the same event
 *      replaces the first. A system that re-registers cannot accumulate
 *      duplicates, and it always has a stable handle to unregister with.
 *   3. Iteration is over a LOCAL SNAPSHOT — a hook may register, unregister or
 *      publish from inside its own callback. Event chains (click ->
 *      unit_selected -> reach_changed) are the normal case, so call() has to be
 *      re-entrant; a shared snapshot would be clobbered by the nested call
 *      while the outer one is still walking it.
 *
 * The cost of rule 3 is one vector copy per publish. That is deliberate:
 * correctness under re-entrancy is worth more here than the allocation, and
 * events are published at interaction rate, not per triangle.
 *
 * Access the process-wide bus through crier(). Construct a Crier directly when
 * you want an isolated one (tests do).
 */
#pragma once

#include "core/events/EventValue.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xcom {

/* Payloads are positional: each event documents its own argument list in
 * Events.hpp, the same contract PO's Events.h carries. */
using EventArgs = std::vector<EventValue>;

class Crier {
public:
    using HookFn = std::function<EventValue(const EventArgs&)>;
    using VoidFn = std::function<void(const EventArgs&)>;

    /* Subscribes `id` to `event`. A hook that can answer returns a value;
     * one that only observes should use onVoid instead of returning NONE. */
    Crier& on(std::string event, std::string id, int weight, HookFn callback);
    Crier& onVoid(std::string event, std::string id, int weight, VoidFn callback);

    /* Unsubscribes `id` from `event` in both lists. Safe for an ID that was
     * never registered — teardown paths should not have to remember. */
    Crier& off(const std::string& event, const std::string& id);

    /* Publishes. Every hook runs; the FIRST non-empty return wins, so a
     * high-priority hook can answer a question the later ones also heard.
     * Void hooks run after the value hooks, in their own weight order. */
    EventValue call(const std::string& event, const EventArgs& args = {});

    /* Folds a number through every value hook: each one is handed the running
     * value as argument 0 (followed by `extra`) and a hook that returns a
     * number replaces it. This is how a base figure collects modifiers — a
     * move cost, a hit chance — with no system knowing the others. */
    double pipeline(const std::string& event, double initial, const EventArgs& extra = {});

    /* Number of hooks listening to `event`, both kinds. Diagnostics only. */
    std::size_t hookCount(const std::string& event) const;

    /* Drops every subscription. For test isolation and full-state teardown —
     * NOT a substitute for systems unregistering their own IDs. */
    void clear();

private:
    struct HookEntry { std::string id; int weight; HookFn fn; };
    struct VoidEntry { std::string id; int weight; VoidFn fn; };

    std::unordered_map<std::string, std::vector<HookEntry>> hooks_;
    std::unordered_map<std::string, std::vector<VoidEntry>> voidHooks_;

    template <typename T> static void insertSorted(std::vector<T>& list, T&& entry);
    template <typename T> static void removeById(std::vector<T>& list, const std::string& id);
};

/* The process-wide bus — the replacement for PO's CRIER macro. Application
 * owns no pointer to it and neither does anything else; that is the point. */
Crier& crier();

}  // namespace xcom
