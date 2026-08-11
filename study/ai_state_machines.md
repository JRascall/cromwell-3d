# AI state machines and branching — Valve and Epic

How four shipped decision architectures decide what an agent does next, and — the
part that actually differs between them — **who is allowed to interrupt whom, and
where that ordering is written down.**

Read alongside [`spatial_queries.md`](spatial_queries.md) (how a decision *scores*
candidate positions), [`gears_tactics.md`](gears_tactics.md) (how selfish agents
read as coordinated), [`navigation.md`](navigation.md) (how the chosen goal becomes
movement) and [`lod_systems.md`](lod_systems.md) §7 (how many agents get to think
at all). Those cover the queries a decision makes. **This note is about the
control flow around them.**

## Source tags

| Tag | Meaning |
|---|---|
| `[VALVE-SDK]` | Read from Source SDK 2013 on this machine — Valve's own shipping C++. Non-commercial licence: **read it, don't copy it.** |
| `[VALVE-PUB]` | Valve published it — Michael Booth's AIIDE-09 deck, the Dota bot scripting documentation. |
| `[EPIC-SRC]` | Read from `UE_5.7/Engine/` on this machine — Epic ship engine source with the binary install. EULA-bound: **read it, don't copy it.** |
| `[COMMUNITY]` | Third-party observation, no primary source. |
| `[inferred]` | My reasoning, marked as such. |

---

## 1. The one axis that separates all four

Every one of these systems does the same three things: it holds a **current
intention**, it has a rule for **choosing a new one**, and it has a rule for
**abandoning the current one early**. The third is the whole game. Choosing is
easy; the systems differ almost entirely in how they answer *"something just
happened — does it outrank what I'm doing?"*

Where each writes that ordering down:

| System | Current intention | Priority lives in | Interrupt test |
|---|---|---|---|
| **Source 1 `CAI_BaseNPC`** | a *schedule* — a fixed list of tasks | a hand-written `if`-ladder in `SelectSchedule()` | a **256-bit mask** on the schedule, ANDed against this frame's conditions |
| **NextBot `Behavior`/`Action`** | a *stack* of Actions | the return value of whichever Action is running | events walk down the stack; the first Action with an opinion wins |
| **Unreal Behavior Tree** | the leftmost runnable leaf | **the tree's own left-to-right layout** — implicit | decorators re-run their branch when an observed blackboard key changes |
| **Unreal StateTree** | a *path* of active states | an explicit `Low..Critical` enum on each transition | transitions collected leaf→root, sorted by that enum |

Read the "priority lives in" column again. Source 1 puts it in code, NextBot puts
it in the call stack, Unreal's BT puts it in **the geometry of the graph**, and
StateTree — the newest of the four, and Epic's own second attempt — puts it in a
field. **That progression is the argument of this note.**

---

## 2. Source 1: conditions, schedules, tasks `[VALVE-SDK]`

`src/game/server/ai_basenpc*.cpp`, `ai_schedule.h`, `ai_task.h`, `ai_condition.h`,
`ai_default.cpp`. This is the Half-Life 2 NPC brain, and it is a **finite state
machine with a data-driven expansion**.

### 2.1 Three vocabularies, and their real sizes

Counted from the SDK on disk:

| | Shared (base class) | Defined by leaf NPCs |
|---|---|---|
| **Conditions** (`COND_*`) | **71** | per-class, e.g. Combine adds 7 |
| **Schedules** (`SCHED_*`) | **87** enum entries, **86** defined | **429** across **47** custom-NPC blocks |
| **Tasks** (`TASK_*`) | **149** | per-class, e.g. Combine adds 9 |

The Combine soldier alone defines **33** schedules. That number is the honest
price of this architecture and it is worth staring at: a single enemy type in a
2004 shooter needed thirty-three hand-authored task lists. Compare
[`source2_animation.md`](source2_animation.md)'s 399 hand-written animation
transitions — **Valve's answer to complexity, in both the AI and the animation
layer, was to author more of it.**

### 2.2 A schedule is data, and it is text

```c
AI_DEFINE_SCHEDULE
(
    SCHED_IDLE_STAND,

    "   Tasks"
    "       TASK_STOP_MOVING        1"
    "       TASK_SET_ACTIVITY       ACTIVITY:ACT_IDLE"
    "       TASK_WAIT               5"
    "       TASK_WAIT_PVS           0"
    ""
    "   Interrupts"
    "       COND_NEW_ENEMY"
    "       COND_SEE_FEAR"
    "       COND_LIGHT_DAMAGE"
    "       COND_HEAVY_DAMAGE"
    "       COND_SMELL"
    "       COND_PROVOKED"
    "       COND_GIVE_WAY"
    "       COND_HEAR_PLAYER"
    "       COND_HEAR_DANGER"
    "       COND_HEAR_COMBAT"
    "       COND_HEAR_BULLET_IMPACT"
    "       COND_IDLE_INTERRUPT"
);
```

The macro pastes a **string** into the binary, parsed at load by
`CAI_SchedulesManager::LoadSchedulesFromBuffer`. Schedules can equally be loaded
from a file. So the task list and the interrupt list are *content*, not code —
which is the reason 429 of them are tolerable at all.

Two halves, and they are not symmetric:

- **Tasks** are a flat sequence with one float argument. No branching, no
  nesting, no conditionals. A schedule that needs to make a decision *ends* and
  lets `SelectSchedule()` pick the next one.
- **Interrupts** name the conditions that may kill the schedule mid-flight.

**A Source schedule is a straight line with a list of ways to die.** All the
branching lives outside it.

### 2.3 The branch is an `if`-ladder, and Valve never pretended otherwise

`SelectSchedule()` switches on `m_NPCState` (`IDLE` / `ALERT` / `COMBAT` /
`SCRIPT` / `DEAD` / `PRONE`) and dispatches to `SelectIdleSchedule()`,
`SelectCombatSchedule()` and friends. Each is a top-to-bottom ladder of
`HasCondition()` tests:

```c
if ( HasCondition( COND_LOW_PRIMARY_AMMO ) || HasCondition( COND_NO_PRIMARY_AMMO ) )
    return SCHED_HIDE_AND_RELOAD;

if ( !HasCondition(COND_SEE_ENEMY) )
{
    if ( !HasCondition(COND_ENEMY_OCCLUDED) )
        return SCHED_COMBAT_FACE;           // unseen but not occluded → turn
    ...
}

if ( HasCondition(COND_TOO_CLOSE_TO_ATTACK) )
    return SCHED_BACK_AWAY_FROM_ENEMY;
```

There is no scoring, no utility, no planner. **Priority is source-line order.**
This is the thing every later system replaces, and it is worth noticing that it
is also the thing that made HL2's NPCs debuggable: you can read the ladder and
know exactly what a soldier will do.

`m_NPCState` is a genuine outer FSM — six states — and it is selected separately
by `SelectIdealState()`. `ShouldSelectIdealState()` carries the best comment in
the file:

