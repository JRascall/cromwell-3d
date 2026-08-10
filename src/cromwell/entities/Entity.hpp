/* Entity.hpp — anything that exists in the world.
 *
 * SINGLE RESPONSIBILITY: be a thing at a place, own components, and pass the
 * frame to the ones that asked for it.
 *
 * THIS IS THE "ACTOR" OF THIS ENGINE, named Entity because that is what a thing
 * in a world is — Unreal's AActor by another name, and an engine concept rather
 * than a game one. cromwell knows that worlds contain things, that things have
 * a position, and that behaviour is bolted on rather than inherited. What those
 * things ARE — a soldier, a crate, a door — is the game's business.
 *
 * POSITION IS WORLD-SPACE XYZ AND NOTHING ELSE. A game with a grid layers its
 * own coordinate on top (see game/units/kinds/Unit.hpp, which carries a lattice
 * Cell) and can still read the XYZ from here; a game without one uses this
 * directly. The engine must not learn what a tile is.
 *
 * THE TRANSFORM IS cromwell's OWN MATH TYPES, not raylib's. Entity lives in the
 * engine's headless half so a game's simulation — and its tests — can use
 * entities without linking a window library. See math/Vec3.hpp and
 * math/Quat.hpp; the render boundary converts for free.
 *
 * LOOKUP IS BY TYPE, as Unreal's GetComponent<T>() is. There is no reflection
 * here, so std::type_index does the work: one entry per component type, so a
 * question like "does this thing block sight" is answered by asking for the
 * component rather than by testing what class the object is. That is what keeps
 * systems free of type checks.
 *
 * THE TICK LIST IS BUILT ONCE, at attach time, from canEverTick(). Walking
 * every component of every entity each frame to call an empty virtual is a cost
 * with no feature attached; only the components that asked appear in the list.
 */
#pragma once

#include "cromwell/entities/Component.hpp"
#include "cromwell/entities/EntityProps.hpp"
#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cromwell {

class Entity {
public:
    Entity() = default;

    /* Everything injectable in one object rather than a growing list of
     * constructor arguments — see EntityProps.hpp. */
    explicit Entity(const EntityProps& props)
        : location_(props.location()),
          rotation_(props.rotation()),
          thinkInterval_(props.thinkInterval()) {}

    virtual ~Entity();

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;

    /* ---- where it is, and which way it faces --------------------------- */
    const Vec3& location() const { return location_; }
    void setLocation(Vec3 location) { location_ = location; }

    const Quat& rotation() const { return rotation_; }
    void setRotation(Quat rotation) { rotation_ = rotation; }

    /* The directions this entity is facing, derived from its rotation rather
     * than stored — two sources of truth for an orientation is how they end up
     * disagreeing. */
    Vec3 forward() const { return rotate(rotation_, Vec3::forward()); }
    Vec3 right()   const { return rotate(rotation_, Vec3::right()); }
    Vec3 up()      const { return rotate(rotation_, Vec3::up()); }

    /* ---- lifecycle ------------------------------------------------------
     * Called once when the entity has entered the world and its components are
     * attached — the moment it is safe to look at neighbours, subscribe to
     * events, or register in an index. Unreal's BeginPlay. */
    virtual void onSpawned() {}

    /* Called once when the entity is leaving the world, before anything is
     * torn down. Unreal's EndPlay. */
    virtual void onDestroyed() {}

    /* Marks this entity for removal. It does NOT free anything — whatever owns
     * the entity sweeps the flagged ones at a safe point, because destroying
     * mid-iteration is how a container invalidates the loop walking it.
     *
     * THIS IS NOT DEATH. An entity is a thing in the world, and most things in
     * a world cannot be killed: a door, a crate, a spawn marker. Whether
     * something has health, and what happens when it runs out, is a component's
     * business and a game's rules. This is only "remove it". */
    void destroy()
    {
        if (pendingDestroy_) return;
        pendingDestroy_ = true;
        onDestroyed();
    }

    bool isPendingDestroy() const { return pendingDestroy_; }

    /* ---- components ----------------------------------------------------- */

    /* Constructs a component in place, attaches it and returns it. One per
     * type: adding a second of the same type replaces the first, because
     * lookup is by type and two would make findComponent ambiguous. */
    template <class T, class... Args>
    T& addComponent(Args&&... args)
    {
        static_assert(std::is_base_of<Component, T>::value,
                      "components must derive from Component");

        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *owned;

        components_[std::type_index(typeid(T))] = std::move(owned);
        ref.onAttach(*this);
        onComponentsChanged();
        if (ref.canEverTick()) ticking_.push_back(&ref);
        if (ref.canEverThink()) {
            /* Stagger, so a squad spawned on one frame does not think in
             * lockstep forever after. Deterministic — derived from how many
             * thinkers this entity already has and from the entity's own
             * phase, never from a clock or a random source, so a replay and a
             * test see the same schedule. */
            ref.offsetThinkPhase(nextThinkPhase(ref.thinkInterval()));
            thinking_.push_back(&ref);
        }
        return ref;
    }

