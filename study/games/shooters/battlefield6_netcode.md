# Battlefield 6 — netcode and replication

How Frostbite replicates a 64-player battlefield, read from the shipped
executable. The companion to
[`frostbite_rendering.md`](../rendering/frostbite_rendering.md), and written
under the same constraint: **EA have published essentially nothing about
Frostbite networking**, so almost everything here is read out of the binary
rather than quoted from a talk.

That inverts the usual evidence situation. The rendering note had SIGGRAPH course
notes with speaker notes and millisecond tables; this one has **one 2014
community blog post, one developer statement on X, and 1,643 recovered symbols**.
The symbols turn out to be unusually legible — Frostbite names things
descriptively and ships those names — so the *architecture* is recoverable in
detail while the *values* are almost entirely not.

| tag | source |
|---|---|
| **[BIN]** | **Our own read of the shipped `bf6.exe`** — 195,618,664 bytes, file-stamped **2025-08-11**, retail Steam install. A plain string dump: no injection, no code run, no game data decrypted. 125,465 unique strings. Categorised networking extract in [`battlefield6_net_strings.txt`](battlefield6_net_strings.txt). Scope and limits in §0.1. |
| **[DICE14]** | *Addressing "Netcode" in Battlefield 4*, DICE, 2014, on battlefield.com. A **community-facing post, not an engineering document** — no formats, no rates, no algorithms. Valuable for one thing: it describes behaviour that the 2025 binary's symbol names explain. |
| **[DEV]** | Statements by named EA/DICE staff reported by press. Specifically David Sirland, BF6 Lead Producer, on X, on the launch tick rate. A developer's word, but second-hand and without engineering detail. |
| **[PATCH]** | Official BF6 patch notes. Useful only where they name an internal system — they named `TimeNudge`, which the binary confirms. |
| **[3P]** | Third-party reporting and community measurement. Observation, never implementation. |
| **[inferred]** | Our reading, not EA's. Used heavily here, and always marked. |

---

## 0. What this document can and cannot support

### 0.1 The evidence is names, not values

Everything tagged [BIN] is a symbol name compiled into the retail executable.
This is the same evidence class as `frostbite_rendering.md` §0.2 and the same
rules apply, but the consequence is sharper here because netcode is a subject
where **the numbers are the whole argument**.

| [BIN] supports | [BIN] does **not** support |
|---|---|
| That a system exists and is compiled in | That it is enabled, or on which platform |
| The **shape** of a system — that time nudging has three calculator strategies | Which strategy ships as default |
| That a **budget exists** — `MaxNetObjectCount`, `MaxNetworkedDestructionBits` | **The budget's value.** These live in data, not the binary |
| The **vocabulary** — `Realm`, `Ghost`, `TimeNudge`, `DamageArbitration` | The wire format, packet layout or bit counts |
| That a failure mode was anticipated — `OversizedCorrectionPayloadIsFatal` | How often it fires |

**No packet was captured and no traffic was observed.** Every claim about the
wire is inference from a symbol name, and marked.

### 0.2 Two unrelated meanings of "ghost"

A trap that would corrupt the whole document if missed. The string `Ghost`
appears 226 times in `bf6.exe` in **three** distinct senses:

1. **Replication ghosts** — the real subject. `EntityGhost`, `GhostNetObjectCreator`,
   `MaxGhostCount`, `ClientState_WaitingForGhosts`. A ghost is a client-side
   proxy of a server-authoritative entity.
2. **`ScenarioGhost*`** — a *completely different system*.
   `ScenarioGhostType_FromJoin`, `ScenarioRallySlotType_Ghost`,
   `ScenarioStatusFlags_Interacting_GhostConnected`,
   `ScenarioGhostDisconnect_PostAlign`. [inferred] This reads as a
   party/rally/matchmaking placeholder — a reserved slot for a player who has not
   joined yet — and has nothing to do with entity replication.
3. **Cosmetic names** — `GhostShark`, `GhostRaptor`, `Ghostface`, `BlueGhost`.
   Skins and callsigns. Pure noise.

The companion strings file filters (3) and keeps (1) and (2) separated. **Any
analysis that greps `Ghost` and reports a count is wrong.**

---

## 1. The architecture in one page

Three layers, each with its own vocabulary, all confirmed present in the 2025
binary [BIN]:

