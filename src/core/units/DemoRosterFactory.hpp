/* DemoRosterFactory.hpp — the prototype's roster.
 *
 * SINGLE RESPONSIBILITY: name who starts where. A choice, not a rule — which
 * is why it is not a method on UnitRoster.
 */
#pragma once

namespace xcom {

class UnitRoster;

class DemoRosterFactory {
public:
    /* Clears `roster` and refills it with the demo squad. */
    static void build(UnitRoster& roster);
};

}  // namespace xcom
