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
 * TAKING PART IS OPT-IN, exactly as Unreal's bCanEverTick is. Most components
 * here describe rather than act — a footprint does not need a frame — and a
 * roster that walked every component of every unit every frame to call an empty
 * virtual would be paying for a feature nobody uses. A component that wants a
 * clock says so; everything else costs nothing.
 *
 * THERE ARE TWO CLOCKS AND THEREFORE THREE PASSES. Which one a component wants
 * is the first question to answer about it, and the wrong answer is invisible
 * until it is expensive:
 *
 *   simulate(fixedSeconds)   fixed clock, every step    what the game IS
 *   think(elapsedSeconds)    fixed clock, every N steps decisions, on a budget
 *   tick(frameSeconds)       frame clock, once a frame  what the game LOOKS like
 *
 * simulate() runs on a step of fixed size, however many times the elapsed real
 * time is worth — so its results do not depend on the frame rate, which is what
 * makes replays, saves and shared-simulation multiplayer possible at all. See
 * FixedTimestep.hpp for why that is not the same as "a fixed amount of time per
 * frame", which is the version that made Fallout run at double speed above 60
 * fps.
 *
 * tick() is the presentation pass: animation blending, particles, camera
 * smoothing, anything whose only consumer is the picture. It runs exactly once
 * per drawn frame with real frame time, and nothing it does may matter to the
 * simulation — two machines will run it a different number of times.
 *
 * THE LAZY MISTAKE IS LEAVING SIMULATION WORK IN tick(), where it silently
 * becomes frame-rate dependent. Both flags default to false precisely so that
 * cannot happen by omission: a component acquires a clock only by asking for
 * one, and asking is a line of code somebody wrote on purpose.
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

    /* ---- the simulation step --------------------------------------------
     * Whether this component wants simulate() called. False by default: see the
     * header note — a description does not need a clock.
     *
     * ANYTHING THE GAME'S RULES DEPEND ON BELONGS HERE, not in tick(): movement,
     * health, cooldowns, physics. */
    virtual bool canEverSimulate() const { return false; }

    /* Every simulation step, for components that asked for it. The step is
     * always the same size, so integrating over it is exact and gives the same
     * answer on every machine. It may be called several times in one frame, or
     * none at all. */
    virtual void simulate(float fixedSeconds) { (void)fixedSeconds; }

    /* ---- the frame ------------------------------------------------------
     * Whether this component wants tick() called. False by default.
     *
     * THIS IS THE PRESENTATION PASS. Once per drawn frame, real frame time,
     * runs a different number of times on different machines — so nothing the
     * simulation reads may be written here. */
    virtual bool canEverTick() const { return false; }

    /* Every frame, for components that asked for it. */
    virtual void tick(float deltaSeconds) { (void)deltaSeconds; }

    /* ---- the slow step --------------------------------------------------
     * THINK IS SIMULATE FOR WORK THAT DOES NOT NEED EVERY STEP. An AI deciding
     * where to move, a sensor re-scanning what it can see, a spawner counting
     * down: all of them are correct at ten hertz and all of them are six times
     * the cost at sixty. Splitting them off is the difference between an AI
     * that scales to a full board and one that eats the frame.
     *
     * THINK RUNS ON THE SIMULATION CLOCK, not the frame clock, because deciding
     * where to move IS the game. An ambush that triggered on a different step on
     * two machines is exactly the divergence the fixed step exists to prevent.
     *
     * think() IS PASSED THE ELAPSED SIMULATED TIME, which is an exact whole
     * number of steps — the interval is resolved to a step count, so the gap is
     * identical every time and identical everywhere. Integrate over the value
     * passed rather than over thinkInterval(): the two differ whenever the
     * interval is not a whole number of steps, and the passed value is the one
     * that actually happened. (This used to be a real drift hazard, when the
     * accumulator ran on variable frame time and overshot by a different amount
     * every time. On a fixed step it cannot.) */
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
     * its own accumulator would think twice per step, and a caller outside
     * the update cycle would desynchronise it from the clock everything else
     * runs on. Private plus a friend rather than public, so the compiler
     * enforces that instead of a comment asking nicely. */
    friend class Entity;

    /* COUNTS STEPS, NOT SECONDS. A float accumulator would work — the step size
     * is constant, so it would even be deterministic — but counting integers is
     * exact, makes the phase offset exact, and removes the whole question of
     * whether an interval that nearly divides the step drifts over an hour. */
    void advanceThink(float fixedSeconds)
    {
        const int due = dueSteps(fixedSeconds);

        /* The phase is stored as a slot at attach time and resolved to a step
         * count here, on the first step, because the step size is not known
         * until the simulation actually runs. */
        if (phaseSlot_ >= 0) {
            stepsSinceThink_ = phaseOffset(due);
            phaseSlot_ = -1;
        }

        if (++stepsSinceThink_ < due) return;

        stepsSinceThink_ = 0;
        think(static_cast<float>(due) * fixedSeconds);
    }

    /* At least one: a zero would fire every step, which is what simulate() is
     * for, and a negative would never fire at all. */
    int dueSteps(float fixedSeconds) const
    {
        const int steps = static_cast<int>(thinkInterval_ / fixedSeconds + 0.5f);
        return steps > 0 ? steps : 1;
    }

    /* The Nth thinker starts N/(N+1) of the way through its own period — the
     * same spread this had when it was measured in seconds, now in steps. */
    int phaseOffset(int due) const
    {
        const float slot = static_cast<float>(phaseSlot_);
        return static_cast<int>(static_cast<float>(due) * (slot / (slot + 1.0f)));
    }

    /* Spreads the first think of each component across the interval, so a
     * hundred entities created on the same frame do not all think on the same
     * step forever after. Called by Entity at attach time, which is why it
     * takes a slot rather than a time: the step size is not known yet. */
    void offsetThinkPhase(int slot) { phaseSlot_ = slot; }

    Entity* owner_ = nullptr;

    /* Seeded from the engine-wide default at construction, NOT read from it
     * per think — see EntityProps.hpp on why a live global would be worse. */
    float thinkInterval_ = EntityProps::defaultThinkInterval();

    int stepsSinceThink_ = 0;

    /* -1 once the phase has been resolved into stepsSinceThink_. Non-negative
     * means "still waiting for the first step to learn the step size". */
    int phaseSlot_ = -1;

protected:
    Component() = default;
};

}  // namespace cromwell
