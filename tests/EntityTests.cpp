/* EntityTests.cpp — headless verification of the entity/component plumbing.
 *
 *   1. components are found by type, and absence is a null rather than a crash
 *   2. adding the same type twice replaces rather than duplicates
 *   3. each pass reaches only the components that asked for it
 *   4. the two clocks are separate: tick drives no simulation and vice versa
 *   5. think fires on its interval, NOT every step
 *   6. think is handed the elapsed SIMULATED time, exactly
 *   7. the interval is configurable, and takes effect live
 *   8. thinkers are staggered, so a squad built on one frame does not think
 *      in lockstep
 *   9. onDetach runs before the owner's memory goes
 *  10. destroy() flags for removal and fires onDestroyed exactly once
 *  11. the fixed step counts from elapsed TIME, not from frames
 *
 * Points 5-8 are the ones worth a test: think's counter is invisible from the
 * outside, and every failure mode of it (firing every step, never firing,
 * drifting, bunching) looks like "the AI feels wrong" rather than like a bug.
 *
 * Point 4 is the regression guard for the two-clock split. If think ever gets
 * driven from tick() again, the simulation silently becomes frame-rate
 * dependent and nothing else in the suite notices.
 *
 * Point 11 is the Fallout test, and it is the reason FixedTimestep exists: an
 * engine that steps once per frame with a fixed step size runs the game faster
 * on a faster machine. Feeding the same wall-clock second at two frame rates
 * must produce the same number of steps.
 */
#include "cromwell/entities/Entity.hpp"
#include "cromwell/entities/EntityProps.hpp"
#include "cromwell/entities/FixedTimestep.hpp"

#include <cstdio>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

/* Ticks every frame, counts them. */
struct Ticker : Component {
    int ticks = 0;
    bool canEverTick() const override { return true; }
    void tick(float) override { ticks++; }
};

/* Runs on the fixed step, counts them. */
struct Simulator : Component {
    int   steps = 0;
    float lastStep = 0.0f;
    bool canEverSimulate() const override { return true; }
    void simulate(float fixedSeconds) override { steps++; lastStep = fixedSeconds; }
};

/* Never asked for anything. */
struct Inert : Component {
    int ticks = 0;
    int steps = 0;
    void tick(float) override { ticks++; }          /* must never be called */
    void simulate(float) override { steps++; }      /* nor this */
};

/* Thinks on the default 100ms. */
struct Thinker : Component {
    int   thinks = 0;
    float lastElapsed = 0.0f;
    float totalElapsed = 0.0f;
    bool canEverThink() const override { return true; }
    void think(float elapsed) override
    {
        thinks++;
        lastElapsed = elapsed;
        totalElapsed += elapsed;
    }
};

struct Detachable : Component {
    bool* flag = nullptr;
    void onDetach() override { if (flag) *flag = true; }
};

void componentsAreFoundByType()
{
    Entity entity;
    Ticker& ticker = entity.addComponent<Ticker>();

    CHECK(entity.findComponent<Ticker>() == &ticker, "lookup should return what was added");
    CHECK(entity.hasComponent<Ticker>(), "hasComponent should be true");
    CHECK(entity.findComponent<Thinker>() == nullptr, "absent component should be null");
    CHECK(!entity.hasComponent<Thinker>(), "hasComponent should be false when absent");
    CHECK(ticker.isAttached(), "component should know it is attached");
    CHECK(&ticker.owner() == &entity, "component should know its owner");
}

void addingTwiceReplaces()
{
    Entity entity;
    entity.addComponent<Ticker>();
    Ticker& second = entity.addComponent<Ticker>();

    CHECK(entity.findComponent<Ticker>() == &second,
          "a second component of the same type should replace the first");
}

void eachPassReachesOnlyItsOwn()
{
    Entity entity;
    Ticker&    ticker    = entity.addComponent<Ticker>();
    Simulator& simulator = entity.addComponent<Simulator>();
    Inert&     inert     = entity.addComponent<Inert>();

    for (int i = 0; i < 5; i++) entity.tick(1.0f / 60.0f);
    for (int i = 0; i < 7; i++) entity.simulate(1.0f / 60.0f);

    CHECK(ticker.ticks == 5, "ticker should have ticked 5 times, got %d", ticker.ticks);
    CHECK(simulator.steps == 7, "simulator should have stepped 7 times, got %d",
          simulator.steps);
    CHECK(inert.ticks == 0, "a component that never asked must not tick, got %d", inert.ticks);
    CHECK(inert.steps == 0, "a component that never asked must not simulate, got %d",
          inert.steps);
}

