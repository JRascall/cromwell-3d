/* Crier.cpp — see Crier.hpp. */
#include "core/events/Crier.hpp"

#include <algorithm>
#include <utility>

namespace xcom {

Crier& Crier::on(std::string event, std::string id, int weight, HookFn callback)
{
    std::vector<HookEntry>& list = hooks_[std::move(event)];
    removeById(list, id);
    insertSorted(list, HookEntry{ std::move(id), weight, std::move(callback) });
    return *this;
}

Crier& Crier::onVoid(std::string event, std::string id, int weight, VoidFn callback)
{
    std::vector<VoidEntry>& list = voidHooks_[std::move(event)];
    removeById(list, id);
    insertSorted(list, VoidEntry{ std::move(id), weight, std::move(callback) });
    return *this;
}

Crier& Crier::off(const std::string& event, const std::string& id)
{
    auto valued = hooks_.find(event);
    if (valued != hooks_.end()) removeById(valued->second, id);

    auto voided = voidHooks_.find(event);
    if (voided != voidHooks_.end()) removeById(voided->second, id);

    return *this;
}

EventValue Crier::call(const std::string& event, const EventArgs& args)
{
    EventValue result;

    /* The snapshots are LOCAL, and both of them: hooks fire hooks, so this
     * function re-enters itself while an outer frame is mid-iteration. A
     * member snapshot would be overwritten under that outer loop, and a bare
     * iterator over hooks_ would dangle the moment a hook subscribed. */
    auto valued = hooks_.find(event);
    if (valued != hooks_.end()) {
        const std::vector<HookEntry> snapshot = valued->second;
        for (const HookEntry& entry : snapshot) {
            EventValue returned = entry.fn(args);
            if (!returned.isNone() && result.isNone()) result = std::move(returned);
        }
    }

    auto voided = voidHooks_.find(event);
    if (voided != voidHooks_.end()) {
        const std::vector<VoidEntry> snapshot = voided->second;
        for (const VoidEntry& entry : snapshot) entry.fn(args);
    }

    return result;
}

double Crier::pipeline(const std::string& event, double initial, const EventArgs& extra)
{
    double value = initial;

    auto valued = hooks_.find(event);
    if (valued == hooks_.end()) return value;

    /* Local snapshot for the same re-entrancy reason as call(). */
    const std::vector<HookEntry> snapshot = valued->second;

    EventArgs args;
    args.reserve(1 + extra.size());
    args.push_back(EventValue::number(value));
    args.insert(args.end(), extra.begin(), extra.end());

    for (const HookEntry& entry : snapshot) {
        const EventValue returned = entry.fn(args);
        /* A hook that returns nothing (or a string) is a pass-through — only
         * a number is a new running value. */
        if (returned.isNumber()) {
            value   = returned.asNumber();
            args[0] = EventValue::number(value);
        }
    }

    return value;
}

std::size_t Crier::hookCount(const std::string& event) const
{
    std::size_t count = 0;

    auto valued = hooks_.find(event);
    if (valued != hooks_.end()) count += valued->second.size();

    auto voided = voidHooks_.find(event);
    if (voided != voidHooks_.end()) count += voided->second.size();

    return count;
}

void Crier::clear()
{
    hooks_.clear();
    voidHooks_.clear();
}

/* Insert BEFORE the first strictly heavier entry, which keeps equal weights in
 * registration order — two hooks that don't care about each other stay in the
 * order the systems booted, rather than shuffling on a rebuild. */
template <typename T>
void Crier::insertSorted(std::vector<T>& list, T&& entry)
{
    auto at = list.begin();
    while (at != list.end() && at->weight <= entry.weight) ++at;
    list.insert(at, std::move(entry));
}

template <typename T>
void Crier::removeById(std::vector<T>& list, const std::string& id)
{
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&id](const T& entry) { return entry.id == id; }),
               list.end());
}

Crier& crier()
{
    static Crier bus;
    return bus;
}

}  // namespace xcom