    /* Null when this entity carries no such component. This is the question a
     * system should be asking — "can this be selected", "does this take
     * damage" — rather than what class the object is. */
    template <class T>
    T* findComponent() const
    {
        const auto it = components_.find(std::type_index(typeid(T)));
        return it == components_.end() ? nullptr : static_cast<T*>(it->second.get());
    }

    template <class T>
    bool hasComponent() const { return findComponent<T>() != nullptr; }

    /* Detaches and destroys a component. Returns false when there was none.
     * Safe to call from outside the frame; calling it from inside a component's
     * own tick would delete the object mid-call. */
    template <class T>
    bool removeComponent()
    {
        const auto it = components_.find(std::type_index(typeid(T)));
        if (it == components_.end()) return false;

        Component* raw = it->second.get();
        raw->onDetach();
        ticking_.erase(std::remove(ticking_.begin(), ticking_.end(), raw), ticking_.end());
        thinking_.erase(std::remove(thinking_.begin(), thinking_.end(), raw), thinking_.end());
        components_.erase(it);
        onComponentsChanged();
        return true;
    }

    /* ---- the cached-pointer hook ----------------------------------------
     * Called after any component is attached or removed. Override it to
     * refresh pointers to the components this entity ALWAYS has.
     *
     * WHY THIS EXISTS. findComponent<T>() hashes a type_index and walks a
     * bucket. That is the right cost for an optional component asked about
     * once — "does this thing take damage" — and quite the wrong one for a
     * facade like Unit::footprint(), which sits inside the ray caster's
     * per-step roster scan. Measured on the demo map, resolving the five
     * components a Unit always carries through the map cost more than the ray
     * casting those queries existed to do.
     *
     * So the map stays, for the generic and optional lookups it is good at,
     * and an entity that knows it will be asked constantly caches a pointer.
     * THIS HOOK IS WHAT MAKES THAT SAFE: the cache is refreshed by the same
     * call that changes the components, so there is no way to add one and
     * forget to rebind. A cache the owner has to remember to update is a
     * dangling pointer with a delay on it. */
    virtual void onComponentsChanged() {}

    /* How many components are attached, and how many take part in each rate.
     * Diagnostics: an entity that thinks when it should not is invisible
     * otherwise. */
    std::size_t componentCount() const { return components_.size(); }
    std::size_t tickingCount()    const { return ticking_.size(); }
    std::size_t thinkingCount()   const { return thinking_.size(); }

    /* For components the caller knows are present — the ones its own factory
     * put there. Undefined if absent, exactly like dereferencing the pointer
     * above without checking, and named so the call site reads as the
     * assertion it is. */
    template <class T>
    T& component() const { return *findComponent<T>(); }

    /* ---- the frame ------------------------------------------------------
     * ONE ENTRY POINT PER FRAME. tick() drives both rates: the per-frame work
     * directly, and the accumulators that decide whether anything is due to
     * think. A caller that had to remember to call both would eventually call
     * one. */
    virtual void tick(float deltaSeconds)
    {
        for (Component* c : ticking_) c->tick(deltaSeconds);
        for (Component* c : thinking_) c->advanceThink(deltaSeconds);

        if (canEverThink()) {
            thinkTimer_ += deltaSeconds;
            if (thinkTimer_ >= thinkInterval_) {
                const float elapsed = thinkTimer_;
                thinkTimer_ = 0.0f;
                think(elapsed);
            }
        }
    }

    /* ---- the slow frame, for the entity itself --------------------------
     * The same contract as Component::think — see Component.hpp. An entity
     * overrides this when the decision belongs to the whole thing rather than
     * to one of its parts. */
    virtual bool canEverThink() const { return false; }
    virtual void think(float secondsSinceLastThink) { (void)secondsSinceLastThink; }

    float thinkInterval() const { return thinkInterval_; }
    void  setThinkInterval(float seconds) { thinkInterval_ = seconds > 0.0f ? seconds : 0.0f; }

private:
    /* Spreads first-thinks evenly across the interval rather than bunching
     * them: the Nth thinker starts N/(N+1) of the way through its own period.
     * Cheap, deterministic, and enough to break lockstep. */
    float nextThinkPhase(float interval) const
    {
        const float slot = static_cast<float>(thinking_.size() + 1);
        return interval * (slot / (slot + 1.0f));
    }

    Vec3  location_;
    Quat  rotation_;
    bool  pendingDestroy_ = false;

    /* Seeded from EntityProps, which reads the engine-wide default. */
    float thinkInterval_ = EntityProps::defaultThinkInterval();
    float thinkTimer_ = 0.0f;

    std::unordered_map<std::type_index, std::unique_ptr<Component>> components_;

    /* Raw pointers into components_, which owns them and outlives these. */
    std::vector<Component*> ticking_;
    std::vector<Component*> thinking_;
};

}  // namespace cromwell