void theTwoClocksAreSeparate()
{
    /* THE REGRESSION GUARD FOR THE SPLIT. Presentation runs a different number
     * of times on different machines, so anything the simulation depends on
     * must not be reachable from it — and think, where the decisions live, is
     * the one that was previously driven from tick. */
    Entity entity;
    Ticker&    ticker    = entity.addComponent<Ticker>();
    Simulator& simulator = entity.addComponent<Simulator>();
    Thinker&   thinker   = entity.addComponent<Thinker>();

    for (int i = 0; i < 600; i++) entity.tick(1.0f / 60.0f);

    CHECK(ticker.ticks == 600, "tick should reach tickers, got %d", ticker.ticks);
    CHECK(simulator.steps == 0, "tick must not drive simulate, got %d", simulator.steps);
    CHECK(thinker.thinks == 0, "tick must not drive think, got %d", thinker.thinks);

    entity.simulate(1.0f / 60.0f);
    CHECK(ticker.ticks == 600, "simulate must not drive tick, got %d", ticker.ticks);
}

void thinkFiresOnIntervalNotEveryStep()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();

    /* One second of steps at 60 Hz against a 100ms interval: ten thinks, not
     * sixty.
     *
     * The stagger does not change the COUNT, only where in the second the
     * thinks land — a phase of half an interval brings the first one forward
     * and pushes the eleventh past the end. The test is written against the
     * public surface for that reason: offsetThinkPhase is Entity's to call, so
     * a test that needed to reach it would be testing a seam nobody else has. */
    for (int i = 0; i < 60; i++) entity.simulate(1.0f / 60.0f);

    CHECK(thinker.thinks == 10, "expected 10 thinks in a second at 100ms, got %d",
          thinker.thinks);
}

void thinkReceivesElapsedSimulatedTime()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();

    for (int i = 0; i < 60; i++) entity.simulate(1.0f / 60.0f);

    /* 100ms is exactly six steps at 60 Hz, so the elapsed time reported is
     * exactly six steps' worth — every time, on every machine. It is worth
     * asserting tightly: the value used to be whatever the frame rate happened
     * to overshoot by, and code integrating over it (a cooldown, a move budget)
     * inherited that jitter. */
    CHECK(thinker.lastElapsed > 0.0999f && thinker.lastElapsed < 0.1001f,
          "elapsed should be six steps of 1/60, got %f", thinker.lastElapsed);

    /* No time may be lost: the reported elapsed times must sum to the second
     * that passed. The tolerance is for the representation of 1/60, not for
     * any slack in the schedule. */
    CHECK(thinker.totalElapsed > 0.999f && thinker.totalElapsed <= 1.001f,
          "elapsed times should account for the second that passed, got %f",
          thinker.totalElapsed);
}

void intervalIsConfigurable()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();
    thinker.setThinkInterval(0.5f);   /* twice a second */

    for (int i = 0; i < 60; i++) entity.simulate(1.0f / 60.0f);

    CHECK(thinker.thinks == 2, "at 500ms expected 2 thinks in a second, got %d",
          thinker.thinks);
}

void thinkersAreStaggered()
{
    Entity entity;
    Thinker& a = entity.addComponent<Thinker>();

    /* A second thinker on the same entity, same interval. If both started at
     * zero they would fire on the same step forever. */
    struct Thinker2 : Thinker {};
    Thinker2& b = entity.addComponent<Thinker2>();

    int sameStep = 0;
    for (int i = 0; i < 60; i++) {
        const int beforeA = a.thinks, beforeB = b.thinks;
        entity.simulate(1.0f / 60.0f);
        if (a.thinks > beforeA && b.thinks > beforeB) sameStep++;
    }

    CHECK(a.thinks > 0 && b.thinks > 0, "both thinkers should have run");
    CHECK(sameStep < a.thinks,
          "thinkers should not fire together every time; %d of %d were shared",
          sameStep, a.thinks);
}

void stepsComeFromElapsedTimeNotFrames()
{
    /* THE FALLOUT TEST. One wall-clock second, delivered at two very different
     * frame rates, must simulate the same amount of game. An engine that steps
     * once per frame passes at 60 and runs at more than double speed at 144. */
    const int rate = 60;

    int stepsAt60 = 0;
    FixedTimestep slow;
    slow.withRate(rate);
    for (int i = 0; i < 60; i++) stepsAt60 += slow.advance(1.0f / 60.0f);

    int stepsAt144 = 0;
    FixedTimestep fast;
    fast.withRate(rate);
    for (int i = 0; i < 144; i++) stepsAt144 += fast.advance(1.0f / 144.0f);

    CHECK(stepsAt60 == 60, "a second at 60fps should be 60 steps, got %d", stepsAt60);
    CHECK(stepsAt144 >= 59 && stepsAt144 <= 60,
          "a second at 144fps should be the same second of game, got %d steps", stepsAt144);
}

void catchUpIsCapped()
{
    /* A ten-second stall must not demand six hundred steps, each of which makes
     * the next frame later still. Past the cap the surplus is dropped and the
     * game briefly runs slow, which is the survivable failure. */
    FixedTimestep timestep;
    timestep.withRate(60).withMaxCatchUp(0.25f);

    const int steps = timestep.advance(10.0f);

    CHECK(steps <= 15, "a ten second stall should be capped to the catch-up window, got %d",
          steps);
    CHECK(steps >= 14, "the cap should still deliver the window it allows, got %d", steps);
}

