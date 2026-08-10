/* ServiceKey.hpp — a named slot in the service container.
 *
 * SINGLE RESPONSIBILITY: be a string that the type system takes seriously.
 *
 * WHY A TYPE RATHER THAN A RAW STRING. The container is keyed by name so that
 * two Settings can coexist as "game" and "user" — but a plain `const char*`
 * parameter accepts any string that happens to be in scope, so a typo compiles
 * and fails at runtime as a service that is mysteriously absent. Wrapping it
 * means only a declared key resolves, and a misspelling is a name the compiler
 * has never heard of.
 *
 * This is TypeScript's `Symbol` in InversifyJS, or Unreal's FName for the same
 * job: an identity you must have been given rather than one you can invent at
 * the call site.
 *
 * constexpr, so keys cost nothing and live in the binary rather than being
 * built at startup. Declare them next to whatever owns the service:
 *
 *     inline constexpr ServiceKey kGameSettings{ "cromwell.settings.game" };
 */
#pragma once

#include <cstddef>
#include <string>

namespace cromwell {

class ServiceKey {
public:
    /* Explicit on purpose: `provide<T>("typo")` must not compile. The name is
     * expected to be a string literal, which lives as long as the program. */
    explicit constexpr ServiceKey(const char* name) : name_(name) {}

    constexpr const char* name() const { return name_; }

    /* Compares by CONTENT, not by pointer. Two translation units may each hold
     * their own copy of the same literal, and a container that told them apart
     * would lose services across a library boundary. */
    bool operator==(const ServiceKey& other) const
    {
        return std::string(name_) == other.name_;
    }
    bool operator!=(const ServiceKey& other) const { return !(*this == other); }

    std::string toString() const { return std::string(name_); }

private:
    const char* name_;
};

}  // namespace cromwell
