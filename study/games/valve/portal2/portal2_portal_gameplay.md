# Portal 2 — the portal as a gameplay object

Placement, teleportation, and the part nobody sees: making a hole in a wall that
*physics* believes in. The rendering half is
[`portal2_portal_rendering.md`](portal2_portal_rendering.md); the paint is
[`portal2_gels.md`](portal2_gels.md).

The thesis in one line: **rendering a portal is a week's work and simulating one
is a year's**, and the reason is that a renderer only has to answer "what does
the camera see", whereas the simulation has to answer "what does a box resting
half in the wall collide with" — for a wall that is solid, a hole that is not,
and a second room that is somewhere else entirely.

---

## 0. Provenance

Same tagging as the rendering note, and the same governing caveat: the code is
**Valve's Portal 1 implementation** read from a community tree
([RubberWar/Portal-2](https://github.com/RubberWar/Portal-2)), tagged
**[PORTAL-SRC]**; Portal 2's own code is not public. Portal 2-specific claims come
from the in-game developer commentary **[VALVE-COMMENTARY]**, from entity and cvar
documentation **[VDC]**, or from **[P2-RETAIL]** — the shipping game installed at
`E:\SteamLibrary\steamapps\common\Portal 2`, with RTTI class names, cvar names and
help strings mined out of `portal2\bin\{client,server}.dll`. §10 is the
verification pass; where it corrects something, the correction is inline.
**[inferred]** is our reading.

---

## 1. Dimensions and the transform

**[PORTAL-SRC]** **[VDC]**

```cpp
#define PORTAL_HALF_WIDTH   32       // 64 × 108 units of hole
#define PORTAL_HALF_HEIGHT  54
#define PORTAL_HALF_DEPTH   2.0f
#define PORTAL_BUMP_FORGIVENESS 2.0f
const Vector vLocalMins( 0.0f, -PORTAL_HALF_WIDTH, -PORTAL_HALF_HEIGHT );
const Vector vLocalMaxs( 64.0f, PORTAL_HALF_WIDTH,  PORTAL_HALF_HEIGHT );
```

In Portal 2 these became keyvalues (`HalfWidth` / `HalfHeight`, defaulting to
32 / 54) plus a `Resize` input, with the caveat that *"portals only function if
they are the same size"* **[VDC]**. The player is 32 units wide and 72 tall, so a
portal is a hair over one and a half player-widths and one and a half
player-heights — it always reads as generous.

Note the *bounds*: 64 units **deep**. The entity's box extends a full portal-height
in front of the plane. That is the touch volume that drives teleportation (§3),
not the visual surface.

The link is one matrix, rebuilt whenever either portal moves:

```cpp
// world → this portal → 180° about up → linked portal → world
*pMatrix = remoteToWorld * Rot180AboutUp * inverse( localToWorld );
```

Everything — position, angles, velocity, the exit camera, the physics clones,
the ghost renderables, the blob streams — is that one matrix applied to a
different noun.

## 2. Placement

`VerifyPortalPlacement()` returns a **float**, not a bool. **[PORTAL-SRC]**

```cpp
#define PORTAL_ANALOG_SUCCESS_NO_BUMP              1.0f
#define PORTAL_ANALOG_SUCCESS_BUMPED               0.3f
#define PORTAL_ANALOG_SUCCESS_CANT_FIT             0.1f
#define PORTAL_ANALOG_SUCCESS_CLEANSER             0.028f
#define PORTAL_ANALOG_SUCCESS_OVERLAP_LINKED       0.027f
#define PORTAL_ANALOG_SUCCESS_NEAR                 0.0265f
#define PORTAL_ANALOG_SUCCESS_INVALID_VOLUME       0.026f
#define PORTAL_ANALOG_SUCCESS_INVALID_SURFACE      0.025f
#define PORTAL_ANALOG_SUCCESS_PASSTHROUGH_SURFACE  0.0f
```

**Failure is graded.** Those values below 0.03 are not "false" — they are a
ranking of *how nearly this worked*, and they exist because the console
controller's placement assist needs to pick the best candidate among several
aim-adjusted traces. "Can't fit" beats "wrong material" beats "sky". This is
CLAUDE.md's *sort before the expensive test* rule expressed as a return type: the
scorer hands back a score, the caller picks the winner.

The checks, in order:

1. **Trace along the portal's forward axis** (`vOrigin + vForward` →
   `vOrigin - vForward`, mask `MASK_SHOT_PORTAL`). Nothing behind the centre →
   invalid.
2. **Is the surface moving?** Any linear or angular velocity on the hit entity →
   invalid. A portal on a moving surface is not a rendering problem, it is a
   *simulation* problem — the carved collision (§5) would have to be rebuilt
   every tick.
3. **Material.** `IsPassThroughMaterial` — `SURF_SKY` or a name in the
   pass-through list — means the shot continues. `IsNoPortalMaterial` —
   `SURF_NOPORTAL`, surface property `CHAR_TEX_GLASS`, or a name beginning
   `**studio**` — means fail. **Portalability is a property of the *surface
   material*, not of the brush**, which is why level artists control it by
   texture choice, and why *models are never portalable*: that `**studio**`
   prefix test is a one-line, total exclusion of every prop in the game.
4. **Bumping.** `FitPortalAroundOtherPortals` first (shove away from the linked
   portal so a pair can sit side by side), then `FitPortalOnSurface`, which
   traces the four corners — inset by `PORTAL_BUMP_FORGIVENESS` — and slides the
   portal off edges and obstructions. Reject if it slid further than
   `MAXIMUM_BUMP_DISTANCE` = `((2w)² + (2h)²)/2`, i.e. half the diagonal squared.
5. **Volumes.** `func_noportal_volume` and overlap with existing portals.

Two design consequences worth extracting **[inferred]**:

- **Bumping is why Portal feels forgiving.** The player aims at a corner and the
  portal *slides* to fit rather than refusing. The forgiveness is metered
  (`PORTAL_BUMP_FORGIVENESS 2.0f` on the corner traces, a hard cap on total
  travel) so it never teleports the portal somewhere you did not mean.
- **`sv_portal_placement_never_bump` and `sv_portal_placement_never_fail` are
  both shipped cheats.** The permissive and strict versions of the rule are one
  cvar apart, which is exactly how you playtest a feel rule.

Portal 2 added a `prop_portal` placement contract for mappers that reads like the
runtime rules made static: **must not start active**, pitch/yaw/roll exactly
matching the surface, 0–8 units off it, centre at least **57 units** from
adjacent surfaces top and bottom and **32** at the sides — and bad placement
silently relocates the portal to the map origin. **[VDC]**

## 3. Teleportation: when

Two conditions, both required, evaluated on touch **[PORTAL-SRC]**:

```cpp
// 1. the entity's centre has crossed the plane
PortalPlane.m_Normal.Dot( pOther->WorldSpaceCenter() ) < PortalPlane.m_Dist
// 2. …and it is actually in the hole, not merely near the portal
&& m_PortalSimulator.EntityIsInPortalHole( pOther )
```

**Centre of mass, not the leading edge and not the eye.** You are teleported when
half of you is through. This is the single most consequential feel decision in
the game and it is one line: it means the transition happens when your body is
symmetric about the portal, so the camera translation is small and the world
appears continuous. It is also why the *ghosts* (§7) are mandatory rather than
polish — for the entire time your centre has not crossed, part of you is
sticking out of the other portal and must be drawn there.

`EntityIsInPortalHole` tests against a dedicated collideable,
`pHoleShapeCollideable`, which the header warns *"should NOT be collided against
in general"* — a separate, cheap shape whose only job is answering this question.
There is a ray version too, `RayIsInPortalHole`, whose bias is documented as
*"towards false positives"*.

Non-teleportables are a hard-coded list, checked up the whole `GetMoveParent`
chain: `func_door`, `func_door_rotating`, `prop_door_rotating`,
`func_tracktrain`, and `physicsshadowclone` (§6 — a clone must never teleport,
or you would get clones of clones).

## 4. Teleportation: what happens

`TeleportTouchingEntity()`, in order **[PORTAL-SRC]**:

**Position.** For non-players, `origin' = M · origin`. For players,
`origin' = M · centre + (origin - centre)` — the centre is transformed and the
origin is reconstructed from it, because the player's origin is at their feet and
transforming *that* through a floor-to-ceiling portal puts them in the ceiling.

**Angles** via `TransformAnglesToWorldSpace`, then normalised positive.
**Velocity** via `M.ApplyRotation()` — rotation only, obviously.

**The axis-aligned bounding box problem.** Player hulls are AABBs and AABBs do
not rotate; a portal pair almost never is axis-aligned. Portal 1's answer is
blunt and in the code:

```cpp
//curl the player up into a little ball
pOtherAsPlayer->SetGroundEntity( NULL );
if( !pOtherAsPlayer->IsDucked() ) {
    pOtherAsPlayer->ForceDuckThisFrame();
    ptOtherCenter.z += (portal faces up) ? -16.0f : +16.0f;   //shrink toward the portal
}
```

— unless both portals are exactly floor/ceiling aligned, in which case no shrink
is needed. Portal 2 replaced this. From the commentary, Dave Kircher:
**[VALVE-COMMENTARY]**

> The player is represented as an axis-aligned box in the world, which creates a
> problem for portal teleportation because portal teleportation is almost never
> axis-aligned. To improve how we handle this, **we trace the player as the
> axis-aligned bounding box they would use on each side of a portal
> simultaneously and merge the results into something usable.**

That is the Portal 1 → Portal 2 delta in one sentence: stop deforming the player
to fit one hull, and instead run *both* hulls — the one you have here and the one
you would have there — through the same movement trace, and merge.

It shipped as a switch. `server.dll` carries **`sv_portal_new_player_trace`** and
**`sv_portal_new_player_trace_vs_remote_ents`**, plus `sv_portal_new_trace_debugboxes`
— so the dual-hull trace is a *replacement* for the old one that could be turned
off, and the "remote ents" variant is the second half of the problem: the merged
trace has to hit the far room's entities too. **[P2-RETAIL]**

**Camera reorientation** is a heuristic, and it is the reason walking through a
floor portal does not make you seasick. The player can be re-oriented by *roll*
or by *pitch*, and there is an explicit decision tree:

| Condition | Reorient by |
|---|---|
| holding an object | **roll** — *"never pitch reorient while holding an object"* |
| entering a wall portal while facing straight up/down, or mostly toward/away from it | pitch |
| floor↔ceiling portals while facing mostly up or down | pitch |
| exiting a "wedge" portal (forward.z 0.75–0.99) while facing its top | pitch |
| otherwise | roll |

The client is told about the teleport explicitly — `m_bFixEyeAnglesFromPortalling`
plus `m_qPrePortalledViewAngles` and `m_matLastPortalled` — so that prediction and
mouse input reconcile across the discontinuity rather than snapping.

**Velocity rules** — the "fling" tuning, and it is four numbers:

```cpp
#define MINIMUM_FLOOR_PORTAL_EXIT_VELOCITY          50.0f    // objects, out of a floor portal
#define MINIMUM_FLOOR_TO_FLOOR_PORTAL_EXIT_VELOCITY 225.0f   // objects, floor → floor
#define MINIMUM_FLOOR_PORTAL_EXIT_VELOCITY_PLAYER  300.0f    // the player, out of a floor portal
#define MAXIMUM_PORTAL_EXIT_VELOCITY              1000.0f
```

Valve's own name for that block is `//velocity hacks`, and they were still trying
to be rid of it in 2011: Portal 2 ships **`sv_portal_unified_velocity`**, whose
help string is *"An attempt at removing patchwork velocity tranformation in
portals, moving to a unified approach"* (Valve's typo). **[P2-RETAIL]** Take that
as the honest epilogue to this section — the numbers below are a patchwork, the
people who wrote them knew it, and they shipped twice anyway because the patchwork
feels right.

Exiting *upward* out of a floor portal, your vertical velocity is **raised** to a
floor value if it is below it — so a portal in the ground always pops you out
rather than dribbling you back in, and floor-to-floor always clears the lip.
Everything is then **clamped to 1000 u/s** total. "Speedy thing goes in, speedy
thing comes out" is true up to 1000 units per second and no further, and the
lower bound is why it never quite feels like it fails.

Finally the entity is handed between simulators — `ReleaseOwnershipOfEntity` on
this side, `TakeOwnershipOfEntity` on the other, with an explicit un-touch so the
destination re-touches cleanly next tick.

## 5. The hole in the wall: `CPortalSimulator`

This is the year of work. **[PORTAL-SRC]**

A portal needs collision geometry that no map contains: a wall with a
portal-shaped hole in it, *plus* the far room's floor, arriving through that hole,
in local coordinates. Source builds both, by CSG, every time a portal moves.

`CreatePolyhedrons()` carves the neighbourhood into convex polyhedra:

**The "World"** — everything in *front* of the portal plane, within an oriented
box:

```cpp
#define PORTAL_COLLISION_SIM_BOUNDS_X  200 * (PORTAL_HALF_WIDTH / 32)    // sv_portal_collision_sim_bounds_x
#define PORTAL_COLLISION_SIM_BOUNDS_Y  200 * (PORTAL_HALF_HEIGHT / 54)
#define PORTAL_COLLISION_SIM_BOUNDS_Z  (72 + PORTAL_HALF_HEIGHT) * 2     // = 252
```

with the sizing rationale written down: *"default size for scale z (252) is
player (height + portal half height) × 2. Any smaller than this will allow for
players to reach unsimulated geometry before an end touch with the portal."*
World brushes and **static props** in that box are clipped into polyhedra and
turned into a `CPhysCollide`.

**The "(Holy) Wall"** — everything *behind* the plane, cut into four convex
chunks around the hole: upper, lower, left, right. Brushes that do not touch the
hole are skipped entirely (*"no part of this brush interacts with the hole, no
point in cutting the brush"*), and brushes that pass through it cannot be
optimised out.

**The Tube** — a 1-unit-deep sleeve through the hole, described in the header as
*"a minimal tube, an object must fit inside this to be eligible for portaling"*.
If VPhysics simulation is off for this portal, **only** the tube is built and the
wall is skipped — the cheap configuration for a portal that only ever has to
carry the player.

**RemoteTransformedToLocal** — the linked portal's *World* collision, transformed
through the link matrix into this portal's space, so an object half-through
collides with the far room's geometry in the near room's physics environment.

Numbers that matter: cuts use an epsilon of `1 / 2^40`; world and wall collision
are separated by 0.1 units because *"separating the world collision from wall
collision by a small amount gets rid of extremely thin erroneous collision at the
separating plane"*; the hole is 0.1 units larger than the portal in each
direction. There is a `StaticCollisionPolyhedronCache` because carving static
props is expensive and they never move.

The header carries the best comment in the codebase, and it is about *naming*:

> You may be wondering... why? wtf? The answer. The previous incarnation of
> server side portal simulation suffered terribly from evolving variables with
> increasingly cryptic names with no clear definition of what part of the system
> the variable was involved with. It's my hope that a nested structure with clear
> boundaries will eliminate that horrible, awful, nasty, frustrating confusion.
> (It was really really bad).

Hence `m_InternalData.Simulation.Static.Wall.RemoteTransformedToLocal.Brushes`.
It is verbose on purpose, and the purpose is that *every* name states which half
of the mirror it belongs to. There is also a field named
`m_bSharedCollisionConfiguration` whose comment ends *"For the love of all that
is holy, pray that this is false."*

## 6. Shadow clones

Each portal owns **its own `IPhysicsEnvironment`**. An object near a portal
therefore has to exist in two simulations at once, and Valve solve it with
`CPhysicsShadowClone` — *"Clones a physics object by use of shadows"*:

- A clone holds an `EHANDLE` to its source, a transform matrix and its inverse,
  and a list of `PhysicsObjectCloneLink_t { pSource, pShadowController, pClone }`.
- Every physics frame it syncs position/velocity through the matrix. Shadow
  controllers are VPhysics' existing mechanism for "this object is driven from
  outside", so a clone is not a special case in the solver — it is a keyframed
  object that happens to be keyframed by another object.
- `m_bImmovable` exists for *"cloning a track train or door, something that
  doesn't really work on a force-based level"*.
- Clones carry `FVPHYSICS_IS_SHADOWCLONE` and are on the never-teleport list.

Cloning is scoped by a trigger volume rather than being global.
`CPhysicsCloneArea` — *"Instead of cloning all physics objects in a level to get
proper near-portal reactions, only clone from a larger area near portals"* — is a
box `PHYSICSCLONEAREASCALE 4.0f` times the portal, whose `StartTouch`/`EndTouch`
call `StartCloningEntity`/`StopCloningEntity`. **The set of duplicated objects is
maintained by the ordinary trigger system**, which is the cheapest possible answer
and needs no per-frame scan.

Per-entity state is a bitfield indexed by entity index —
`unsigned int EntFlags[MAX_EDICTS]` — with `PSEF_OWNS_ENTITY`,
`PSEF_OWNS_PHYSICS`, `PSEF_IS_IN_PORTAL_HOLE` (*"updated per-phyframe"*) and
`PSEF_CLONES_ENTITY_FROM_MAIN`. A flat array indexed by index, one word per
entity, updated per physics frame: exactly the data layout CLAUDE.md asks for in
the query layer, in 2007 Valve code.

## 7. Ghosts: seeing the half of you that is elsewhere

`C_PortalGhostRenderable` — a client-only, non-networked renderable that clones
another entity's *appearance* through the portal matrix **[PORTAL-SRC]**:

```cpp
SetModelName ( source->GetModelName() );
SetModelIndex( source->GetModelIndex() );
SetEffects   ( source->GetEffects() | EF_NOINTERP );
if ( source is CBaseAnimating ) { SetCycle(); SetSequence(); m_nBody; m_nSkin; }
```

It is added to the client leaf system as opaque or translucent to match, and
shares one **render clip plane** with the portal so the ghost and the original are
cut on the same plane with no double-drawn sliver. There is a flag for the local
player, because your own body has to be ghosted too — the thing you see when you
look down while standing half in a portal.

Note what it does *not* do: it does not re-simulate, re-animate or re-light. It
copies the model, the sequence and the cycle each frame and draws the same pose
somewhere else. **A ghost is a view of an entity, not a second entity** — which
is exactly the split that made the physics side hard, and exactly why the render
side is 362 lines and the physics side is 3,337.

## 8. Prediction

Kircher again, and this is the honest bit **[VALVE-COMMENTARY]**:

> We have to predict quite a bit more than previous Source Engine games because
> portals and projected entities change the way the player moves through the
> world. Prediction itself is a mind bending headache when dealing with portals.
> We're already dealing with a non-linear space. Now we also have to deal with
> **non-linear time in a non-linear space**.

Client-side prediction re-runs movement for the same tick after a correction. If
a portal transition happens inside the re-run, the entire coordinate frame of the
prediction changes — hence the explicit `m_matLastPortalled` handshake in §4, and
hence a client-side `portal_demohack` cvar to make demo playback survive going
through a portal at all.

**Portal 2's answer is to stop inferring the teleport and start sending it.**
`client.dll` carries a networked type `DT_EntityPortalledNetworkMessage`, and the
player's networked state includes `m_EntityPortalledNetworkMessages` and
`m_iEntityPortalledNetworkMessageCount`, with the message's own fields including
`m_bFromPortal` / `m_bToPortal`. **[P2-RETAIL]** So a teleport is not something
the client works out from a position that jumped — **the server states it, as a
message, in a queue, with the two portals named.** That is exactly the shift
[`../valve_networking.md`](../valve_networking.md) identifies as the Source 1 →
Source 2 change ("the server *infers* what the client saw" → "the client *states*
it"), arriving early and in one specific place, because portals are where
inference fails hardest.

Beside it, `cl_portal_teleportation_interpolation_fixup_method` (§10) decides what
the *interpolator* does with the discontinuity once it has been told about it.
Two separate mechanisms — one to know a teleport happened, one to stop it
poisoning the interpolation history — where Portal 1 had a matrix and a bool.

Two more pieces of Portal 2 networked state are worth naming because they show
which cases needed explicit handling: `m_hPortalEnvironment` (which portal
simulator the player is currently inside — the §5 environment, on the wire), and
`m_bHeldObjectOnOppositeSideOfPortal` with `m_hHeldObjectPortal` (**carrying a cube
through a portal is its own replicated state**, not an emergent consequence of the
cube's position). Compare [`../valve_networking.md`](../valve_networking.md)
§7.3 on lag compensation: the same class of problem, one dimension worse.

## 9. What transfers here

This is a turn-based tile game. It will never have a portal gun. Five things
transfer anyway:

**1. Graded failure.** §2's analog success codes. Any "can I place this here"
query — cover placement, ability targeting, building a wall — is better as a
*score* than a bool, because the caller almost always wants the best of several
candidates and the UI wants to say *why* the bad ones are bad. We have this
shape already in the dev placement tools; the lesson is to return the ranking
rather than re-deriving it.

**2. Bump-to-fit is a feel feature with a hard cap.** Slide the placement to
make it legal, refuse if it slid too far. The cap is what stops forgiveness from
becoming teleportation.

**3. Duplicate by trigger volume, not by scan.** §6 — the set of objects needing
special treatment near a special place is maintained by the entity system's own
enter/exit events. Our equivalents (units near a destructible, entities inside an
effect volume) should be maintained on the boundary, not swept per frame. That is
the same rule as the occlusion grid's *invalidate at the boundary that owns the
data*.

**4. Name every field for which side of the mirror it is on.** §5's nested
structure is over-engineered right up until you have two coordinate systems that
look identical, and then it is the only thing keeping the code alive. Our
world/grid/screen space conversions are one such pair.

**5. Teleport on the centre.** If we ever move a unit discontinuously — a
transport, a dropship, a lift — the discontinuity should happen when the thing is
*symmetric* about the boundary, and something must draw it on both sides until it
is not.

---

## 10. Verified against the retail install

Read 2026-08-15 from `portal2\bin\{client,server}.dll` — RTTI names, cvar names
and adjacent help strings. **[P2-RETAIL]**

### Confirmed

`CPortalSimulator`, `CPortalSimulatorEventCallbacks`, `CPortalCollideableEnumerator`,
`CPortalGameMovement`, `CPortalPlayerShared`, `C_PortalGhostRenderable` and
`CPSCollisionEntity` are all in the 2011 binaries under the same names as §5–§7.

**The "(Holy) Wall" shipped, and it is in a cvar name.** Portal 2 exposes the
carved geometry as four independent trace toggles:

| cvar | help string |
|---|---|
| `sv_portal_trace_vs_holywall` | *"Use traces against portal environment carved wall"* |
| `sv_portal_trace_vs_staticprops` | *"Use traces against portal environment static prop geometry"* |
| `sv_portal_trace_vs_world` | — |
| `sv_portal_trace_vs_displacements` | — |

That last one is new: Portal 1 could not carve displacements, and Portal 2 ships
`portal_clone_displacements` and `portal_carve_vphysics_clips` alongside it. The
static-prop carving cache of §5 is also there and switchable, as
`sv_portal_staticcollisioncache_cachebrushes` and `…_cachestaticprops`.

`sv_portal_placement_never_bump`, `sv_portal_placement_never_fail`,
`sv_portal_placement_debug`, `sv_portal_debug_touch` and
`sv_portal_new_velocity_check` are all present, i.e. §2 and §3 are the shipped
code paths.

### New in Portal 2

**The camera reorientation of §4 became animated.** Portal 1 snapped the roll or
pitch correction; Portal 2 has `cl_portal_camera_orientation_rate` and
`…_rate_base` (**45.0**), `…_max_speed`, and `…_acceleration_rate` (**1000.0**).
A rate, a base rate, a cap and an acceleration is a small motion controller, and
it is the difference between "the world flips" and "the world rolls upright".

**Prediction got an explicit fixup method.**
`cl_portal_teleportation_interpolation_fixup_method`, help string *"0 = transform
history only, 1 = insert discontinuity transform"*. That is §8's problem stated
precisely: when an entity's interpolation history spans a teleport you can either
**rewrite the history into the new frame** or **record the discontinuity and let
the interpolator skip it** — and Valve shipped both and made it selectable.

**The portal is now `CPortal_Base2D`**, shared with `CLinkedPortalDoor` and
`CPropLinkedPortalDoor`, and `CPortalSimulator` appears as a networked member of
it. Other additions worth knowing exist: `sv_portal_high_speed_physics_early_untouch`
(the fast-object case §3's touch logic can miss), `portal_max_separation_force`,
`portal_player_interaction_quadtest_epsilon` (`-0.03125`),
`portal_trace_shrink_ray_each_query`, `sv_portals_block_other_players`, and
`portal_environment_radius`.

### Not verified

Shipped defaults for anything (the strings give names and help text, not reliably
the values), and whether the teleport condition is still centre-crossing in 2011.
Those need a running game with a console dump.

---

## Sources

- **Portal client/server/shared code** (Valve's, via a community tree):
  [RubberWar/Portal-2](https://github.com/RubberWar/Portal-2) —
  `src/game/shared/portal/{PortalSimulation.h,PortalSimulation.cpp,prop_portal_shared.cpp,portal_shareddefs.h}`,
  `src/game/server/portal/{prop_portal.cpp,portal_placement.cpp,physicsshadowclone.h,PhysicsCloneArea.cpp}`,
  `src/game/client/portal/C_PortalGhostRenderable.cpp`.
- **Portal 2 developer commentary**:
  [theportalwiki.com](https://theportalwiki.com/wiki/Portal_2_developer_commentary)
  — "Predicting With Portals" (Dave Kircher).
- **Valve Developer Community**:
  [prop_portal](https://developer.valvesoftware.com/wiki/Prop_portal),
  [func_noportal_volume](https://developer.valvesoftware.com/wiki/Func_noportal_volume),
  [Portal 2 engine branch](https://developer.valvesoftware.com/wiki/Portal_2_engine_branch).
- **The retail install**, `E:\SteamLibrary\steamapps\common\Portal 2\portal2\bin\{client,server}.dll`.
  See §10.
