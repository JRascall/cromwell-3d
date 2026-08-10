/* Component.hpp — the base every entity component derives from.
 *
 * SINGLE RESPONSIBILITY: define what it MEANS to be a component — an owner, a
 * lifecycle, and a place in the update cycle.
 *
 * Modelled on Unreal's UActorComponent, and for the reason that matters: a
 * component is not just a bag of fields bolted to an owner. It is a unit of
 * behaviour with a lifetime. It learns who owns it (onAttach), it may take part
 * in the frame (tick), and it is found generically by type rather than by the
 * owner declaring a member for it. Without that shared contract, "components"
 * degenerates into a handful of unrelated structs that a class happens to hold
 * — which is what the first cut of this was.
 *
 * TICK IS OPT-IN, exactly as Unreal's bCanEverTick is. Most components here
 * describe rather than act — a footprint does not need a frame — and a roster
 * that walked every component of every unit every frame to call an empty
 * virtual would be paying for a feature nobody uses. A component that wants the
 * frame says so and overrides tick(); everything else costs nothing.
 *
 * OWNERSHIP: a Unit owns its components and outlives them. `owner()` is
 * therefore a plain pointer and is never null after attachment.
 */
#pragma once

#include "cromwell/entities/EntityProps.hpp"

namespace cromwell {

class Entity;

class Component {
public:
    virtual ~Component() = default;

    /* Components are owned by exactly one entity and are not shared or copied —
     * a copied component would have an owner pointer into another entity. */
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    /* Called once, by Entity::addComponent, as the component is attached.
     * Override to do setup that needs the owner; call the base if you do. */
    virtual void onAttach(Entity& owner) { owner_ = &owner; }

    /* Called once before the owner is destroyed, so a component can undo
     * anything it registered elsewhere (an event subscription, a handle in a
     * spatial index). */
    virtual void onDetach() {}

    /* ---- the frame ------------------------------------------------------
     * Whether this component wants tick() called. False by default: see the
     * header note — a description does not need a frame. */
    virtual bool canEverTick() const { return false; }

    /* Every frame, for components that asked for it. */
    virtual void tick(float deltaSeconds) { (void)deltaSeconds; }

    /* ---- the slow frame -------------------------------------------------
     * THINK IS TICK FOR WORK THAT DOES NOT NEED SIXTY HERTZ. An AI deciding
     * where to move, a sensor re-scanning what it can see, a spawner counting
     * down: all of them are correct at ten hertz and all of them are six times
     * the cost at sixty. Splitting them off is the difference between an AI
     * that scales to a full board and one that eats the frame.
     *
     * think() IS PASSED THE REAL ELAPSED TIME, not the interval it asked for.
     * The accumulator fires on the first frame at or past the interval, so the
     * true gap is always a little more than requested and varies with frame
     * rate — anything integrating over it (a cooldown, a move budget) must use
     * what actually elapsed or it will drift. */
    virtual bool canEverThink() const { return false; }

    virtual void think(float secondsSinceLastThink) { (void)secondsSinceLastThink; }

    /* How long between thinks. Configurable per component and at runtime — an
     * idle sentry can think twice a second, the same component can drop to
     * every frame's-worth once it is in combat. */
    float thinkInterval() const { return thinkInterval_; }

    /* Clamped rather than stored raw: zero would fire every frame, which is
     * what tick() is for. */
    void setThinkInterval(float seconds) { thinkInterval_ = seconds > 0.0f ? seconds : 0.0f; }

    Entity& owner() const { return *owner_; }
    bool    isAttached() const { return owner_ != nullptr; }

private:
    /* Driven by the owning Entity and by nothing else — a component advancing
     * its own accumulator would think twice per frame, and a caller outside
     * the update cycle would desynchronise it from the clock everything else
     * runs on. Private plus a friend rather than public, so the compiler
     * enforces that instead of a comment asking nicely. */
    friend class Entity;

    void advanceThink(float deltaSeconds)
    {
        thinkTimer_ += deltaSeconds;
        if (thinkTimer_ < thinkInterval_) return;

        const float elapsed = thinkTimer_;
        thinkTimer_ = 0.0f;
        think(elapsed);
    }

    /* Spreads the first think of each component across the interval, so a
     * hundred entities created on the same frame do not all think on the same
     * frame forever after. Called by Entity at attach time. */
    void offsetThinkPhase(float seconds) { thinkTimer_ = seconds; }

    Entity* owner_ = nullptr;

    /* Seeded from the engine-wide default at construction, NOT read from it
     * per think — see EntityProps.hpp on why a live global would be worse. */
    float thinkInterval_ = EntityProps::defaultThinkInterval();
    float thinkTimer_ = 0.0f;

protected:
    Component() = default;
};

}  // namespace cromwell