| layer | what it is | evidence |
|---|---|---|
| **Backend — Blaze** | EA's online services SDK: authentication, entitlements, friends/association lists, matchmaking, game session management | ~814 `Blaze::*` strings, e.g. `Blaze::GameManager::ReplicatedGameData`, `Blaze::Authentication::*`, `Blaze::Association::*` |
| **Transport — DirtySDK / DirtySock** | EA's long-standing low-level network library: sockets, TLS (`ProtoSSL`), HTTP/2, and `NetGameLink` for game traffic | `DirtySockMaxConnectionCount`, `DirtySockClientPacketQueueCapacity`, `DirtySockServerNetGameLinkQueueLength`, `DirtySockSingleThreaded` |
| **Game replication — `NetObjectSystem`** | Frostbite's own entity replication: ghosts, realms, net states, packers, interpolation, prediction | `NetObjectSystem`, `NetObjectSystemSettings`, `NetObjectSystemInterpolationSettings`, `GhostNetObjectCreator` |

**The model is server-authoritative with client prediction**, which the binary
states plainly: `ScenarioNetworkMode_ServerAuthoritative`, `Server authority`,
`MaxRemoteAuthorityNetObjectCount`, and a client/server split running through
every gameplay type (§2).

There is **no evidence of peer-to-peer or listen-server topology for
multiplayer** in the strings, and `ClientState_StartServer` exists alongside
`ClientState_ConnectToServer` — consistent with a dedicated-server model where
the client can also host locally for single-player. [BIN] + [inferred]

---

## 2. Realms — the client/server code split

**The most architecturally distinctive thing in the binary, and the thing most
worth stealing.** [BIN]

Frostbite partitions gameplay code by **realm**. A realm is a tag on code and
data saying which side of the wire it lives on:

```
RealmEx_Client   RealmEx_Server   RealmEx_Render   RealmEx_Test   RealmEx_Invalid
EcsStreamRealm_Client / _Server / _Both / _None
BTreeRealm_Client / _Server / _Both
```

and the replication direction is a separate axis:

```
RealmReplication_ServerToClient
RealmReplication_ClientToServer
RealmReplication_ClientToRender
RealmReplication_NoReplication
```

**What this buys, and why the naming everywhere else follows from it.** Nearly
every gameplay type in the binary exists as a matched pair:

| client | server |
|---|---|
| `ClientDestructionComponent` | `ServerDestructionComponent` |
| `ClientGhostEntityOwner` | `ServerGhostEntityOwner` |
| `ClientSquadManagerGhostHelper` | `ServerSquadManagerGhostHelper` |
| `ClientPlayerStateMachineReplicationEntity` | `ServerPlayerStateMachineReplicationEntity` |
| `SchematicsInstanceClientGhost` | `SchematicsInstanceServerGhost` |
| `ClientAuthoritativeAimInputEntity` | `ServerAuthoritativeAimInputEntity` |

with `Cl`/`Sv` short forms too (`ClStaticGhostGroup`, `SvBlueprintGhostEntityOwner`)
and even `DummyClientDestructionComponent` / `DummyServerDestructionComponent`
for the null cases.

[inferred] This is **one authored system split by realm at build time**, not two
hand-written implementations. That is the only reading consistent with the sheer
regularity of the pairing — hundreds of types, no exceptions. It also explains
`CrossRealmDataAccessor` and `AllRealmObjectsAccessNone` / `…AccessReads` /
`…AccessEdits`: reaching across the split is possible but is an explicit,
permission-checked act.

There is a third and fourth realm beyond client/server:

- **`RealmEx_Render`** and `RealmReplication_ClientToRender`, plus
  `IsRenderAuthoritative` — the renderer is treated as a *replication target* in
  its own right, receiving state from the client sim the way the client receives
  it from the server. [inferred] This is the same "presentation is downstream of
  simulation" idea as `DiceChannelRealm_Simulation` / `_SimulationSynced` /
  `_Presentation` and `DiceGameplayExpressionRealm_Simulation` / `_DerivedState`
  / `_Presentation`.
- **`RealmEx_Test`** — a realm for tests, which is how you get deterministic
  netcode tests without a network.

**Why this matters for our engine.** CLAUDE.md's rule is that `cromwell` may not
name anything in `game/`. Realms are the same *kind* of rule applied to a
different axis — a compile-time-checked partition that makes an
easy-to-violate-silently boundary into one the build enforces. A single-player
engine that ever wants multiplayer pays enormously for not having drawn this line
early, because "does this code run on the server?" becomes unanswerable once
thousands of call sites exist. See §9.

---

## 3. Ghosts — the replication unit

A **ghost** is the client-side proxy of a server entity. [BIN]

### 3.1 Object budgets

Four separate counts, which is itself informative — Frostbite distinguishes
categories of networked object rather than pooling them: [BIN]

| setting | [inferred] meaning |
|---|---|
| `MaxNetObjectCount` | total networked objects |
| `MaxStaticNetObjectCount` | level-placed objects that are networked but never spawn/despawn |
| `MaxRemoteAuthorityNetObjectCount` | objects whose authority sits on the far side |
| `MaxLocalSimulatedNetObjectCount` | objects simulated locally rather than replicated |
| `MaxGhostCount` | ghosts specifically |
| `MaxClientToServerGhostCount` | the client→server direction, budgeted separately and presumably far smaller |