> **"NOTE: This logic was a source of pure, distilled trouble in Half-Life. If you change this function, please supply good comments."**

…followed by the Half-Life 1 version of the function preserved in a comment block
so you can see what it used to be. That is a nine-year-old scar tissue marker,
and it is about exactly the thing this note is about: **when to re-decide.**

### 2.4 The interrupt mask is the design, and it costs one AND

This is the part worth taking. Every frame, per NPC:

```c
// Start out with the base schedule's set interrupt conditions
GetCurSchedule()->GetInterruptMask( &m_CustomInterruptConditions );

if ( m_NPCState != NPC_STATE_SCRIPT && !IsInLockedScene()
     && !m_CustomInterruptConditions.IsBitSet( COND_NO_CUSTOM_INTERRUPTS ) )
{
    BuildScheduleTestBits();        // leaf class may add/remove bits
}

SetCustomInterruptCondition( COND_NPC_FREEZE );   // always forced on

// This is like: m_CustomInterruptConditions &= m_Conditions;
CAI_ScheduleBits testBits;
m_CustomInterruptConditions.And( m_Conditions, &testBits );

if (!testBits.IsAllClear())
    return false;                   // schedule is no longer valid
```

`CAI_ScheduleBits` is `CBitVec<MAX_CONDITIONS>` with `MAX_CONDITIONS = 32*8`, so
**the entire "should I reconsider?" test is a 256-bit AND against a 256-bit
mask — eight `uint32` operations.** Everything expensive (line of sight, sound,
squad state, ammo) already happened once in `GatherConditions()` and got reduced
to bits. The re-decide check reads none of it.

That is CLAUDE.md's *"do less work"* rule at the architectural level, and the
shape is exactly this codebase's `OcclusionGrid`: **an expensive authoritative
state summarised into a bit-per-fact structure that the hot path can test with
arithmetic.** The summary is rebuilt once a frame at a boundary that owns it.

Note also the header comment on `MAX_CONDITIONS`:

> `// NOTE: Changing this constant will break save files!!! (changes type of CAI_ScheduleBits)`

A fixed-width bitset is a *serialisation format*. Worth knowing before choosing one.

### 2.5 `BuildScheduleTestBits` — per-NPC interruptibility, and the escape hatch

The base implementation is empty; leaves override it. The Combine's:

```c
void CNPC_Combine::BuildScheduleTestBits( void )
{
    BaseClass::BuildScheduleTestBits();

    if (gpGlobals->curtime < m_flNextAttack)
    {
        ClearCustomInterruptCondition( COND_CAN_RANGE_ATTACK1 );
        ClearCustomInterruptCondition( COND_CAN_RANGE_ATTACK2 );
    }

    SetCustomInterruptCondition( COND_COMBINE_HIT_BY_BUGBAIT );

    if ( !IsCurSchedule( SCHED_COMBINE_BURNING_STAND ) )
        SetCustomInterruptCondition( COND_COMBINE_ON_FIRE );
}
```

Three things in eleven lines:

1. **The shot regulator suppresses an interrupt rather than gating the attack.**
   "I can attack" stays true; it just stops being a *reason to stop what I'm
   doing*. Separating "is this fact true" from "does this fact outrank my current
   plan" is the correct split and most hand-rolled AI conflates them.
2. Custom conditions get folded into the same mask — no second mechanism.
3. `!IsCurSchedule(SCHED_COMBINE_BURNING_STAND)` — **don't let being on fire
   interrupt the being-on-fire schedule.** Self-interruption is the classic
   infinite loop in this architecture and here it is being hand-patched, one
   schedule at a time. StateTree's `Self` abort mode (§6.3) is the generalisation
   of this patch.

And `COND_NO_CUSTOM_INTERRUPTS` is a condition whose only job is to say *"do not
call BuildScheduleTestBits for this schedule"* — a bit in the data that disables
a code path, for "schedules that must strictly control their interruptibility."

### 2.6 `TranslateSchedule` — the subclass override point

`GetScheduleOfType()` runs every schedule ID through the virtual
`TranslateSchedule()` before resolving it, so a subclass can substitute its own
version of any base schedule *without the selection ladder knowing*. The Combine
turns `SCHED_TAKE_COVER_FROM_ENEMY` into one of four things depending on squad
membership, difficulty, grenade slot availability and whether it should charge
the player — and returns `SCHED_COMBINE_TOSS_GRENADE_COVER1` while **speaking a
line** on the way past:

```c
if ( g_pGameRules->IsSkillLevel( SKILL_HARD ) &&
     HasCondition(COND_CAN_RANGE_ATTACK2) &&
     OccupyStrategySlot( SQUAD_SLOT_GRENADE1 ) )
{
    m_Sentences.Speak( "COMBINE_THROW_GRENADE" );
    return SCHED_COMBINE_TOSS_GRENADE_COVER1;
}
```

Translation calls translation (`return TranslateSchedule( SCHED_FAIL )`), which
is where the *real* branching depth of this system lives — not in the selection
ladder, but in the translation layer under it. Schedule IDs are namespaced:
each class has a local ID space mapped onto the global one
(`AI_IdIsLocal`, `ScheduleLocalToGlobal`), so a subclass can define
`SCHED_COMBINE_ASSAULT` without colliding.

`OccupyStrategySlot` is the squad coordination mechanism and it is nearly free:
a squad owns N slots of each kind, taking one is a claim, and it is checked
*inside* the branch that wants it. Two soldiers cannot both be the one throwing
the grenade because the second one's `if` fails. Compare `gears_tactics.md`'s
WorldState — same idea, more machinery.

### 2.7 Failure is a first-class path

Most hand-rolled AI treats "the plan didn't work" as a bug. Source makes it a
control-flow edge:

- `COND_TASK_FAILED` is a condition like any other.
- `SelectFailSchedule( failedSchedule, failedTask, taskFailCode )` is virtual and
  gets told *which* schedule and *which* task failed.
- `TASK_SET_FAIL_SCHEDULE` lets a schedule name its own fallback *as a task*.
- In `MaintainSchedule`, a failure is deliberately routed away from normal
  selection: **"Get a fail schedule if the previous schedule failed during
  execution and the NPC is still in its ideal state. Otherwise, the NPC would
  immediately select the same schedule again and fail again."**

And the base `SCHED_FAIL` is itself defensive — its comment reads *"This schedule
itself can fail because the NPC may be unable to finish the stop moving"*, so its
first task is `TASK_SET_FAIL_SCHEDULE SCHEDULE:SCHED_FAIL_NOSTOP`, a copy of
itself with the `TASK_STOP_MOVING` removed. **The failure handler has a failure
handler.** That is not paranoia; it is what a shipped version of this looks like.

### 2.8 `MaintainSchedule` is time-budgeted, and it is per-NPC

```c
const int timeLimit = ( IsDebug() ) ? 16 : 8;     // milliseconds
int taskTime = Plat_MSTime();
...
for ( i = 0; i < MAX_TASKS_RUN && !bStopProcessing; i++ )   // MAX_TASKS_RUN == 10
```

