/* EntityProps.hpp — everything you can inject into an entity when it is made.
 *
 * SINGLE RESPONSIBILITY: carry an entity's construction parameters as one
 * object.
 *
 * WHY A PROPS OBJECT. An entity's constructor started as `Entity(Vec3)` and was
 * already growing — a rotation, a think interval, a name, and later a tag, an
 * owning world, a network id. Every one of those as an argument gives a
 * constructor nobody can call without counting commas, where swapping two
 * floats compiles cleanly and goes wrong at runtime. Unreal solves it with
 * FActorSpawnParameters; this is the same idea.
 *
 * FLUENT, AND ENCAPSULATED. The obvious form is a struct of public fields, but
 * that would be the one rule this project does not bend (see Vec3.hpp for the
 * exception it does). Chained setters give the same readability at the call
 * site while keeping the fields private and the defaults in one place:
 *
 *     Entity entity{ EntityProps{}
 *                        .withLocation({ 4.0f, 0.0f, 9.0f })
 *                        .withThinkInterval(0.25f) };
 *
 * DEFAULTS COME FROM SETTINGS, ONCE. defaultThinkInterval() reads the game's
 * settings bag if one was registered and falls back to the engine's own number
 * if not, so a test or a tool with no settings still constructs entities. It is
 * read at construction rather than per think: a live global reaching back into
 * every object would make an entity's rate depend on when it happened to be
 * created relative to a settings change, which is the kind of bug that only
 * appears in a long session.
 */
#pragma once

#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"

#include <string>
#include <utility>

namespace cromwell {

class EntityProps {
public:
    EntityProps() = default;

    /* ---- fluent setters ------------------------------------------------- */
    EntityProps& withLocation(Vec3 location) { location_ = location; return *this; }
    EntityProps& withRotation(Quat rotation) { rotation_ = rotation; return *this; }

    /* Ignores a non-positive interval: zero would make think() fire every
     * frame, which is what tick() is for. */
    EntityProps& withThinkInterval(float seconds)
    {
        if (seconds > 0.0f) thinkInterval_ = seconds;
        return *this;
    }

    /* Purely for logs and the dev panel — nothing keys on it. */
    EntityProps& withName(std::string name) { name_ = std::move(name); return *this; }

    /* ---- reads ----------------------------------------------------------- */
    Vec3 location() const { return location_; }
    Quat rotation() const { return rotation_; }
    float thinkInterval() const { return thinkInterval_; }
    const std::string& name() const { return name_; }

    /* The engine-wide starting interval: the game's setting if there is one,
     * the engine's fallback otherwise. */
    static float defaultThinkInterval();

private:
    Vec3        location_;
    Quat        rotation_;
    float       thinkInterval_ = defaultThinkInterval();
    std::string name_;
};

}  // namespace cromwell