**None of the values are in the binary.** They are data.

`NetObjectOverflow` and `NetObjectOverflowSettings` exist, so exceeding these is
a handled runtime condition rather than an assert. [BIN]

### 3.2 Static ghost groups

`StaticGhostGroup`, `StaticGhostGroupManager`, `ClientStaticGhostGroup`,
`ServerStaticGhostGroupManager`, and a state-type enum: [BIN]

```
StaticGhostGroupNetStateType_AutomaticSlidingDoor
StaticGhostGroupNetStateType_AutomaticSwingDoor
StaticGhostGroupNetStateType_InteractableDoor
```

[inferred] Level-placed interactables — doors above all — are **grouped** rather
than replicated individually. A map with hundreds of doors cannot afford a
full net object each; grouping them into a shared state block with a small
per-door state type is the standard fix, and the enum says exactly which cases
were worth special-casing.

### 3.3 Replication priority and frequency

- `ReplicationPrioritySettings`, `MinimumPriorityToReplicate` — priority-based
  selection of what to send. [BIN]
- `EnableVariableGhostFrequency` — **per-ghost variable update rate.** [BIN]
  [inferred] The nearby, contested, player-relevant entities update often; the
  distant ones update rarely. This is the single most important bandwidth lever
  in a 64-player game and it is a named toggle.
- `NetObjectProximityMethod`, `DefaultProximityMethod`, `DefaultProximityValue`,
  `EnableParallelProximityCalculation` — relevance is proximity-driven, and the
  calculation is parallelised, which implies it is expensive enough to matter.
  [BIN]
- `MoveManagerOutgoingFrequencyDivider` — the client's outgoing input rate is a
  *divider* on some base rate rather than an independent number. [BIN]

### 3.4 Send ordering and dependencies

`NetObjectDependencyType_*` is a small, careful enum and worth reading in full,
because it is the answer to "the client received a reference to an object it does
not have yet": [BIN]

```
NetObjectDependencyType_None
NetObjectDependencyType_Init
NetObjectDependencyType_InitFlushDependencyFirst
NetObjectDependencyType_FlushDependencyFirst
NetObjectDependencyType_SendDependencyFirst
NetObjectDependencyType_ResolveBeforeSend
NetObjectDependencyType_ResolveOnRemote
NetObjectDependencyType_ResolveOnlyIfUnfiltered
```

[inferred] Three strategies visible: **order the send** (`SendDependencyFirst`,
`FlushDependencyFirst`), **resolve locally before sending** (`ResolveBeforeSend`),
or **ship the reference and let the remote resolve it** (`ResolveOnRemote`).
`ResolveOnlyIfUnfiltered` is the interesting one — if relevance filtering means
the remote will never see the dependency, do not drag it into the stream just to
satisfy a reference.

### 3.5 Connection lifecycle

The client state machine, complete: [BIN]

```
Startup → LoadProfileOptions → StartServer | ConnectToServer
        → WaitingForServer → StartLoadingLevel → WaitingForLevel
        → WaitingForStaticBundleLoad → WaitingForLevelLoaded
        → WaitingForLevelLink → LevelLinked → WaitingForGhosts
        → Ingame → LeaveIngame → ShuttingDown → Shutdown
        (LostConnection from anywhere)
```

**`WaitingForGhosts` is a distinct state between "level is loaded" and "in
game".** [inferred] The client does not enter the world until the initial ghost
set has arrived — i.e. there is an explicit initial-state sync barrier, not a
"spawn in and let it stream" approach.

---

## 4. Net state — how a ghost's data is described and packed

### 4.1 Data-driven descriptors

`NetStateDescriptorResource` is a **resource**, i.e. authored data rather than
code. [BIN] Together with `NetStateMaxFlagCount`, `NetStateGroup_Group0` …
`_Group31` (**32 groups**) and `NetStateLogInterval`, this says replicated state
is declared in data and grouped, presumably for delta-tracking and priority.
[inferred]

The per-field detail is visible where gameplay constants leaked into the binary:

```
GrenadeEntityNetStateDefs_PrimedTimeMaxBits
GrenadeEntityNetStateDefs_PrimedTimeScale
HealthPercentageConstants_NetStateMaxValue
MotionMachineRequestNetStateFlags_HaveTarget / _RequestValid / _Trigger
MotionMachineInfo_IndexBitCount
```

**`MaxBits` + `Scale` per field is textbook quantisation**: pick a range, pick a
bit count, scale into it. A grenade's primed time is not a float on the wire —
it is an integer of declared width. [BIN] + [inferred]

