/* EntityTests.cpp — headless verification of the entity/component plumbing.
 *
 *   1. components are found by type, and absence is a null rather than a crash
 *   2. adding the same type twice replaces rather than duplicates
 *   3. tick reaches only the components that asked for it
 *   4. think fires on its interval, NOT every frame
 *   5. think is handed the REAL elapsed time, not the interval it asked for
 *   6. the interval is configurable, and takes effect live
 *   7. thinkers are staggered, so a squad built on one frame does not think
 *      in lockstep
 *   8. onDetach runs before the owner's memory goes
 *   9. destroy() flags for removal and fires onDestroyed exactly once
 *
 * Points 4-7 are the ones worth a test: think's accumulator is invisible from
 * the outside, and every failure mode of it (firing every frame, never firing,
 * drifting, bunching) looks like "the AI feels wrong" rather than like a bug.
 */
#include "cromwell/entities/Entity.hpp"
#include "cromwell/entities/EntityProps.hpp"

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

/* Never asked for anything. */
struct Inert : Component {
    int ticks = 0;
    void tick(float) override { ticks++; }   /* must never be called */
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

void tickReachesOnlyTickers()
{
    Entity entity;
    Ticker& ticker = entity.addComponent<Ticker>();
    Inert&  inert  = entity.addComponent<Inert>();

    for (int i = 0; i < 5; i++) entity.tick(1.0f / 60.0f);

    CHECK(ticker.ticks == 5, "ticker should have ticked 5 times, got %d", ticker.ticks);
    CHECK(inert.ticks == 0, "a component that never asked must not tick, got %d", inert.ticks);
}

void thinkFiresOnIntervalNotEveryFrame()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();

    /* One second at 60fps against a 100ms interval: ten thinks, not sixty.
     *
     * The stagger does not change the COUNT, only where in the second the
     * thinks land — a phase of half an interval brings the first one forward
     * and pushes the eleventh past the end. The test is written against the
     * public surface for that reason: offsetThinkPhase is Entity's to call, so
     * a test that needed to reach it would be testing a seam nobody else has. */
    for (int i = 0; i < 60; i++) entity.tick(1.0f / 60.0f);

    CHECK(thinker.thinks == 10, "expected 10 thinks in a second at 100ms, got %d",
          thinker.thinks);
}

void thinkReceivesRealElapsedTime()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();

    /* 1/60 does not divide 0.1 evenly, so the accumulator always overshoots.
     * think() must report what actually elapsed, or anything integrating over
     * it drifts. */
    for (int i = 0; i < 60; i++) entity.tick(1.0f / 60.0f);

    CHECK(thinker.lastElapsed >= 0.1f,
          "elapsed should be at least the interval, got %f", thinker.lastElapsed);
    CHECK(thinker.lastElapsed < 0.13f,
          "elapsed should be close to the interval, got %f", thinker.lastElapsed);

    /* No time may be lost: the reported elapsed times must sum to roughly the
     * wall time that passed.
     *
     * The upper bound carries a tolerance because the wall time itself does:
     * 1/60 is not representable in binary, and sixty of them sum to 1.00000008
     * rather than 1.0. Asserting `<= 1.0f` fails by one ulp on arithmetic that
     * is entirely correct. */
    CHECK(thinker.totalElapsed > 0.95f && thinker.totalElapsed <= 1.001f,
          "elapsed times should account for the second that passed, got %f",
          thinker.totalElapsed);
}

void intervalIsConfigurable()
{
    Entity entity;
    Thinker& thinker = entity.addComponent<Thinker>();
    thinker.setThinkInterval(0.5f);   /* twice a second */

    for (int i = 0; i < 60; i++) entity.tick(1.0f / 60.0f);

    CHECK(thinker.thinks == 2, "at 500ms expected 2 thinks in a second, got %d",
          thinker.thinks);
}

void thinkersAreStaggered()
{
    Entity entity;
    Thinker& a = entity.addComponent<Thinker>();

    /* A second thinker on the same entity, same interval. If both started at
     * zero they would fire on the same frame forever. */
    struct Thinker2 : Thinker {};
    Thinker2& b = entity.addComponent<Thinker2>();

    int sameFrame = 0;
    for (int i = 0; i < 60; i++) {
        const int beforeA = a.thinks, beforeB = b.thinks;
        entity.tick(1.0f / 60.0f);
        if (a.thinks > beforeA && b.thinks > beforeB) sameFrame++;
    }

    CHECK(a.thinks > 0 && b.thinks > 0, "both thinkers should have run");
    CHECK(sameFrame < a.thinks,
          "thinkers should not fire together every time; %d of %d were shared",
          sameFrame, a.thinks);
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
    tickReachesOnlyTickers();
    thinkFiresOnIntervalNotEveryFrame();
    thinkReceivesRealElapsedTime();
    intervalIsConfigurable();
    thinkersAreStaggered();
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