with `// UNDONE: Tune/fix this MAX_TASKS_RUN... This is just here so infinite loops are impossible`.

Several tasks may complete in one think — `TASK_SET_ACTIVITY` finishes instantly
— so the loop drains them until either ten have run or the clock says stop.
`ShouldStopProcessingTasks` also bails on a memory flag:

```c
// We ran a costly task, don't do it again!
if ( pNPC->HasMemory( bits_MEMORY_TASK_EXPENSIVE ) && bInScript == false )
    return true;
```

**Tasks self-report as expensive and the scheduler rations them.** A pathfinding
task sets the bit; the NPC will not run a second one this frame. That is a
one-line admission-control system, and it is the thing a naive port of this
architecture always leaves out and then rediscovers as a frame spike.

Inside, `RunTask` gets an eight-iteration loop guarded by `GetTaskInterrupt()`
— a task can yield mid-work and be resumed within the same think — with
`AssertMsg( j < 8, "Runaway task interrupt\n" )`.

### 2.9 Behaviors: composition without inheritance — and the trap

`CAI_Behavior` exists because HL2 needed *"a way for various NPCs to share
behaviors without sharing an inheritance relationship, and without cramming those
behaviors into the base NPC class"*. A behavior is a whole second scheduling
brain that can take over. The Combine registers six:

```c
AddBehavior( &m_RappelBehavior );
AddBehavior( &m_ActBusyBehavior );
AddBehavior( &m_AssaultBehavior );
AddBehavior( &m_StandoffBehavior );
AddBehavior( &m_FollowBehavior );
AddBehavior( &m_FuncTankBehavior );
```

And arbitration is:

```c
for ( int i = 0; i < m_Behaviors.Count(); i++ )
{
    if ( m_Behaviors[i]->CanSelectSchedule() && ShouldBehaviorSelectSchedule( m_Behaviors[i] ) )
    {
        DeferSchedulingToBehavior( m_Behaviors[i] );
        return true;
    }
}
```

**First one that says yes wins. The priority order is the `AddBehavior` call
order, and nothing in the type system says so.** Rappel outranks assault because
it was added first, on line 361.

This is the same failure mode [`source2_animation.md`](source2_animation.md) §7
found in the AnimGraph — *transition order is creation order*, so the most
specific transition, created last, never fires. **Valve shipped the same
implicit-ordering hazard in two different subsystems, a decade apart.** It is not
a Valve problem; it is what happens whenever "priority" is a property of a
container rather than a value on the thing. §6.3 is Epic eventually fixing it.

The bridging is worth a glance for the cost it reveals: `CAI_BehaviorBase` exposes
**52 distinct `BridgeXxx` methods** — `BridgeSelectSchedule`, `BridgeTranslateSchedule`,
`BridgeIsValidEnemy`, `BridgeGetFlinchActivity`, `BridgeOnCalcBaseMove`… Every
overridable decision on the NPC needs a matching bridge on the behavior. **The
extension surface of a delegating layer is the entire virtual interface of what
it delegates for**, and that bill comes due whether you plan for it or not.

### 2.10 What this architecture is good and bad at

**Good:** trivially debuggable (every decision is one named schedule and one named
break condition, and the code logs both); interruption is essentially free; the
data half is authorable; failure has a real path; long behaviours are natural
because a schedule *is* a sequence.

**Bad:** all branching is imperative and untyped — priority is line order in six
different ladders plus a translation layer plus a behavior array; no concurrency
(one schedule at a time, so "reload while retreating" is a *third schedule*); the
schedule count explodes combinatorially, which is exactly what the 429 count is
telling you.

---

## 3. NextBot: `Behavior` and `Action` `[VALVE-SDK]` `[VALVE-PUB]`

`src/game/server/NextBot/NextBotBehavior.h`, header dated **"Author: Michael Booth,
April 2006"**. This is the Left 4 Dead / TF2 bot brain, and it is Valve's
*second* answer — a hierarchical state machine with a stack, built after living
with §2 for five years.

Booth's AIIDE-09 deck describes it as **"HFSM+Stack"** and the shipped header
agrees.

### 3.1 Transitions are return values, and that is the entire safety argument

```c
enum ActionResultType
{
    CONTINUE,       // continue executing this action next frame - nothing has changed
    CHANGE_TO,      // change actions next frame
    SUSPEND_FOR,    // put the current action on hold for the new action
    DONE,           // this action has finished, resume suspended action
    SUSTAIN,        // for use with event handlers - a way to say "It's important to keep doing what I'm doing"
};
```

From the header's opening notes:

> **"By using return results to cause transitions, we ensure the atomic-ness of these transitions. For instance, it is not possible to change to a new Action and continue execution of code in the current Action."**

and Booth's slide puts the consequence plainly:

> **"The only way to go from Action A to Action B is for Action A to return a transition to Action B."**

Compare Source 1, where `SetSchedule()` is a method anyone can call at any point.
The difference is not stylistic. A returned transition **cannot** leave code
running in a state that has conceptually already exited, and cannot delete the
object it is executing inside. Every hand-rolled FSM eventually hits both bugs.

Second, quieter benefit, also from the header:

> **"Creation and deletion of Actions during transitions allows passing of type-safe arguments between Actions via constructors."**

`SuspendFor( new CTFBotUseTeleporter( nearbyTeleporter ), "Using nearby teleporter" )`.
The parameter is a constructor argument, checked by the compiler. Source 1's
equivalent is stashing an `EHANDLE` on the NPC and hoping the next schedule reads
the right one. **Blackboards (§5) reintroduce exactly the problem this solves.**

Every transition also carries a **reason string**, printed by the debug overlay:
`"I died!"`, `"Get the losers!"`, `"Run away from threat!"`,
`"Waiting for squadmates to get back into formation"`. The causal log is a
side-effect of the type, not a thing anyone has to remember to write.

### 3.2 Two relations, not one: tree *and* stack

```c
Action< Actor > *m_parent;          // the Action that contains us
Action< Actor > *m_child;           // the ACTIVE Action we contain, top of the stack
Action< Actor > *m_buriedUnderMe;   // the Action just "under" us in the stack, we resume to it when we finish
Action< Actor > *m_coveringMe;      // the Action just "above" us, it resumes to us when it finishes
```

`parent/child` is **containment** — "I am fighting, and within fighting I am
reloading". `buried/covering` is **suspension** — "I was reloading, then a grenade
landed, so retreating is on top of me". These are orthogonal, and conflating them
is why hand-rolled hierarchical FSMs get confusing. `Update` recurses child-first:

```c
// update our child action first, since it has the most specific behavior
if ( m_child )
    m_child = m_child->ApplyResult( me, behavior, m_child->InvokeUpdate( me, behavior, interval ) );
```

### 3.3 Events return *desires*, and the highest priority is applied later