### 4.2 The packer library

A typed packing layer with range checking, recovered from its error strings —
which is the nicest kind of [BIN] evidence, because an error message states the
contract: [BIN]

```
IntPacker value %lld out of range [%lld;%lld], %u bits
IntLimitPacker value %d out of range [%d;%d]
ScaledVec3Packer value %f, %f, %f out of range [%f, %f, %f ; %f, %f, %f]
ScaledVec4Packer value %f, %f, %f, %f out of range [...]
ExtrapolatableFloatAngularPacker value %f out of range [0; Tau]
ExtrapolatableFloatAngularPacker velocity %f out of range [%f;%f]
LinearTransformPacker scaled values are not supported
Invalid ExtendableEnumPacker value %08X. Has the enum specific initCache()
    method been called in the appropriate RuntimeModule?
```

plus `ContextualPacker` / `ContextualPackerShared` [inferred: packing whose
encoding depends on context already known to both ends].

And the protocol's error enum, small enough to quote entire: [BIN]

```
NetError_NoError
NetError_Object_InvalidHandle      NetError_Object_UnknownCreator
NetError_Object_CreationDenied     NetError_Object_CreationFailed
NetError_Object_BitCountMismatch
NetError_Packer_ValueOutOfRange    NetError_Packer_ArraySizeTooLarge
NetError_Packer_UnknownUnionType
```

`NetError_Object_BitCountMismatch` is the tell that **both ends must agree
exactly on the bit layout** — there is no self-describing wire format, and a
mismatch is a first-class error rather than a corruption. [inferred]

### 4.3 Extrapolatable types

The replicated value types carry **value *and* velocity**: [BIN]

```
ExtrapolatableFloat.Value       .Velocity
ExtrapolatableVector3.Value     .Velocity
ExtrapolatableVector4.Value     .Velocity
ExtrapolatableQuaternion.Value  .Velocity
ExtrapolatableTransform.Value   .Velocity  .AngularVelocity
```

[inferred] This is a deliberate design position: rather than sending positions
and having the receiver difference them to guess motion, **the sender ships the
derivative explicitly**. The receiver can then extrapolate correctly through a
dropped packet, and the angular packer's explicit velocity range check
(`velocity %f out of range`) shows the derivative is quantised on its own budget.

---

## 5. Time, interpolation and "TimeNudge"

**The most player-visible netcode system, and the one BF6's patch notes named.**

### 5.1 TimeNudge

`TimeNudge` is the client's offset between its own clock and the server's — in
effect, how far in the past remote entities are rendered so that interpolation
always has two samples to work between. [inferred, from the name plus the
surrounding settings]

Recovered configuration: [BIN]

| symbol | reading [inferred] |
|---|---|
| `TimeNudgeCalculatorType_GameTime` | derive the offset from game-time comparison |
| `TimeNudgeCalculatorType_PacketDelta` | derive it from observed inter-packet delta (i.e. jitter) |
| `TimeNudgeCalculatorType_PacketPrediction` | predict the arrival of the next packet |
| `AutomaticTimeNudge` | adapt it at runtime rather than fixing it |
| `TimeNudgeBias`, `TimeNudgeMax` | a deliberate bias and a ceiling |
| `HighTimeNudgeInMS`, `CriticalTimeNudgeInMS` | two thresholds — a warning level and a critical level |
| `DefaultTimeNudgeSettings`, `SinglePlayerTimeNudgeSettings`, `MemorySocketTimeNudgeSettings` | three profiles, including one for single-player and one for an in-memory socket |
| `PerformanceIconType_TimeNudge` | **surfaced to the player as an icon** |

That last one connects directly to [DICE14], which introduced exactly such an
icon for BF4:

> "The first icon, seen at the top in the shape of a clock, indicates that your
> connection to the server is lagging. … The effect of such lag is that it will
> take a bit longer for you to see what is happening in the game world."
> — [DICE14]

**Twelve years apart, the same concept and the same clock icon.** [inferred] The
`SinglePlayerTimeNudgeSettings` profile is a nice detail: even offline, the sim
runs through the same client/server realm split (§2), so it still needs a nudge
configuration — just a trivial one.

`[PATCH]` confirms TimeNudge is still being actively tuned: BF6 update 1.3.3.0
is reported to have adjusted "TimeNudge behavior" to reduce "delayed damage, late
enemy position updates, and extreme cases of dying behind cover".

### 5.2 Object nudging

Separate from time nudging, and about *position* rather than *clock*: [BIN]

```
ObjectNudgeCalculator_Default / _Frequency / _Noop
FrequencyBasedObjectNudgeCalculatorSettings
MaximumNudge, MaximumNudgeSpeed, ObjectNudgeDataAgeLimit
```

