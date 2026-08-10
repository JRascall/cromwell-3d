/* EventValue.hpp — one loosely-typed value on the event bus.
 *
 * SINGLE RESPONSIBILITY: carry a number, a bool, a string or nothing, so a
 * publisher can describe what happened without the subscriber's header ever
 * appearing in the publisher's includes.
 *
 * This is the stand-in for PO's TSharedPtr<FJsonValue>: the payloads are the
 * same shape (small ordered lists of scalars), and the "empty" state is what
 * PO's null shared pointer meant — a hook that handled the event but had no
 * answer to give.
 */
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace cromwell {

class EventValue {
public:
    /* Default is NONE — the "no answer" a hook returns when it only observed. */
    EventValue() = default;

    static EventValue number(double value)   { EventValue v; v.slot_ = value; return v; }
    static EventValue boolean(bool value)    { EventValue v; v.slot_ = value; return v; }
    static EventValue text(std::string value){ EventValue v; v.slot_ = std::move(value); return v; }

    bool isNone()   const { return std::holds_alternative<std::monostate>(slot_); }
    bool isNumber() const { return std::holds_alternative<double>(slot_); }
    bool isBool()   const { return std::holds_alternative<bool>(slot_); }
    bool isText()   const { return std::holds_alternative<std::string>(slot_); }

    /* Readers never throw: a payload that is not what the subscriber expected
     * yields the fallback. A wrong-typed argument is a wiring bug, and a
     * wiring bug should not take the frame down mid-render. */
    double asNumber(double fallback = 0.0) const
    {
        const double* n = std::get_if<double>(&slot_);
        return n ? *n : fallback;
    }

    bool asBool(bool fallback = false) const
    {
        const bool* b = std::get_if<bool>(&slot_);
        return b ? *b : fallback;
    }

    const std::string& asText() const
    {
        static const std::string kEmpty;
        const std::string* s = std::get_if<std::string>(&slot_);
        return s ? *s : kEmpty;
    }

private:
    std::variant<std::monostate, double, bool, std::string> slot_;
};

}  // namespace cromwell