```c
enum EventResultPriorityType
{
    RESULT_NONE,        // no result
    RESULT_TRY,         // use this result, or toss it out, either is ok
    RESULT_IMPORTANT,   // try extra-hard to use this result
    RESULT_CRITICAL     // this result must be used - emit an error if it can't be
};
```

The rationale is quoted verbatim because it is the subtlest thing in the file:

> **"It is not possible to have event handlers instantaneously change state upon return due to out-of-order and recurrence issues, not to mention deleting the state out from under itself. Therefore, events return DESIRED results, and the highest priority result is executed at the next Update()."**

So an event handler returns `TryChangeTo( new CTFBotDead, RESULT_CRITICAL, "I died!" )`
and the change happens at the top of the next `Update`. `StorePendingEventResult`
does the arbitration, and its comment records a **deliberate reversal of the
original policy**:

> *"We keep the most recently processed event because this allows code to check history/state to do custom event collision handling. If we keep the first event at this priority and discard subsequent events (original behavior) there is no way to predict future collision resolutions (MSB)."*

Ties go to the **latest**, not the first, so a handler can reason about what it is
overriding. And two `RESULT_CRITICAL` results colliding logs a developer warning
rather than silently dropping one — the system does not pretend the ordering is
total.

### 3.4 Events fall through the stack

```
Events are propagated to each Action in the hierarchy. If an action is suspended
for another action, it STILL RECEIVES EVENTS that are not handled by the events
"above it" in the suspend stack. In other words, the active Action gets the first
response, and if it returns CONTINUE, the Action buried beneath it can process it,
and so on deeper into the stack of suspended Actions.
```

The macro that implements it is a plain `while` down `GetActionBuriedUnderMe()`,
breaking at the first non-`CONTINUE`. **`Continue()` is "no opinion, ask someone
else"** — an event handler is a *chain of responsibility*, not a switch. That is
why a suspended `Retreat` still notices it was killed even though `Taunt` is on
top of it.

And the subtle correctness bit that falls out of allowing buried actions to
speak — a covering Action checks whether the ground has moved under it:

```c
/**
 * If any Action buried underneath me has either exited
 * or is changing to a different Action, we're "out of scope"
 */
bool IsOutOfScope( void ) const
```

checked at the top of `InvokeUpdate`, returning `Done( "Out of scope" )`. Without
it, an Action can keep running inside a context that no longer exists. Anyone
building a suspend/resume stack will hit this and probably not name it as
cleanly.

### 3.5 Contextual queries — the idea most worth stealing

Behaviour answers *questions*, not just "what am I doing":

```c
/**
 * Since behaviors can have several concurrent actions active, we ask
 * the topmost child action first, and if it defers, its parent, and so
 * on, until we get a definitive answer.
 */
enum QueryResultType { ANSWER_NO, ANSWER_YES, ANSWER_UNDEFINED };
```

with `ShouldPickUp`, `ShouldHurry`, `ShouldRetreat`, `ShouldAttack`, `IsHindrance`,
`SelectTargetPoint`, `SelectMoreDangerousThreat`, `IsPositionAllowed`. Every base
implementation returns `ANSWER_UNDEFINED`.

This is the same fall-through as events, in the read direction. The rest of the
codebase — locomotion, vision, item pickup, movement collision — asks the
behaviour stack a question and gets an answer *from whichever Action currently has
an opinion*, without knowing which one that is. Booth's stated purpose is
**"Allows concurrent Behaviors to coordinate."**

The three-valued answer is the whole trick. Two-valued forces every Action to
have a policy about everything; `UNDEFINED` lets an Action be silent on the
questions it does not care about, which is nearly all of them. **A boolean query
with a default is a decision made by whoever wrote the default. A three-valued
query with a fall-through is a decision made by whoever is closest to the
context.**

### 3.6 The monitor idiom, and where the "branching" really is

TF2's bot root chain, read from `InitialContainedAction`:

```
CTFBotMainAction              (survival: died? taunted at? damaged?)
  └─ CTFBotTacticalMonitor    (local opportunism: health, ammo, cover, teleporters, sentries)
       └─ CTFBotScenarioMonitor (objective: squad? leader? class role?)
            └─ <class/scenario action>
```

Each outer level runs *concurrently with* everything under it and interrupts by
`SuspendFor`. The tactical monitor's Update is a ladder of them:

```c
return SuspendFor( new CTFBotRetreatToCover,   "Run away from threat!" );
return SuspendFor( new CTFBotGetHealth,        "Grabbing nearby health" );
return SuspendFor( new CTFBotGetAmmo,          "Grabbing nearby ammo" );
return SuspendFor( new CTFBotDestroyEnemySentry, "Going after an enemy sentry to destroy it" );
return SuspendFor( new CTFBotUseTeleporter( nearbyTeleporter ), "Using nearby teleporter" );
```

**So NextBot's branching is still an `if`-ladder — the improvement is not that the
ladder went away, it is that the ladder is now stratified by concern and the
levels compose rather than interleave.** Grabbing health does not need to know
what scenario action it interrupted, and the scenario action does not need to
know it was interrupted; when the health is grabbed, `Done()` resumes it exactly
where it was. In §2 the same feature would need a schedule per (interruption ×
activity) pair.