[inferred] When a correction arrives, you do not teleport the object — you nudge
it toward the corrected position at a bounded speed. `MaximumNudgeSpeed` is the
knob that trades "converges to truth quickly" against "visibly slides".
`ObjectNudgeDataAgeLimit` presumably stops nudging toward information that has
gone stale. The `_Frequency` variant scales the nudge by the object's update
frequency — an object updated rarely should be nudged more gently. [inferred]

### 5.3 Interpolation modes

`NetObjectInterpolationMode_Legacy` and `NetObjectInterpolationMode_Jotunheim`
[BIN]. **A new interpolation mode with a codename, shipping alongside the old
one.** Nothing else about Jotunheim is recoverable — no EA source mentions it.
Also present: `NetInterpolatorAdapter`, `InterpolationManagerSettings`,
`ClientInterpolationPooledBufferSize`, `Interpolation.PerfTrackLevel`.

### 5.4 Clock sync telemetry

`BFNetworkTimeSyncReportBucket_ForwardMinor` / `_ForwardExtreme` /
`_BackwardMinor` / `_BackwardExtreme` [BIN]. [inferred] Clock corrections are
bucketed by direction and severity and reported as telemetry — DICE measure how
often a client's clock has to jump, and by how much, in the field.

---

## 6. Prediction and correction

### 6.1 Correction payloads

[BIN]:

```
CorrectionClient    CorrectionServer    CorrectionEnabled
MaxCorrectionStateSize      MaxAllowedCorrectionPayloadSize
MaxCorrectionUpdateCount    EnableAutomaticCorrectionUpdateCount
OversizedCorrectionPayloadIsFatal
CorrectionCache status:
```

[inferred] Corrections are bounded in **size** and **count per frame**, and
exceeding the size bound is configurably fatal — a development-time tripwire for
"someone made a predicted state too big to correct". `EnableAutomaticCorrection
UpdateCount` suggests the per-frame correction budget can adapt.

### 6.2 Client input authority

[BIN]:

```
ClientAuthoritativeInputExtent      AuthoritativeInputExtent
ClientAuthoritativeAimInputEntity   ServerAuthoritativeAimInputEntity
ClientAuthoritativeCameraInputExtent
AuthoritativeAimInputInputExtent
SetClientAuthoritativeAimingCameraController
AuthorityTimeoutTicks
```

[inferred] **Aim and camera are client-authoritative**, which is the standard
choice for a shooter — you cannot make aim feel right if it round-trips — while
the *consequences* of aim are arbitrated (§7). `AuthorityTimeoutTicks` bounds how
long a claimed authority survives without confirmation.

### 6.3 Physics-level prediction

The physics layer has its own prediction/correction vocabulary: [BIN]

```
EA.Physics.Bodies.PredictionCorrection
EA.Physics.Bodies.NextPredictionCorrection
EA.Physics.Bodies.CorrectionEnabled
EA.Physics.Bodies.LinearCorrection / .AngularCorrection
AntRigidBodyCollisionLayer_PredictedVehicleLayer
```

[inferred] `PredictedVehicleLayer` as a *collision layer* is notable — predicted
vehicle bodies live on their own layer so they can be made to interact (or not)
with confirmed bodies. Vehicles are the hardest prediction case in Battlefield
and this says they got dedicated treatment.

---

## 7. Damage arbitration — the hit registration model

**The best-recovered system in this document, and the one that most directly
answers "why did I die behind cover".**

### 7.1 The model

Clients report hits; the server arbitrates. [BIN]

```
AllowClientSideDamageArbitration
DamageArbitrationMatchingStrategy
DamageArbitrationExpirationDuration
DamageArbitrationRealmFilterClientDamage
DamageArbitrationRealmMinCooldown / RealmMaxCooldown
DamageArbitrationPostAllEvents
DamageArbitrationLogDamageAccepted
```

The server does not simply trust the claim. It **recomputes and compares**: [BIN]

```
ServerAuthoritativeBulletDamage
ServerAuthoritativeBulletDamageThreshold
ServerAuthoritativeBulletDamageReportThreshold
ServerAuthoritativeBulletDamageMismatchReportOnly
ServerAuthoritativeBulletDamageUseBulletMaterial
ServerAuthoritativeBulletDamageValidateMaterial
```

[inferred] A tolerance **threshold** (client and server need not agree exactly),
a separate **report threshold** for telemetry, and a **report-only mode** so the
validation can be run in the field measuring disagreement without yet rejecting
anything. That is how you ship a change like this safely, and its presence
suggests it was rolled out exactly that way. `ValidateMaterial` /
`UseBulletMaterial` mean the server checks *what the bullet hit* — the material
determines penetration and damage falloff, so a client claiming a hit through the
wrong material is caught.

