/* Services.hpp — the process's service container, keyed by name.
 *
 * SINGLE RESPONSIBILITY: hold named service instances and hand them back.
 *
 * KEYED BY STRING, NOT BY TYPE, and that is the whole design. A type-keyed
 * container can hold exactly one Settings, one Logger, one of anything — which
 * breaks the moment you want game settings and user settings, or a live config
 * and the defaults to reset to. Named keys are TypeScript's symbols in
 * InversifyJS, or Unreal's named subsystems: the type says what it IS, the key
 * says which one it is.
 *
 *     Services::provide<Settings>(keys::kGameSettings);
 *     Services::provide<Settings>(keys::kUserSettings);
 *     Settings& s = Services::get<Settings>(keys::kGameSettings);
 *
 * NAME THE KEY, DO NOT SPELL IT. Keys are a TYPE (see ServiceKey.hpp), so a
 * bare string literal does not compile and a misspelling is a name the
 * compiler has never heard of — the same discipline Events.hpp applies to
 * event names, enforced rather than asked for.
 *
 * THE TYPE IS STILL CHECKED. A key resolved as the wrong type returns null
 * rather than reinterpreting the bytes, so a mismatch is a failed lookup you
 * can test for instead of undefined behaviour you cannot.
 *
 * WHAT THIS IS AND IS NOT. It is a service locator: the pragmatic half of
 * dependency injection, where a caller asks for what it needs rather than being
 * handed it. It is NOT constructor injection, and the difference matters — a
 * class that reaches in here has a dependency the compiler cannot see, so
 * anything with a real seam should still take what it needs as a parameter.
 * This is for the few things that are genuinely process-wide and would
 * otherwise be threaded through twenty constructors to reach one leaf.
 *
 * REGISTER AT STARTUP, BEFORE ANYTHING RUNS. Nothing here is thread-safe and
 * nothing re-notifies on replacement; a service swapped mid-frame leaves
 * whatever already read it holding the old answer.
 *
 * TESTS OWN THE CONTAINER. clear() and provide-over-the-top are why this is a
 * locator with a reset rather than a scattering of singletons — a test installs
 * the services it wants, runs, and clears.
 */
#pragma once

#include "cromwell/services/ServiceKey.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace cromwell {

class Services {
public:
    /* Installs a service under `key`, replacing anything already there.
     * Returns a reference to it so the caller can configure it in place. */
    template <class T, class... Args>
    static T& provide(ServiceKey key, Args&&... args)
    {
        auto owned = std::make_unique<Holder<T>>(std::forward<Args>(args)...);
        T& ref = owned->value;
        registry()[key.toString()] = std::move(owned);
        return ref;
    }

    /* Null when nothing is registered under `key`, AND null when something is
     * but it is not a T. The form to use anywhere that must survive absence —
     * the engine's own defaults do, so a game that never installs settings
     * still runs. */
    template <class T>
    static T* tryGet(ServiceKey key)
    {
        const auto it = registry().find(key.toString());
        if (it == registry().end()) return nullptr;
        if (it->second->type != std::type_index(typeid(T))) return nullptr;
        return &static_cast<Holder<T>*>(it->second.get())->value;
    }

    /* For services a caller knows are installed. Undefined if absent, and named
     * so the call site reads as the assertion it is. Prefer require() unless
     * the absence is genuinely impossible. */
    template <class T>
    static T& get(ServiceKey key) { return *tryGet<T>(key); }

    /* THROWS when the service is missing or is registered as another type.
     *
     * For the things a game cannot run without — its core config, its content
     * index — where carrying on with a fallback would mean starting up wrong
     * and failing later somewhere that says nothing about the cause. The
     * message names the key, so the failure reads as "nobody registered
     * cromwell.settings.game" rather than as an access violation. */
    template <class T>
    static T& require(ServiceKey key)
    {
        if (T* found = tryGet<T>(key)) return *found;

        const std::string name = key.toString();
        throw std::runtime_error(
            has(key) ? "service '" + name + "' is registered as a different type"
                     : "required service '" + name + "' was never registered");
    }

    /* Is anything registered under this key, whatever its type. */
    static bool has(ServiceKey key)
    {
        return registry().find(key.toString()) != registry().end();
    }

    /* Removes one service. False when there was nothing there. */
    static bool remove(ServiceKey key)
    {
        return registry().erase(key.toString()) > 0;
    }

    /* Drops every service. For test isolation and orderly shutdown. */
    static void clear() { registry().clear(); }

    static std::size_t count() { return registry().size(); }

private:
    /* Type-erased storage. The base carries the real type so a lookup can
     * check it rather than trusting the caller. */
    struct HolderBase {
        explicit HolderBase(std::type_index t) : type(t) {}
        virtual ~HolderBase() = default;
        std::type_index type;
    };

    template <class T>
    struct Holder : HolderBase {
        template <class... Args>
        explicit Holder(Args&&... args)
            : HolderBase(std::type_index(typeid(T))), value(std::forward<Args>(args)...) {}
        T value;
    };

    using Registry = std::unordered_map<std::string, std::unique_ptr<HolderBase>>;

    /* Function-local static: constructed on first use, which removes the
     * static-initialisation-order question entirely. */
    static Registry& registry()
    {
        static Registry instance;
        return instance;
    }
};

}  // namespace cromwell