Booth's deck adds a detail the SDK does not show: L4D's SurvivorBots ran **two
concurrent `Behavior` objects** — `Main` ("primary decision making, attention,
target selection and attack") and `Legs` ("slaved to Main, responsible for staying
near team unless otherwise directed"). Two stacks, one body, coordinated through
the query interface. That is the same decomposition as an animation upper/lower
body split, applied to intent.

### 3.7 The mechanism-level lesson

The Action framework has essentially no *content* — no conditions, no schedules,
no task vocabulary. It is ~1,900 lines of transition machinery and event routing,
and every game-specific decision lives in a subclass. That is the opposite
distribution to §2, where the framework is thin and the content is 429 schedules.
**Neither is wrong; they are the two ends of the same trade, and NextBot's end is
the one that survives being handed to a different game.** Which, per §4, is
literally what happened.

---

## 4. Source 2: what is knowable, and what is not

### 4.1 The Action framework crossed the engine boundary `[VALVE-SDK]`

The strongest available evidence is sitting in the Source SDK 2013 header. Inside
`Action<Actor>`, guarded by a preprocessor block:

```c
#ifdef DOTA_SERVER_DLL
    virtual EventDesiredResult< Actor > OnCastAbilityNoTarget( Actor *me, CDOTABaseAbility *ability )   { return TryContinue(); }
    virtual EventDesiredResult< Actor > OnCastAbilityOnPosition( Actor *me, CDOTABaseAbility *ability, const Vector &pos ) { return TryContinue(); }
    virtual EventDesiredResult< Actor > OnCastAbilityOnTarget( Actor *me, CDOTABaseAbility *ability, CBaseEntity *target ) { return TryContinue(); }
    virtual EventDesiredResult< Actor > OnDropItem( Actor *me, const Vector &pos, CBaseEntity *item )   { return TryContinue(); }
    virtual EventDesiredResult< Actor > OnPickupRune( Actor *me, CBaseEntity *item )                    { return TryContinue(); }
    virtual EventDesiredResult< Actor > OnDominated( Actor *me )                                        { return TryContinue(); }
    ...
#endif
```

**Valve's publicly released Source 1 SDK ships a header carrying Dota 2's bot
event vocabulary.** Dota 2 moved to Source 2 in 2015. The same `Action` base
class, the same `EventDesiredResult`, the same `TryContinue()`, extended with
`OnCastAbilityOnTarget` and `OnPickupRune`. This is the AI equivalent of
[`rigging_ik.md`](rigging_ik.md) finding Ken Perlin's solver surviving the engine
rewrite as a named option: **the Behavior/Action architecture is engine
furniture at Valve, not a Left 4 Dead artefact.**

Second, weaker thread `[COMMUNITY]`: the Half-Life: Alyx Workshop Tools expose
`aiscripted_schedule` — the Source 1 entity name, in a Source 2 game, for
commanding an NPC's schedule. So §2's machine did not die either; HL:A's NPCs are
still schedule-driven at the authoring surface. Two architectures, both carried
forward, which is consistent with what they are for — §2 for authored
single-player NPCs, §3 for player-substitute bots.

### 4.2 Dota 2's bots: desire arbitration `[VALVE-PUB]`

The best-documented Source 2 decision system, because Valve documented it for
modders. Three levels:

> **Team Level** — *"code that determines how much the overall team wants to push each lane, defend each lane, farm each lane, or kill Roshan."*
>
> **Mode Level** — *"Modes are the high-level desires that individual bots are constantly evaluating, with the highest-scoring mode being their currently active mode."*
>
> **Action Level** — *"Actions are the individual things that bots are actively doing on a moment-to-moment basis."*

Twenty-one overridable modes: `laning`, `attack`, `roam`, `retreat`,
`secret_shop`, `side_shop`, `rune`, `push_tower_top/mid/bot`,
`defend_tower_top/mid/bottom`, `assemble`, `team_roam`, `farm`, `defend_ally`,
`evasive_maneuvers`, `roshan`, `item`, `ward`.

Each mode implements four functions:

| | |
|---|---|
| `GetDesire()` | *"Called every frame, and needs to return a floating-point value between 0 and 1 that indicates how much this mode wants to be the active mode."* Called for **all** modes, every frame. |
| `OnStart()` | *"Called when a mode takes control as the active mode"* |
| `Think()` | *"Called every frame while this is the active mode. Responsible for issuing actions"* |
| `OnEnd()` | *"Called when a mode relinquishes control to another active mode"* |

**This is utility AI, and it is the first of the four systems where priority is a
computed number rather than a position.** Every alternative is scored every frame
and the argmax runs. No ladder, no tree layout, no call order.

Three consequences worth naming, because they are the standing arguments for and
against utility selection:

1. **Adding a mode cannot break the others' relative order** — there is no order
   to break. Compare §2.9's `AddBehavior` array.
2. **Cost scales with the number of alternatives**, not with how deep the tree is.
   Twenty-one `GetDesire()` calls per bot per frame, always, even when nineteen of
   them are obviously zero.
3. **Two modes at 0.71 and 0.70 will swap on noise and the bot will thrash.**
   This is precisely the failure [`motion_matching.md`](motion_matching.md) §3.2
   documents in Unreal's pose search and fixes with a **negative
   `ContinuingPoseCostBias = -0.01`** on the option already running. A desire
   system without an incumbency bonus is the textbook case for that fix. Whether
   Valve's shipped modes carry one is **not knowable from the documentation**
   `[inferred]`.

### 4.3 What is *not* knowable

- **Half-Life: Alyx's NPC internals.** No source, no talk. The entity names
  imply schedule-based AI; nothing beyond that is sourced here.
- **CS2's bots.** The CS bot was never in a released SDK, and CS2 is closed.
  Community reports about behaviour changes after the Source 2 port are not
  evidence about architecture.
- **Whether Source 2 kept the `CAI_BaseNPC` condition bitmask,** its width, or its
  schedule text format. Unknown.

Unlike [`valve_networking.md`](valve_networking.md) — where the wire protocol is
dumped from shipping binaries on every update, so field names are Valve's own —
**there is no equivalent leak surface for AI.** AI does not go over the wire (see
[`networked_animation_physics.md`](networked_animation_physics.md): decisions are
inputs, and only their *results* replicate). So this section is, and will stay,
the thinnest in the note.

---

## 5. Unreal Behavior Trees `[EPIC-SRC]`

`Engine/Source/Runtime/AIModule/`. Epic's first answer, from UE4, still shipping
in 5.7.

### 5.1 It is not the textbook behavior tree

The canonical BT description — tick the root every frame, walk down, re-evaluate
everything — describes almost nothing about Epic's. What Epic actually built is
an **event-driven priority machine that happens to be drawn as a tree**.

The proof is in `ScheduleNextTick`:

```c
if (NextTickDeltaTime == UE::BehaviorTree::DisableTick)
{
    if (IsComponentTickEnabled())
        SetComponentTickEnabled(false);
}
```

**A behavior tree with a running latent task, no ticking services and no
time-based decorators turns its own component tick off entirely.** It costs
literally nothing per frame until something wakes it. The delta needed is
aggregated up from every active node via a `float& NextNeededDeltaTime` out-param
threaded through `WrappedTickNode`.

### 5.2 Priority *is* the drawing

Nodes are numbered depth-first at load (`InitializeNodeHelper`, one running
`uint16 ExecutionIndex`), and:

```c
bool FBTNodeIndex::TakesPriorityOver(const FBTNodeIndex& Other) const
{
    // instance closer to root is more important
    if (InstanceIndex != Other.InstanceIndex)
        return InstanceIndex < Other.InstanceIndex;

    // higher priority is more important
    return ExecutionIndex < Other.ExecutionIndex;
}
```

Eleven lines, and they are the entire priority model. **Lower execution index
wins, and execution index is left-to-right depth-first order, so "priority" means
"further left in the graph".** The composites are correspondingly trivial —
`Selector` advances on `Failed`, `Sequence` advances on `Succeeded`, and both
return `ReturnToParent` otherwise. There is no scoring anywhere in the system.

Compare Source 1: an `if`-ladder is also "first match wins", but you can *read*
it and it lives in one function. Here the ordering is spread across the visual
layout of a whole asset, and reordering two subtrees in the editor silently
changes every abort relationship in the tree. **This is §2.9's implicit-ordering
hazard again, promoted to a design principle instead of an accident.**

`InstanceIndex` first means a subtree closer to the root outranks anything in a
subtree it pushed — so a parent tree can always abort into its own higher-priority
branch even while a child tree is running.

### 5.3 Conditional aborts, quoted from the source

The whole reason a BT needs an abort mode is §5.2: since only the leftmost
runnable branch runs, a fact changing on the *left* of the running task must be
able to reach in and stop it.

```c
UENUM()
namespace EBTFlowAbortMode
{
    enum Type : int
    {
        None            UMETA(DisplayName="Nothing"),
        LowerPriority   UMETA(DisplayName="Lower Priority"),
        Self            UMETA(DisplayName="Self"),
        Both            UMETA(DisplayName="Both"),
    };
}
```

and `EvaluateBranch` states the semantics as a search *range*:

```c
// search range depends on decorator's FlowAbortMode:
//
// - LowerPri: try entering branch = search only nodes under decorator
//
// - Self: leave execution = from node under decorator to end of tree
//
// - Both: check if active node is within inner child nodes and choose Self or LowerPri
```

That is the clearest statement of conditional aborts anywhere, and it reframes
them usefully: **an abort mode is not "who can I interrupt", it is "which
interval of execution indices do I re-search".** `LowerPriority` searches
*inward* (I want in). `Self` searches *outward to the end* (I want out, and
whatever is next should take over). `Both` picks by asking whether the active
node is currently inside the decorator's branch.

The result code carries it: `Self` → `Failed`, `LowerPriority` → `Aborted`, and
`Aborted` is overloaded — `const bool bSwitchToHigherPriority = (ContinueWithResult == EBTNodeResult::Aborted);`
with a comment elsewhere warning *"although it shouldn't be set to Aborted, as it
has special meaning in RequestExecution (switch to higher priority)"*. A sentinel
value doing double duty, flagged by Epic's own comment as a hazard.

### 5.4 One pending request per frame, and the highest wins

```c
// check if it's more important than currently requested
if (bAlreadyHasRequest && ExecutionRequest.SearchStart.TakesPriorityOver(ExecutionIdx))
{
    UE_VLOG(..., TEXT("> skip: already has request with higher priority"));
    ...
    return;
}
```

The component holds **one** `ExecutionRequest`; competing requests in the same
frame collapse to the most important. This is structurally the same decision as
NextBot's `StorePendingEventResult` (§3.3) — *defer the transition, arbitrate,
apply once* — reached independently, with a different ordering source (graph
position vs. an explicit enum).

Epic also ship a guardrail worth copying:

```c
if (RelativePriority < EBTNodeRelativePriority::Same)
{
    const FString ErrorMsg(FString::Printf(TEXT("%s: decorator %s requesting restart has lower priority than Current Task %s"), ...));
    UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("%s"), *ErrorMsg);
    ensureMsgf(false, TEXT("%s"), *ErrorMsg);
}
```

A lower-priority decorator asking to abort a higher-priority task is *always* an
authoring bug, and it `ensure`s rather than silently no-oping. There is also a
comment upstream reading *"Adding code to track an problem earlier that is
happening by RequestExecution from a decorator that has lower priority"* —
i.e. this fired in production and someone went hunting. **The interesting part is
that the invariant is checkable at all**, which it is only because priority is a
number.

### 5.5 Decorators and services are *observers*, scoped to relevance

Both derive from `UBTAuxiliaryNode` with `OnBecomeRelevant` / `OnCeaseRelevant`.
A blackboard decorator registers an observer on its key **when its branch becomes
relevant** and unregisters when it does not — so a tree with 200 decorators is
only observing the handful on the active path. That is what makes §5.1's
tick-disable possible: nothing polls, keys notify.

Services fill the gap where something genuinely must be sampled:

> *"Behavior tree service nodes is designed to perform 'background' tasks that update AI's knowledge. Services are being executed when underlying branch of behavior tree becomes active, but unlike tasks they don't return any results and can't directly affect execution flow."*

with an `Interval` and — a small, real detail — a `RandomDeviation` added to it,
so a hundred agents sharing a tree do not all run their expensive enemy scan on
the same frame. **Phase-jittering a periodic AI task is a one-property fix for a
spike that otherwise looks like a mysterious 100-agent cliff.**

### 5.6 The memory model, which is the DOD part

> *"Template nodes are shared across all behavior tree components using the same tree asset and must store their runtime properties in provided NodeMemory block (allocation size determined by GetInstanceMemorySize())"*

So a `UBTTask_MoveTo` object exists **once per tree asset**, not once per agent.
Per-agent state lives in a flat `uint8` block; each node holds an offset into it.
At load:

```c
// sort nodes by memory size, so they can be packed better
InitList.Sort(FBehaviorTreeNodeInitializationData::FMemorySort());
```

**Epic sort the nodes by memory size before assigning offsets, purely for
packing.** That is a struct-of-arrays instinct applied to a `UObject` graph, and
it is the reason a BT is cheap to have a thousand of. It also explains the
constraint that trips up every new UE AI programmer — *do not write to member
variables in a decorator* — which is not a style rule but a data-race-shaped
correctness rule falling straight out of the sharing.

The cost is the one this project should notice: **because nodes are shared and
stateless, they cannot hold typed parameters for a specific invocation.** Hence
the blackboard — a stringly-keyed variant map — which is exactly the type safety
NextBot's constructor-argument transitions (§3.1) were built to preserve. The
same trade shows up in this repo's rules: caching a component pointer at bind
time beats a `findComponent<T>()` per access, and a blackboard key lookup is the
`findComponent` of AI.

---

## 6. Unreal StateTree `[EPIC-SRC]`

`Engine/Plugins/Runtime/StateTree/`. Epic's second answer, and it exists because
of §5.2.

### 6.1 The pitch, in one line

A BT's priority is its shape; a StateTree's priority is a field. Everything else
follows.

### 6.2 Selection behaviour is per state — including utility

```c
enum class EStateTreeStateSelectionBehavior : uint8
{
    None,                                   // cannot be directly selected
    TryEnterState,                          // select even if it has children
    TrySelectChildrenInOrder,               // first child that accepts
    TrySelectChildrenAtRandom,              // shuffle, then first that accepts
    TrySelectChildrenWithHighestUtility,    // highest utility score; ties break in order
    TrySelectChildrenAtRandomWeightedByUtility, // probability = normalised utility
    TryFollowTransitions,                   // don't select a child - trigger transitions instead
};
```

Seven behaviours, and the important thing is that they are **a property of the
parent state**, so one asset can be an ordered fallback ladder here, a random
picker there, and a utility selector somewhere else. A BT can express exactly one
of these (`TrySelectChildrenInOrder`) and needs custom composite classes for the
rest.

`TrySelectChildrenAtRandomWeightedByUtility` — sample proportional to normalised
score — is worth flagging separately: **it is the standard fix for utility AI's
determinism problem**, where the argmax means an agent in the same situation
always does the same thing and a group of them does it in unison.

Utility itself is `FStateTreeConsiderationBase` with
`GetNormalizedScore(Context)`, composed with `EStateTreeExpressionOperand` and a
`DeltaIndent` — i.e. considerations combine as a **parenthesised boolean/arithmetic
expression** authored as an indented list, the same trick as the BT's
`FBTDecoratorLogic` op-stack. It is marked:

> *"This feature is experimental and the API is expected to change."*

### 6.3 Transitions: explicit priority, collected leaf to root

```c
enum class EStateTreeTransitionTrigger : uint8
{
    OnStateCompleted = 0x1 | 0x2,
    OnStateSucceeded = 0x1,
    OnStateFailed    = 0x2,
    OnTick           = 0x4,
    OnEvent          = 0x8,
    OnDelegate       = 0x10,
};

enum class EStateTreeTransitionPriority : uint8
{
    None, Low, Normal, Medium, High, Critical,
};
```

and the algorithm, from the comment block at the top of `TriggerTransitions()`:

```
//1. Process transition requests. Keep the single request with the highest priority.
//2. Process tick/event/delegate transitions and tasks. TriggerTransitions, from bottom to top.
// If delayed,
//  If delayed completed, then process.
//  Else add them to the delayed transition list.
//3. If no transition, Process completion transitions, from bottom to top.
//4. If transition occurs, check if there are any frame (sub-tree) that completed.
```

with the collected handlers sorted by

```c
bool operator<(const FTransitionHandler& Other) const
{
    // Highest priority first.
    return Priority > Other.Priority;
}
```

**Leaf-to-root collection plus an explicit priority sort.** Leaf-to-root gives the
natural default — the most specific state gets first refusal — and the enum lets
any transition override that default without moving anything in the asset. That
is the fix for §2.9 and §5.2, and it is one `uint8`.

Two further details:

- **Delayed transitions are first-class.** A transition can carry a delay; the
  execution state holds a `DelayedTransitions` list and expiry is checked before
  evaluation. In a BT this needs a cooldown decorator plus a task; in Source 1 it
  needs a `TASK_WAIT` inside a schedule that must then be interruptible in
  exactly the right way. It is a common enough requirement that having it in the
  transition model is worth real money — a "wait half a second before actually
  panicking" is what stops AI reading as twitchy.
- Selection is bounded per tick, with the reasoning stated:

```c
// The state selection is repeated up to MaxIteration time. This allows failed EnterState() to potentially find a new state immediately.
// This helps event driven StateTrees to not require another event/tick to find a suitable state.
static constexpr int32 MaxIterations = 5;
```

Compare Source 1's `MAX_TASKS_RUN 10` with *"this is just here so infinite loops
are impossible"*. **Both engines bound the re-decide loop with a small constant
and say so in a comment.** Twenty years apart, same constant to within a factor of
two, and neither is tuned. If this project builds a decision loop, it gets one of
these on day one.

### 6.4 StateTree is what Mass uses at crowd scale

`Engine/Plugins/AI/MassAI/Source/MassAIBehavior/` — `MassStateTreeProcessors`,
`MassStateTreeFragments`, `MassStateTreeSchema`. Mass entities run StateTrees,
not Behavior Trees, and the settings carry:

```c
UPROPERTY(EditAnywhere, Category = "Mass|LOD", config)
int32 MaxActivationsPerLOD[EMassLOD::Max];
```

**A per-LOD cap on how many StateTrees may be activated per frame.** That is
[`lod_systems.md`](lod_systems.md) §7's AI-LOD argument appearing as a shipped
config field, and it is an admission-control system of the same shape as Source
1's `bits_MEMORY_TASK_EXPENSIVE` (§2.8) — the expensive thing here being
*starting to think*, not thinking. Note it is a budget on **activations**, not on
ticks: the transition is what costs, because it runs selection.

That StateTree, not BT, is the one wired into Mass is the clearest available
statement of Epic's direction `[inferred]`: **the BT's per-agent `UObject`
component and its blackboard do not go to ten thousand agents, and a
struct-based state machine does.**

### 6.5 And the one Epic shipped and stopped

`Engine/Plugins/AI/HTNPlanner/HTNPlanner.uplugin`:

```json
"Version" : 1,
"VersionName" : "0.01",
"Description" : "[EXPERIMENTAL] Adds experimental support for Hierarchical Task Network (HTN) planner to the UE4's AI module",
"EnabledByDefault" : false,
"IsBetaVersion" : true,
```

Version 0.01, still describing itself as targeting **UE4's** AI module, in UE 5.7.
Same signal as [`rigging_ik.md`](rigging_ik.md) §4's two full-body IK solvers both
still Experimental: **Epic ship exploratory systems and leave them.** Do not read
the presence of a plugin as an endorsement of the technique, and do not build a
planner here because Unreal has a folder called `HTNPlanner`.

---

## 7. The comparison, on the one axis that matters

| | Source 1 schedules | NextBot Actions | UE Behavior Tree | UE StateTree | Dota modes |
|---|---|---|---|---|---|
| **Shape** | FSM + task lists | HFSM + suspend stack | priority tree | HFSM + explicit transitions | flat utility set |
| **Priority written as** | source-line order | call/stack position | graph left-to-right | `uint8` enum | `float` desire |
| **Re-decide test** | 256-bit AND | event walks the stack | key-change observers | trigger flags, leaf→root | argmax, every frame |
| **Concurrency** | none | containment + 2 Behaviors | SimpleParallel only | tasks per active state | none |
| **Transition args** | none (side state) | **constructor, type-safe** | blackboard keys | property bindings | globals |
| **Failure path** | first-class (`SelectFailSchedule`) | `Done()` resumes buried | `Failed` walks up | `OnStateFailed` trigger | desire drops |
| **Per-agent cost floor** | one `GatherConditions` | one `Update` recursion | **zero — tick disables** | one tick | N desires |
| **Debug story** | named schedule + named break condition | **reason string per transition** | visual debugger + vlog | trace/debugger | numbers |
| **Explodes when** | behaviours multiply (429 schedules) | ladders get long | the graph gets reordered | states multiply | options multiply |

Three things fall out of that table.

**One: everybody defers the transition and arbitrates.** NextBot stores a desired
result and applies it next `Update`; the BT keeps one `ExecutionRequest` and takes
the highest-priority; StateTree collects handlers and sorts. None of them
transitions at the moment of the trigger, and all three give the same reason —
you cannot safely delete the state you are executing inside. That is a
convergent-evolution result and should be treated as a rule, not a preference.

**Two: the systems that got better got their priority ordering out of the
container.** Source 1 → array order. BT → graph order. StateTree → a field. Dota
→ a float. The two newest are the two explicit ones.

**Three: cost floor is the axis nobody advertises.** The BT is the only one of the
five that costs *nothing* when nothing is happening, and it achieves that with the
observer registration in §5.5. Utility costs the most and cannot be made to cost
less, because "evaluate everything every frame" is its definition.

---

## 8. What this project should take, and what it should not

### 8.1 Take: the condition bitmask, essentially verbatim (§2.4)

Gather expensive facts once per agent per tick into a fixed-width bitset; give
each intention a mask of the bits that may interrupt it; test with an AND. It is
eight `uint32` ops, it is trivially debuggable (find the first set bit, print its
name — Source does exactly this and logs `"Break condition -> %s"`), and it makes
"should I reconsider" free enough that you can afford to ask every frame for
every unit.

This is the same derived-cache pattern CLAUDE.md already mandates, one layer up:
**the authoritative state is the world; the condition bitset is the summary; the
decision loop is only ever allowed to read the summary.** Invalidate at the
boundary that owns it — `GatherConditions()` — exactly as `World::at()` dirties
the occlusion grid.

Fix the width once and know it is a save-format decision.

### 8.2 Take: three-valued contextual queries (§3.5)

Any place the rest of the codebase wants to ask the AI a question — *should this
unit take this shot? is this cover acceptable? may I path through here?* — should
get `YES / NO / UNDEFINED` from the innermost intention outward, not a boolean
from a default. This project already has the situations: overwatch, target
selection, ability placement. Costs an enum.

### 8.3 Take: a reason string on every transition (§3.1)

Not a debug feature bolted on later — a **parameter of the transition
constructor**, so it cannot be omitted. `"Break condition -> COND_HEAVY_DAMAGE"`
and `"Run away from threat!"` are the difference between a five-minute and a
five-hour diagnosis of "why did that unit do that". It costs a `const char*`.

### 8.4 Take: explicit priority. Never container order.

Three shipped instances of the same bug are in this note: `AddBehavior` order
(§2.9), BT graph position (§5.2), and — from
[`source2_animation.md`](source2_animation.md) §7 — AnimGraph transition creation
order. Valve hit it twice, Epic once, and Epic's fix was a `uint8`.

The general form: **if reordering a container silently changes behaviour, the
ordering is a design decision that has not been written down.** Cheap now,
expensive after there are forty of them.

### 8.5 Take: bound the re-decide loop, and let expensive work self-report

`MAX_TASKS_RUN = 10` (§2.8), `MaxIterations = 5` (§6.3),
`MaxActivationsPerLOD` (§6.4), `bits_MEMORY_TASK_EXPENSIVE` (§2.8),
`RandomDeviation` on service intervals (§5.5). Five separate admission-control
mechanisms across two engines, all trivially cheap, all present because the
alternative shipped once and hurt. Put the loop bound in when the loop is
written.

### 8.6 Take: incumbency, from a note that is already in this directory

Every scored selector in this note has the thrash problem, and only Unreal's pose
search is documented as fixing it. Dota's desires (§4.2) and StateTree's utility
considerations (§6.2) are both argmax over noisy floats. Whatever this project
scores — cover, targets, ability positions, and now *intentions* — gets
`motion_matching.md` §8.2's small bias toward the option already running.
CLAUDE.md already carries this rule; §4.2 is just one more instance of where it
bites.

### 8.7 Do not build: a behaviour graph editor, a blackboard, or a planner

- **Not a visual graph.** UE's BT needs one because its priority is its layout,
  which is circular. Source's 429 schedules are *text*, editable in the same file
  as the code that selects them, and that is the better fit for a project with one
  programmer and no content team.
- **Not a blackboard.** It exists to work around shared stateless node templates
  (§5.6) at a scale this project will never have. Typed constructor arguments
  (§3.1) are strictly better and CLAUDE.md's hot-loop rules already ban the map
  lookup a blackboard is.
- **Not an HTN or GOAP planner.** §6.5 is Epic's own verdict by neglect. A planner
  earns its place when the action space is large and the goal is variable; a tile
  tactics game has a small, enumerable action space and a goal that never changes.
- **Not an archetype ECS for intentions.** CLAUDE.md says this about the entity
  layer and it holds here: the cost is in the queries a decision makes
  ([`spatial_queries.md`](spatial_queries.md)), not in iterating the deciders.

### 8.8 The shape that actually fits this project `[inferred]`

Turn-based tactics has a property none of these four systems assume: **decisions
are rare and expensive, not continuous and cheap.** One unit decides once per
turn, may spend milliseconds doing it, and nothing interrupts it mid-think.

That inverts the priorities. The interrupt machinery of §2–§6 — the whole reason
these architectures are shaped as they are — is *nearly worthless* here, and the
scoring layer is nearly everything. So:

- Take §2.4's bitmask anyway, but for **preconditions and filtering**, not
  interruption — "which of my 40 abilities are even legal this turn" is the same
  AND.
- Take §3.5's queries and §3.1's reason strings, which are about legibility and
  cost nothing.
- Spend the actual effort in the place [`spatial_queries.md`](spatial_queries.md)
  §3.6 already identifies: **sort candidates cheap, then run the expensive test
  from the best down and stop at the first pass.** For a turn-based unit, the
  branching structure is a formality and the candidate enumeration is the bill.
- Keep the ability to run the *same* decision layer continuously later, because
  `cromwell` is aimed at RTS and FPS too, and those need every one of §2–§6's
  interrupt mechanisms. The bitmask and the query interface are the two pieces
  that serve both, which is a reason to build those first rather than a
  coincidence.

---

## 9. Sourcing and honest limits

**Strong.** §2, §3 and §4.1 are read from Valve's own shipping C++ on this
machine, including comments. §5 and §6 are read from Epic's engine source.
Enum values, constants (`MAX_TASKS_RUN 10`, `MaxIterations 5`, `MAX_CONDITIONS
32*8`, `timeLimit 8/16`) and every quoted comment are transcribed, not
remembered. The counts in §2.1 are grepped from the tree and reproducible.

**Medium.** §3.7 and Booth's Main/Legs split come from the AIIDE-09 deck, which is
Valve-published but is slides — the quotes are verbatim, the surrounding
interpretation is mine. §4.2 is Valve's own bot-scripting documentation, quoted,
but documentation describes the modding surface and not necessarily the engine
under it.

**Weak, and marked in place.** §4.1's HL:A thread is one entity name from
community tutorials. §4.3 lists what is unknown rather than guessing. §6.4's
reading of Epic's direction and all of §8.8 are `[inferred]`.

**Licence.** Source SDK 2013 is non-commercial; Unreal's source is EULA-bound.
Both are **read-only for this project** — take the ideas, not the files. That is
the same conclusion [`rigging_ik.md`](rigging_ik.md) §6 reached for the animation
layer, and unlike that case there is no permissively licensed reference
implementation to fall back on: **there is no MIT-licensed behaviour-tree or
schedule system worth taking.** This layer is small enough to write, which is
part of why nobody has bothered to make one.

**Not covered here.** Perception and memory (`ai_senses`, `ai_memory`,
`CKnownEntity`, UE's `AIPerceptionComponent`) — that is what *fills* the condition
bits, and it is a note of its own. Squad and group coordination beyond §2.6's
strategy slots — see [`gears_tactics.md`](gears_tactics.md). Dialogue and the
response system (`AI_ResponseSystem`, `AI_Criteria`), which is a genuinely
separate rules engine sitting beside the schedule machine and is the most
underrated thing in the Source SDK.