### 7.2 Weapon identity and timing checks

[BIN]:

```
DamageArbitrationEnableWeaponHashDebug
TweakableRefType_KS_DisableDamageArbitrationWeaponHashkMatching   [sic]
TweakableRefType_GS_DamageArbitrationDelayedWeaponHashCalculation
TweakableRefType_KS_ServerDamageArbitrationDisableUnlockMatchForArbitration
TweakableRefType_GS_DamageArbitrationDamageReceivedLateThreshold
TweakableRefType_GS_DamageArbitrationProcessedLateThreshold
TweakableRefType_KS_DisableDamageArbitrationUsingInputLatency
```

[inferred] The claim carries a **weapon hash** the server matches against what
that player is actually holding — and an *unlock* match, so a player cannot claim
damage from a weapon or attachment they have not unlocked. Late-arriving damage
is rejected past a threshold, in two flavours (**received** late vs **processed**
late). And arbitration explicitly **accounts for the reporting client's input
latency** — that is lag compensation, stated as a toggle.

`DamageArbitrationDisableTTKSpacing` [inferred]: TTK spacing enforces a minimum
interval between damage events, presumably rejecting physically impossible rates
of fire.

### 7.3 The hybrid fallback

Two settings that change the model per-situation: [BIN]

```
UseServerSideHitDetectionForHighPing
RequiredVelocityForServerSideHitDetection
```

[inferred] **Client-side hit detection is not universal.** High-ping players are
switched to server-side detection — the classic trade where the disadvantaged
player stops being able to impose their lag on everyone else — and *fast-moving*
targets above a velocity threshold are also resolved server-side, because
client-side rewind is least trustworthy exactly when the target is moving fastest.

### 7.4 What [DICE14] said about the same system, twelve years earlier

The 2014 post describes this machinery from the outside, without naming it:

> "This could happen when a portion of damage dealt was **rejected by the
> server**, since the bullets that caused it were **fired after the point of
> death for the firing player** – the kill card would show the health **as
> predicted by your game client**, rather than the health **confirmed by the
> server**." — [DICE14]

That is client-predicted damage, a server arbitration step, and a rejection rule
based on event timing — precisely
`DamageArbitrationDamageReceivedLateThreshold` and
`DisableDamageArbitrationRejectingClientEvents`. **The architecture in the 2025
binary is recognisably the one the 2014 post is describing the symptoms of.**
[inferred]

---

## 8. Destruction over the network

`frostbite_rendering.md` §7.5 recorded that **EA have published nothing on
destruction since 2010**. The binary fills a good deal of that gap on the
networking side. [BIN]

### 8.1 Prediction modes

```
DestructionPredictionMode_None
DestructionPredictionMode_HealthOnly
DestructionPredictionMode_Full
DestructionPredictionMode_Invalid
```

[inferred] Three real levels. `HealthOnly` is the interesting middle: the client
predicts the *damage accumulating* on a structure but not the *state transition*
— so the cracks appear immediately and the wall only actually falls when the
server says so. That is the right split, because a mispredicted "wall fell" is
catastrophic for gameplay (you take cover behind rubble that still exists on the
server) while a mispredicted health bar is invisible.

### 8.2 Transitions, rewind and correction

```
DestructionTransitionTriggerType_Networked
DestructionTransitionTriggerType_Correction
DestructionTransitionTriggerType_Rewind
DestructionTransitionTriggerType_ResetToPredicted
ReplicatedDestructionUseClientPrediction
ReplicatedDestructionHistorySize
ReplicatedDestructionDisableRewindThreshold
RewindDestructionComponent
ClientDestructionConfirmationTimer
```

[inferred] Destruction keeps a **history buffer** (`HistorySize`) and can
**rewind** through it — with a threshold past which rewinding is abandoned,
presumably because rewinding a very old or very large destruction event costs
more than it is worth. `ResetToPredicted` as a distinct trigger from `Correction`
suggests two different reconciliation paths: snap back to what we predicted, or
apply an authoritative correction.

`ClientDestructionConfirmationTimer` [inferred]: the client waits a bounded time
for the server to confirm a predicted destruction before doing something about
it.

### 8.3 Bandwidth

```
MaxNetworkedDestructionBits
DestructionNetworkingPriorityBucketSize
DestructionNetworkingPrioritySendDamageInfoThreshold
DestructionNetworkingSendDamageInfoInPredestruction
ServerPredestructionEntity
MaxReplicatedDetonations
```