void blendReportsProgressThroughTheStep()
{
    /* Presentation blends with this when the step is coarser than the frame.
     * Half a step banked and no step owed means halfway to the next one. */
    FixedTimestep timestep;
    timestep.withRate(60);

    const int steps = timestep.advance(1.0f / 120.0f);

    CHECK(steps == 0, "half a step is not a step, got %d", steps);
    CHECK(timestep.blend() > 0.45f && timestep.blend() < 0.55f,
          "blend should report about half a step, got %f", timestep.blend());
}

void rateIsRefusedRatherThanDividedByZero()
{
    FixedTimestep timestep;
    timestep.withRate(0).withRate(-30);

    CHECK(timestep.rate() == 60, "a non-positive rate must not be stored, got %d",
          timestep.rate());
}

void destroyFiresOnceAndCallsBack()
{
    struct Watched : Entity {
        int destroyed = 0;
        void onDestroyed() override { destroyed++; }
    };

    Watched entity;
    entity.destroy();
    entity.destroy();   /* idempotent: a second call must not fire again */

    CHECK(entity.destroyed == 1, "onDestroyed should fire exactly once, fired %d",
          entity.destroyed);
    CHECK(entity.isPendingDestroy(), "should be flagged after destroy");
}

void componentCanBeRemoved()
{
    Entity entity;
    entity.addComponent<Ticker>();
    CHECK(entity.tickingCount() == 1, "ticker should be in the tick list");

    CHECK(entity.removeComponent<Ticker>(), "remove should report success");
    CHECK(!entity.hasComponent<Ticker>(), "component should be gone");
    CHECK(entity.tickingCount() == 0, "and out of the tick list");
    CHECK(!entity.removeComponent<Ticker>(), "removing twice should report false");
}

void detachRunsBeforeDestruction()
{
    bool detached = false;
    {
        Entity entity;
        entity.addComponent<Detachable>().flag = &detached;
        CHECK(!detached, "onDetach must not fire while the entity is alive");
    }
    CHECK(detached, "onDetach should have fired when the entity was destroyed");
}

void propsCarryConstructionValues()
{
    const EntityProps props = EntityProps{}
        .withLocation(Vec3{ 7.0f, 0.0f, 2.0f })
        .withThinkInterval(0.25f)
        .withName("sentry");

    CHECK(props.location().x == 7.0f, "props should carry the location");
    CHECK(props.thinkInterval() == 0.25f, "props should carry the interval");
    CHECK(props.name() == "sentry", "props should carry the name");

    /* A non-positive interval is refused rather than stored: zero would make
     * think() fire every frame, which is what tick() is for. */
    const EntityProps refused = EntityProps{}.withThinkInterval(0.0f);
    CHECK(refused.thinkInterval() > 0.0f, "a zero interval must not be accepted");

    Entity entity{ props };
    CHECK(entity.location().x == 7.0f, "the entity should take the props' location");
    CHECK(entity.thinkInterval() == 0.25f, "the entity should take the props' interval");
}

void rotationDrivesDirections()
{
    Entity entity;
    CHECK(nearlyEqual(entity.forward(), Vec3::forward()),
          "an unrotated entity faces forward");

    /* A quarter turn about Y takes +Z onto +X. Derived from the rotation
     * rather than stored, so the two can never disagree. */
    entity.setRotation(Quat::fromAxisAngle(Vec3::up(), 3.14159265f * 0.5f));
    CHECK(nearlyEqual(entity.forward(), Vec3::right(), 1e-3f),
          "a 90 degree yaw should point forward along +X, got (%f %f %f)",
          entity.forward().x, entity.forward().y, entity.forward().z);
}

void transformRoundTrips()
{
    Entity entity{ EntityProps{}.withLocation(Vec3{ 1.0f, 2.0f, 3.0f }) };
    CHECK(entity.location().y == 2.0f, "location should survive construction");

    entity.setLocation(Vec3{ 4.0f, 5.0f, 6.0f });
    CHECK(entity.location().x == 4.0f && entity.location().z == 6.0f,
          "location should be settable");

    /* An entity is removed, not killed — health belongs to a component and
     * most things in a world have none. */
    CHECK(!entity.isPendingDestroy(), "a new entity is not pending destroy");
    entity.destroy();
    CHECK(entity.isPendingDestroy(), "destroy should flag it for removal");
}

}  // namespace

int main()
{
    componentsAreFoundByType();
    addingTwiceReplaces();
    eachPassReachesOnlyItsOwn();
    theTwoClocksAreSeparate();
    thinkFiresOnIntervalNotEveryStep();
    thinkReceivesElapsedSimulatedTime();
    intervalIsConfigurable();
    thinkersAreStaggered();
    stepsComeFromElapsedTimeNotFrames();
    catchUpIsCapped();
    blendReportsProgressThroughTheStep();
    rateIsRefusedRatherThanDividedByZero();
    detachRunsBeforeDestruction();
    destroyFiresOnceAndCallsBack();
    componentCanBeRemoved();
    transformRoundTrips();
    propsCarryConstructionValues();
    rotationDrivesDirections();

    if (g_failures == 0) std::printf("entity tests passed\n");
    else                 std::printf("%d entity check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