[inferred] Destruction has its **own bit budget** and its own priority bucketing,
separate from the general net object priority (§3.3) — which is what you would
expect for the one system that can generate an unbounded amount of state change
in a single frame. `MaxReplicatedDetonations` caps how many explosions can be
replicated, which is a hard admission that a busy frame can exceed the budget.

---

## 9. Tick rate and the numbers we actually have

### 9.1 What is published

**60 Hz at launch.** [DEV] David Sirland, BF6 Lead Producer, answering on X
whether the tick rate would be raised, quoted by MP1st:

> "60hz is the base for now." — [DEV], reported [3P]

Reported alongside DICE "optimizing for a 60Hz tick rate, ensuring the game
server more frequently updates the positions and actions for all players". [3P]

For historical contrast, BF4 (2013) shipped at 30 Hz and later added a **High
Frequency Network Update** option with LOW/MEDIUM/HIGH levels, with community
reporting of up to 120–144 Hz on some servers. [3P] In 2014 DICE said:

> "Though we haven't got any immediate plans to increase the tickrate at this
> moment, we are exploring the possibilities of raising the tickrate on specific
> servers." — [DICE14]

**Caveat that matters:** a "tick rate" number conflates several rates that the
binary keeps separate — `TickRate`, `TargetTickRate`, `ServerTickRate`,
`ServerReceiveRate`, `UpdateFrequency`, and the client's own
`MoveManagerOutgoingFrequencyDivider`. The 60 Hz figure is a headline for the
server simulation rate; it is **not** evidence that every ghost updates at 60 Hz
(`EnableVariableGhostFrequency` says the opposite, §3.3).

### 9.2 The in-game network overlay

BF6 ships a network performance overlay, and its stat list is a clean statement
of what DICE consider the meaningful metrics: [BIN]

| stat | short label [BIN] |
|---|---|
| `NetworkPerfOverlayStat_Latency` | `Latency(ms)` |
| `NetworkPerfOverlayStat_LatencyVariation` | `LatVrtn(ms)` — jitter |
| `NetworkPerfOverlayStat_ExtraOffset` | `ExtrOff(ms)` — [inferred] the interpolation buffer / TimeNudge offset, exposed |
| `NetworkPerfOverlayStat_ServerTickRate` | `SrvTick(Hz)`, also `SrvTick(ms)` |
| `NetworkPerfOverlayStat_ServerReceiveRate` | `SrvRecv(Hz)` |
| `NetworkPerfOverlayStat_UpdateFrequency` | `TickRate(Hz)` |
| `NetworkPerfOverlayStat_ServerAverageConnectionLatency` | — |
| `NetworkPerfOverlayStat_UpStream` / `_DownStream` | — |
| `NetworkPerfOverlayStat_UpPacketLoss` / `_DownPacketLoss` | — |
| `NetworkPerfOverlayStat_IncomingNetObjects` | — |
| `NetworkPerfOverlayStat_ClientFrameTime` | — |

**`ExtraOffset` being a player-visible number is the notable one.** [inferred]
It means the interpolation delay is not hidden — a player can see how far behind
the server their view is, which is the honest way to present the trade.

> **This overlay is the cheapest possible upgrade to this document.** Turning it
> on in a live match would yield real values for tick rate, receive rate,
> interpolation offset and incoming net object counts — no capture, no injection,
> no anti-cheat exposure, because it is a shipped feature. **We have not done
> this.** Everything in §9.1 is second-hand.

### 9.3 Telemetry

`BandwidthCategoriesDataDog.yaml`, `bandwidthCategories`, `ghost.bandwidth`,
`BandwidthMax`, `BandwidthDelayMax` [BIN]. [inferred] Bandwidth is bucketed into
named categories and shipped to DataDog, with `ghost.bandwidth` as a metric name
— so DICE can attribute field bandwidth to the ghost system specifically.

---

## 10. What could not be found

Searched: EA/Frostbite/SEED publication and news indexes; the SIGGRAPH Advances
course index 2010–2026; GDC session listings; targeted web search; and the full
string dump.

| topic | status |
|---|---|
| **Any Frostbite networking engineering talk or paper** | **Appears not to exist.** EA have published FrameGraph, PBR, volumetrics, GI, Serac and physics work, but no networking architecture talk. The only DICE netcode communication located is [DICE14], a community post with no technical content. **This is the largest confirmed absence of the two documents.** |
| **Wire format, packet layout, header structure** | **Not recoverable from strings.** Would need traffic capture. |
| **Every budget value** — `MaxNetObjectCount`, `MaxGhostCount`, `MaxNetworkedDestructionBits`, `TimeNudgeMax`, thresholds | **Names only.** These live in data (`.cas` archives), not the executable. |
| **What "Jotunheim" interpolation actually is** | **Name only.** No EA source mentions it. |
| **Server topology** — regions, instance counts, host hardware, whether Blaze or a separate orchestrator places matches | **Not addressed.** Blaze `GameManager` types are visible but say nothing about deployment. |
| **Rollback/resimulation depth for player movement** | **Ambiguous.** `Rollback` appears once and `Rewind` only in destruction and physics contexts. No `MoveManager` symbols beyond `MoveManagerOutgoingFrequencyDivider` survive, so the movement prediction loop itself is not legible. |
| **Actual measured netcode performance** | **Not measured by us.** §9.2 is the route and it was not taken. |
| **BF6 REDSEC / battle royale netcode differences** | **Not investigated.** The mode exists; whether it changes tick rate or relevance is unknown. |

---

## 11. What is worth taking for this project

Filtered for an engine that is currently single-player but whose stated future
includes RTS, FPS and third-person projects (CLAUDE.md). Ranked by how expensive
each becomes if deferred.

1. **Draw the realm boundary now, and let the build check it.** [BIN] §2 This is
   the same class of rule as CLAUDE.md's `cromwell` may-not-name-`game/`
   constraint, and it has the same property: **cheap to establish, ruinous to
   retrofit.** Once thousands of call sites exist, "does this run on the server?"
   is unanswerable. Even with no networking, tagging simulation vs presentation
   (`DiceChannelRealm_Simulation` / `_Presentation`) pays immediately — it is the
   line that stops rendering state leaking into gameplay decisions.

2. **Separate "what is authoritative" from "what direction it replicates".**
   [BIN] §2 Frostbite keeps `RealmEx_*` (where code lives) and
   `RealmReplication_*` (which way data flows) as two axes. Collapsing them into
   one enum is the obvious first design and it fails as soon as one entity needs
   client-authoritative aim and server-authoritative damage.

3. **Ship the derivative, not just the value.** [BIN] §4.3 `Extrapolatable*`
   types carrying explicit velocity is a small decision with a large payoff: the
   receiver extrapolates correctly through a dropped update instead of guessing
   from position history.

4. **Quantise per field, declared in data, with range assertions.** [BIN] §4.1–4.2
   `MaxBits` + `Scale` per field, and a packer that *asserts* on out-of-range
   rather than silently wrapping. The error strings show DICE chose loud failure,
   and `NetError_Object_BitCountMismatch` shows the two ends are required to
   agree exactly.

5. **Nudge, never teleport — with a bounded speed.** [BIN] §5.2 `MaximumNudge
   Speed` is the one knob that decides whether corrections read as "responsive"
   or "rubber-banding". Worth building in from the first correction.

6. **Predict the cheap half, confirm the expensive half.** [BIN] §8.1
   `DestructionPredictionMode_HealthOnly` is the generalisable idea: predict the
   part whose misprediction is invisible (accumulating damage), wait for
   authority on the part whose misprediction changes the game (the wall falling).
   Applies to any state machine with a visible progress term and a discrete
   commit.

7. **Ship validation in report-only mode first.** [BIN] §7.1
   `ServerAuthoritativeBulletDamageMismatchReportOnly` plus a separate
   `ReportThreshold` is how you deploy an authority change without breaking the
   game on day one: measure disagreement in the field, then start enforcing.
   This is the same instinct as CLAUDE.md's "test derived data against its
   source".

8. **Give the budget an overflow path, not an assert.** [BIN] §3.1
   `NetObjectOverflow` and `LargeNetObjectTrackerMode_Warning` / `_Assert` /
   `_Fatal` — a configurable severity, so the same tripwire is fatal in
   development and a warning in retail.

9. **Group the many small things.** [BIN] §3.2 Hundreds of doors do not each get
   a net object. If a future project has many identical interactables, group them
   before they become the bandwidth.

10. **What not to copy:** the whole stack. DirtySDK and Blaze are EA
    infrastructure; the ghost system assumes dedicated servers, a matchmaking
    backend and an anti-cheat posture none of which apply here. The *vocabulary*
    and the *decisions* transfer; the architecture is sized for a problem this
    project does not have.

---

## 12. Provenance

- [BIN] evidence read from `bf6.exe`, 195,618,664 bytes, file-stamped
  **2025-08-11**, retail Steam install, on **2026-08-13**. Same dump as
  `frostbite_rendering.md`; no second read of the binary was needed.
- Categorised networking extract — 1,643 lines, thirteen sections — in
  [`battlefield6_net_strings.txt`](battlefield6_net_strings.txt). Cosmetic-name
  noise filtered; the two senses of "ghost" (§0.2) kept separate.
- [DICE14] retrieved from battlefield.com and read in full.
- **No packet capture, no traffic observation, no process injection.** The
  in-game network overlay (§9.2) was **not** used, and is the obvious next step.
