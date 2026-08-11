# The XCOM 2 tactical camera — architecture and every shipped number

How XCOM 2 decides **which camera runs**, **where a shot camera goes**, **how it
keeps the shooter and target framed**, **why you never end up staring at a
wall**, and **how it follows a moving unit** — plus the tuning values behind the
tactical view: field of view, pitch, yaw, zoom distances and interpolation
rates. Written to be copied into a prototype, so **every number here is the
shipped value, verbatim**, and anything derived is marked as derived.

**[FIRAXIS]** is transcribed from Firaxis' own shipped source or data.
**[derived]** is arithmetic on the shipped values. **[inferred]** is reasoning
about native C++ that is not published — flagged wherever a function is
`native` and only its script-side callers and config are visible.

> **Where this came from.** The primary source is the **War of the Chosen SDK
> installed on this machine** — `E:\SteamLibrary\steamapps\common\XCOM 2 War of
> the Chosen SDK\`, which ships Firaxis' full UnrealScript tree
> (`Development\SrcOrig\XComGame\Classes\`, comments intact, headers signed
> `AUTHOR: David Burchanowski -- 2/10/2014`) and the shipped defaults
> (`XComGame\Config\DefaultCamera.ini`). The base-game SDK is installed
> alongside it. An earlier revision of this note worked from the community
> Highlander's copy of `X2Camera_LookAt.uc` and the
> [`russgray/xcom2-config`](https://github.com/russgray/xcom2-config) dump of
> the **launch** ini (stamp 4 Feb 2016); that dump is kept as the "launch"
> column in §10, and every value it contained agrees with the SDK except the
> handful §10 lists as deliberate retunes. The one thing still unreadable is
> the native C++ behind the `native` function declarations.

---

## 1. The answer, before the detail

There is no single camera and no single FOV. XCOM 2 drives a **camera stack**,
and each camera on it carries its own field of view. Shipped values: [FIRAXIS]

| Camera | FOV | Where |
|---|---|---|
| Tactical game cam, **player turn** | **60°** | `X2Camera_LookAt.HumanTurnFov` |
| Tactical game cam, **alien turn** | **55°** | `X2Camera_LookAt.AlienTurnFOV` |
| Rush cam (chases a dashing unit) | **45°** | `X2Camera_RushCam.FOVInDegrees` |
| Shot cameras (OTS, matinees) | authored per shot | the matinee content |
| Midpoint (frames a group) | 60° × **0.75 framing** | `X2Camera_Midpoint.FramingFOVPercentage` |

If you want one number to start a prototype with, it is **60**.

These are **horizontal** FOV at 16:9, because `AspectRatioAxisConstraint` ships
as `AspectRatio_MaintainXFOV` — the horizontal angle is held and the vertical
one falls out of the aspect ratio. [FIRAXIS] The vertical equivalents: [derived]

| Horizontal | Vertical at 16:9 |
|---|---|
| 60° | 35.98° |
| 55° | 31.99° |
| 45° | 25.61° |

The consequence is **Vert−**: an ultrawide monitor gets *less* vertical view,
not more horizontal. This is why the standing modder advice is to flip the
constraint to `AspectRatio_MaintainYFOV` on 16:9 and wider — the shipped
default holds the wrong axis for a wide display. [COMMUNITY]

And the four mechanism answers, before the detail:

- **Where cameras go**: dramatic cameras are not solved, they are **authored as
  a library of matinee shots and the game picks the least-penalised one**, by
  tracing rays at the shooter's head, the shooter's waist, the target, and the
  camera's own start position (§5).
- **How the shooter and target stay in frame**: each authored shot carries
  marker spheres for where the two heads should sit on screen; at runtime the
  camera re-aims so the *actual* heads land on those screen positions (§5.1).
  Group shots solve camera-from-framing analytically instead (§6).
- **Why you never stare at a wall**: two independent systems. Shot cameras
  *avoid* walls by scoring; the tactical camera makes the world *yield* — 
  floors hide, walls cut down, and occluders dither, driven by focus points
  every camera publishes (§7).
- **How it follows a mover**: the tactical camera leads the unit along its
  path with rate-limited interpolation and skips moves that are already on
  screen; the dash cam chases behind and slides around obstacles by searching
  yaw offsets (§4).

---

## 2. The camera stack — who gets the camera

`X2CameraStack` owns everything. The header comment states the design contract
outright: [FIRAXIS] `X2CameraStack.uc:10`

> The camera stack keeps track of and arbitrates all in-flight camera requests
> from the game. […] Note that any number of cameras may be on the stack. DO
> NOT attempt to directly interact with or interfere with cameras added by
> other game systems! […] Example: Let's assume that three different aliens
> are being shot at in a replay at the same time a gas tank explodes. Four
> cameras should be created, and the appropriate priorities set. […] None of
> these systems are aware that the other cameras even exist, nor do they need
> to care.

So a game system never *moves* the camera; it **adds a camera object and
removes it when done**, and the stack arbitrates. The arbitration is two
rules:

**Priority is an enum whose ordinal is the priority.** [FIRAXIS] `X2Camera.uc:57`

```unrealscript
enum ECameraPriority
{
    eCameraPriority_Default,            // exclusively for the cursor-following camera
    eCameraPriority_EnemyHeadLookat,    // hovering a UI alien head
    eCameraPriority_CharacterMovementAndFraming, // units moving to and fro
    eCameraPriority_LookAt,
    eCameraPriority_GameActions,        // explosions "and other sundry things"
    eCameraPriority_Kismet,
    eCameraPriority_Cinematic,          // matinees, full-screen happenings
};
```

Insertion keeps the array sorted by priority; the active camera is the highest
one that does not **yield**. `YieldIfActive()` is the small idea worth
stealing: a camera can sit on the stack *waiting* — added early so its place
in line is reserved, declining focus until its subject is ready.

**Any camera can push a child camera on itself.** The rush cam pushes a
ladder camera when its unit starts climbing (§4.3) and gets control back
automatically when the child pops. Complex sequences compose from simple
cameras instead of one camera growing states.

### 2.1 Blending

When the active camera changes, the stack blends **from a snapshot of the old
camera's TPOV to the live new camera** over the *incoming* camera's
`BlendDuration` (default **2 s**; the OTS targeting cam overrides to **0.75 s**
— "snappier"). Location lerps, rotation `RLerp`s, FOV lerps, all through one
config curve: [FIRAXIS] `DefaultCamera.ini`

```ini
[XComGame.X2CameraStack]
BlendCurve=(Points=(  (InVal=0,     OutVal=0),
                      (InVal=0.24f, OutVal=0.5f,  ArriveTangent=2.289),
                      (InVal=0.6f,  OutVal=0.95f, ArriveTangent=0.281),
                      (InVal=1,     OutVal=1)))
```

Read it as: **half the move in the first quarter of the time, 95% by 60%, and
a long slow settle.** The incoming camera decides *whether* to blend at all
via `ShouldBlendFromCamera()` — most cuts in Cinescript are hard cuts, and
blends are reserved for transitions the player should read as one continuous
move (game cam → targeting cam, targeting cam → firing cam).

### 2.2 The stack pauses garbage collection while the camera moves

`GetCameraLocationAndOrientation()` compares this frame's location to last
frame's; if the camera moved more than 0.01 uu it calls `XENGINE.PauseGC()`,
and unpauses when it stops. [FIRAXIS] `X2CameraStack.uc:117` A GC hitch is
invisible on a static frame and glaring mid-pan, so the engine simply never
collects while the camera is in flight. The camera, of all things, is the
system that knows when a hitch would be seen.

---

## 3. The tactical game camera — `X2Camera_LookAt`

This is the camera you play the game through. It looks at a point (the 3D
cursor, or a unit) from a fixed orientation and a zoom-controlled distance.
The whole shipped section, verbatim: [FIRAXIS]

```ini
[XComGame.X2Camera_LookAt]
MaximumInterpolationDistancePerSecond=3000	; in unreal units per second.  The fastest the game cam is allowed to translate.
MaximumInterpolationRotationPerSecond=400	; in degrees per second.  The fastest the game cam is allowed to rotate.
MaximumInterpolationZoomPerSecond=3.0		; in percent, where 1.0 is 100%
RotationInterpolationRampUpDuration=0.4 	; in seconds.  Total time to go from standstill to full interpolation speed
LocationInterpolationRampUpDuration=0.35 	; in seconds.  Total time to go from standstill to full interpolation speed
ZoomInterpolationRampUpDuration=0.25 	    ; in seconds.  Total time to go from standstill to full interpolation speed
ZoomedDistanceFromCursor=2600				; in Unreal units.  Distance from lookat target at maximum zoom.
DistanceFromCursor=1256						; in Unreal units.  Distance from lookat target at minimum zoom.
TetherScreenPercentage=1.2					; percentage of fov that counts as inside the tether. 1.2 corresponds roughly with the edges of the screen.
UseSwoopyCam=true
MaxTilesCameraCanMoveOutsideLevelVolume=10 ; how many tiles outside the level volume is the camera allowed to move?
AlienTurnPitch=-25
AlienTurnYaw=68.5
AlienTurnRoll=-2.5
AlienTurnFOV=55
HumanTurnPitch=-38
HumanTurnYaw=48.5
HumanTurnRoll=0
HumanTurnFov=60
```

(`TetherScreenPercentage` shipped at launch as **0.075** and was widened to
**1.2** in a patch — the change and its reason are documented in §10.)

### 3.1 The two turn poses

FOV is not varied on its own. **The turn change is a complete pose swap**, and
the FOV is one of four channels in it:

| Channel | Player turn | Alien turn | Δ |
|---|---|---|---|
| Pitch | −38° | −25° | +13° (flatter, more horizon) |
| Yaw | 48.5° | 68.5° | +20° |
| Roll | 0° | −2.5° | a slight dutch tilt |
| FOV | 60° | 55° | −5° (tighter, more compressed) |

The player's own yaw input is preserved as a delta on top, so rotating the
view survives the turn swap.

### 3.2 Zoom

Zoom is a normalised scalar, not a distance. The player's input accumulates
into `TargetZoom`, which is clamped: [FIRAXIS] `X2Camera_LookAt.uc`

```unrealscript
function ZoomCamera(float Amount)
{
	// don't let them overdo the zoom
	TargetZoom = FClamp(TargetZoom + Amount, -0.75, 1.0);
}
```

So the zoom range is **−0.75 → +1.0**, with 0 the default. `DistanceFromCursor`
(1256) is the distance at zoom 0 and `ZoomedDistanceFromCursor` (2600) at zoom
1.0. The mapping from scalar to distance is in `GetCameraLocationAndOrientation`,
which is `native` and therefore not readable; a straight lerp would put the
fully-zoomed-in distance at 248 uu, which is implausibly close, so **assume
the negative half is scaled differently and treat 1256–2600 as the figures to
copy.** [inferred] (The midpoint camera's script does treat it as
`distance = 1256 + zoom × 2600` when it *derives* a zoom from a distance —
§6 — which supports the linear reading for the positive half.)

There is a second, separate zoom channel for scripted push-ins —
`ZoomCameraPushIn(Amount, Time)` overrides the player's target and derives its
own speed as `abs(target) / time` rather than using the configured rate.

### 3.3 Camera geometry in world units

One XCOM 2 tile is **96 uu** square; one *floor height* is **64 uu** and a
storey is four of them, **256 uu** — all from `XComWorldData.uc`: [FIRAXIS]

```unrealscript
const WORLD_StepSize = 96.0f;
const WORLD_FloorHeight = 64.0f;
const WORLD_FloorHeightsPerLevel = 4.0f;
```

With the camera distance decomposed into height above the lookat point and
ground-plane offset: [derived]

| Pose | Distance | Height | Ground offset |
|---|---|---|---|
| Player turn, zoom 0 | 1256 uu / 13.1 tiles | 773 uu / **8.1 tiles** | 990 uu / 10.3 tiles |
| Player turn, zoom 1 | 2600 uu / 27.1 tiles | 1601 uu / **16.7 tiles** | 2049 uu / 21.3 tiles |
| Alien turn, zoom 0 | 1256 uu / 13.1 tiles | 531 uu / **5.5 tiles** | 1138 uu / 11.9 tiles |
| Alien turn, zoom 1 | 2600 uu / 27.1 tiles | 1099 uu / **11.5 tiles** | 2356 uu / 24.6 tiles |

The headline: **the default tactical shot is about 8 tiles up and 10 tiles
back at a 60° lens.** Zoomed fully out it is 17 up and 21 back. That is the
number to match if you want a prototype to *feel* like XCOM before any art
exists.

### 3.4 Interpolation, and the ramp/brake pair

Every channel is rate-limited rather than eased between two keys, which is
what makes the camera feel like it has mass while still tracking an arbitrary,
changing target:

| Channel | Max rate | Ramp-up |
|---|---|---|
| Translation | 3000 uu/s (31.25 tiles/s) | 0.35 s |
| Rotation | 400 °/s | 0.4 s |
| Zoom | 3.0 /s (300%/s) | 0.25 s |
| FOV | **1 °/s**, no ramp | — |

The location and zoom channels each multiply their speed by two alphas — one
ramping *up* over the configured duration, one braking as the target nears:
[FIRAXIS] `X2Camera_LookAt.uc:ComputeLocationBrakeAlpha`

```unrealscript
BrakeStartDistance = LocationInterpolationRampUpDuration * MaximumInterpolationDistancePerSecond * 0.4;
BrakeAlpha = DistanceFromDestination / BrakeStartDistance;
BrakeAlpha = FClamp(BrakeAlpha, 0.01f, 1.0f);   // never 0, or we never arrive
```

The braking distance is *derived from the ramp-up duration and top speed*, not
authored — so the deceleration is automatically symmetric with the
acceleration, and there is one knob per channel instead of two that can
disagree. With the shipped values, braking starts 420 uu (4.4 tiles) from the
destination.

The FOV interpolator is the odd one out and is worth quoting because it is so
blunt: [FIRAXIS] `X2Camera_LookAt.uc`

```unrealscript
protected function InterpolateFOV(float DeltaTime)
{
	// no ease in/out for now
	CurrentFOV += ((TargetFOV > CurrentFOV) ? 1 : -1) * (fMin(abs(TargetFOV - CurrentFOV), DeltaTime));
}
```

`DeltaTime` is used directly as the step, so the rate is **exactly 1°/s**. The
5° turn change therefore takes **five seconds** to complete — slower than the
rotation and the movement it happens alongside, and deliberately below the
threshold at which a lens change is noticed as an event.

### 3.5 The tether

Two native helpers implement the "is it on screen" question every follow
behaviour uses: [FIRAXIS] `X2Camera_LookAt.uc:81`

```unrealscript
// Returns the new tethered lookat for the camera, given the desired point. I.e., returns the camera's
// lookat point that keeps the desired lookat within the screen tether.
protected native final function Vector GetTetheredLookatPoint(Vector LookatPoint, TPOV Camera);

// returns true if the given point lies within the screen tether
protected static native final function bool IsPointWithinTether(TPOV Camera, Vector LookatPoint);
```

The tether is a **screen-space** dead zone sized as a fraction of the FOV
(`TetherScreenPercentage`). The camera is only pushed when the subject leaves
it, and only far enough to bring the subject back to its edge — hysteresis in
one number. At the shipped 1.2 the tactical camera lets a tracked subject
wander essentially anywhere on screen before following (see §10 for why).

### 3.6 Manual control limits

- **Pitch** is clamped to **[−90°, −10°]** in `PitchCamera` — straight down to
  10° below horizontal. Never level, never below.
- **Yaw** accepts new input only while the queued rotation is under one full
  turn: `if(abs(TargetRotation.Yaw - CurrentRotation.Yaw) < 360°)`. Otherwise a
  player spinning the wheel banks several seconds of rotation they then have
  to sit through.
- The camera may leave the level volume by at most **10 tiles** horizontally;
  height is always clamped to the roof of the level volume.
- A native helper, `MoveCameraUpIfEmbeddedInFloor`, exists purely to stop the
  camera being zoomed or pitched into the ground plane.

The manual-control philosophy: the tactical camera **does not collide with
the world at all**. It is held on rails above the play space (pitch floor,
volume clamp, floor embed nudge), and everything between it and its subject
is handled by hiding the world, not steering the camera (§7).

---

## 4. Following a moving unit

Two different cameras follow movers, and the split is deliberate: a normal
move keeps the **tactical framing** and simply tracks; a *dash* gets a low
dramatic chase camera. Both sit at `eCameraPriority_CharacterMovementAndFraming`,
so any game-action or cinematic camera outranks them.

### 4.1 `X2Camera_FollowMovingUnit` — the tactical follow

Extends `X2Camera_LookAt`, so it inherits the pose, the rails and the
rate-limited interpolation of §3 — following a mover is *just a moving lookat
point*. The shipped tuning: [FIRAXIS]

```ini
[XComGame.X2Camera_FollowMovingUnit]
TilesToLookAhead=1
TilesToPlaceCameraAheadOfUnit=1
UseFollowUnitCamera=true                  ; Do we use this camera, or a normal frame ability camera when a unit is moving?
LookAheadVsLookTowardDestinationRatio=0.0 ; 0.0 means only look ahead on the path, 1.0 means only look toward the destination
SkipIfPathAlreadyInSafeZone=true          ; if true, will only do the camera if the path starts or ends outside the safe zone
```

The mechanisms, from `X2Camera_FollowMovingUnit.uc`: [FIRAXIS]

- **Skip the whole camera if the move is already on screen.** On activation it
  walks every point of the path and tests `IsPointWithinTether` against the
  current view; if the entire path is inside, the camera goes *stationary* —
  no movement at all, it just keeps the unit tethered in case the player spins
  the view mid-move. This is the single biggest difference between XCOM 2's
  launch feel and its patched feel (§10): the camera stopped yanking to every
  soldier whose move you could already see.
- **Lead the unit, don't trail it.** The lookat point is
  `FindPointOnPath(distanceMoved + 1 tile)` — a point *ahead on the actual
  path*, not a velocity extrapolation, so it never looks into a wall the path
  turns away from. A config ratio blends that with "look toward the final
  destination" (shipped fully at look-ahead).
- **Smooth the look-ahead direction, not the position.** The bearing to the
  look-ahead point is `RLerp`ed with alpha = `DeltaTime` (~63%/s convergence),
  so a zig-zag path reads as a gentle pan rather than head-whipping at every
  corner.
- **The camera also sits ahead**: the lookat point is pushed 1 tile along the
  smoothed bearing (scaled by current zoom so the lead reads the same at any
  height), and clamped by remaining distance so the camera **arrives exactly
  on the destination** rather than overshooting past it.
- **Vertical snap.** The lookat Z is snapped to the floor plane of the unit's
  current storey (unless the unit is flying — "It feels janky for flying
  units"), so stairs, rubble hops and jump-downs do not bounce the camera.
- **Fog-of-war entrances.** If the mover is hidden when the camera starts
  (an enemy moving in fog), the camera looks at **the point where the unit
  will appear out of the fog** — the first visible point of the path — rather
  than tracking an invisible actor.
- **Handoff.** `HasArrived` flips when the lookat enters the tether at the
  destination; visualization-idle and active-unit-changed observers remove the
  camera defensively so a stuck camera can never deadlock the turn.

The part that answers "no obstacles in the way": this camera does not steer
around anything. It publishes **the remaining path tiles as focus points**
(§7.1) every frame, each with a synthetic camera position projected 9999 uu
back along the view direction — so the building-visibility system cuts down
walls and hides floors along the *whole path ahead of the mover*, and the
consumed tiles are dropped from the list as the unit passes them. The world
is opened up in front of the unit; the camera itself never dodges.

### 4.2 `X2Camera_RushCam` — the dash chase

```ini
[XComGame.X2Camera_RushCam]
FollowPitchInDegrees=-10      ; ideal pitch of the vector from the camera to the unit
FollowYawInDegrees=-25        ; ideal yaw of the vector from the camera to the unit
FOVInDegrees=45               ; fov to use while following the unit
CameraYawInDegrees=15         ; camera yaw relative to the follow yaw
CameraFollowDistanceInTiles=2 ; distance, in tiles, by which the camera should trail the moving camera
OffsetBlendTime=0.1           ; time, in seconds, that it take the camera to transition from one block/avoiding offset to another
CameraShake=CIN_CameraAnims.Shakes.Handheld1_Low
```

Note **`CameraYawInDegrees=15` is separate from `FollowYawInDegrees=-25`**:
the camera sits at one bearing from the unit and *points* 15° off that
bearing. The unit is therefore framed off-centre with the space ahead of it
in shot, which is the difference between a follow cam and a leash. `-10°`
pitch is right at the manual pitch clamp: this is a near-ground shot, with a
looping handheld shake on top.

**Obstacle avoidance is a yaw-offset search.** [FIRAXIS] `X2Camera_RushCam.uc`
Every update:

```unrealscript
// check if we can return to our desired behind the back location (or stay there)
NewTargetOffset = FindNearestUnblockedOffset(UnitLocation, ToDestination, 0.0f);

if(NewTargetOffset != 0.0f)
{
    // our desired camera location is blocked, so adjust from the previous offset. This makes sure that
    // any adjustments we need to do are relative to the previous offset, which keeps the camera from swinging
    // around crazily
    NewTargetOffset = FindNearestUnblockedOffset(UnitLocation, ToDestination, TargetOffset);
}
CurrentOffset = Lerp(CurrentOffset, TargetOffset, DeltaTime / OffsetBlendTime);
```

`FindNearestUnblockedOffset` is native, but the contract is visible: the
camera's position is parameterised as **an angular offset around the unit**
from the ideal behind-the-back bearing, and when the ideal is blocked the
search returns the nearest clear angle. The two-call structure is the part
worth copying: *prefer the ideal, but when it is blocked, search from where
you already are* — otherwise a camera oscillating between two clear arcs
swings wildly through the blocked one. The offset then blends over 0.1 s
rather than cutting. The camera is primed with one search before its first
frame so it never spawns inside a wall.

Arrival is a distance check (within one tile of the destination), and the
camera removes itself — the stack blends back to whatever is next.

### 4.3 Traversal cut-ins — child cameras in action

When the rush cam notices the mover's current path segment is a ladder climb,
drop-down or jump-up, it **pushes** a `X2Camera_ClimbLadderCam` child on
itself (§2) and stops updating; the child frames the traversal from 3 tiles
out and 1 tile above (falls: 2 out, 2 below), and when it pops the rush cam
resumes the chase. Traversal framing composes with the chase instead of being
a state inside it.

```ini
[XComGame.X2Camera_ClimbLadderCam]
DistanceFromLadderInTiles=3     ; ground plane distance to push the camera out from the ladder
DistanceAboveLadderInTiles=1    ; relative offset from the top of the ladder to place the camera

[XComGame.X2Camera_FallingCam]
DistanceFromFallInTiles=2     ; ground plane distance to push the camera out from the fall
DistanceBelowFallInTiles=2    ; relative offset from the top of the fall to place the camera
```

The AI-reveal variant of the lookat camera overrides *only* the location
ramp-up, 0.35 → **0.70 s** — the pod-reveal pan moves in at half speed and
everything else about it is inherited.

---

## 5. The shot cameras — authored shots, scored at runtime

This is the answer to "how do they decide where to put the camera" for every
firing, targeting and death shot, and it is not a solver. The class header
says it plainly: [FIRAXIS] `X2Camera_OverTheShoulder.uc:6`

> Over the shoulder cameras are a special variety of Matinee style camera that
> automatically adjust themselves to keep both the shooter and the target
> correctly framed in screen space […] They are authored as "shots" in
> matinee, where **spheres are placed to represent where the shooter and
> target's heads should be in frame**. In game then, this camera will look at
> where the actual shooter and target's heads are, and automatically rotate
> the camera location as needed to ensure that they appear in frame in exactly
> the same location as they did in matinee. For example, if the target is two
> floors above the shooter, the camera will move down and rotate upward to
> keep both heads correctly in frame.

So a camera artist authors a **library of shots** — each a matinee with two
marker spheres encoding the intended screen composition — and tags each with a
comment prefix (`CIN_Soldier_FF_Firing`, …). At runtime:

1. **Gather candidates** — every matinee in the level whose comment starts
   with the requested prefix.
2. **Randomise their order, then stable-sort by the artist's priority flag**
   (High/Med/Low/VeryLow). Randomising first means equal-priority shots are
   drawn in a different order every time — variety costs one shuffle.
3. **Score each candidate; lowest penalty wins; 0 is perfect and stops the
   search** (the early-out is compiled only into final release — dev builds
   score everything so the debug overlay can show the full table).
4. If nothing scores acceptably and the shot is optional
   (`ShouldAlwaysShow=false`), **don't play a cinematic at all** — the game
   quietly stays in tactical view. Spectacle is abandoned rather than shown
   badly. If the shot is mandatory, the last-tried matinee plays as a
   failsafe.

### 5.1 Framing, prediction, and the tether

The re-aiming that keeps both heads on their authored screen positions is
native (`ComputeCameraLocationAndOrientationAtTime`), but the script feeds it
two inputs worth noting:

- **The predicted head position, not the current one.**
  `GetPredictedHeadLocation(ShooterPawn, Target, TargetLocation)` — at
  selection time the shooter has usually not yet stepped out of cover or
  turned to face the target, so the shot is framed for where the head *will
  be*. Cover step-out is why this matters: the firing pose can be a full tile
  away from the idle pose.
- **A tether smooths the tracking.** The matinee-computed TPOV is chased with
  `alpha = DeltaTime / TetherDuration` (**0.8 s**; the reaction-fire variant
  tightens to **0.2 s**) — so breathing and recoil animations wobble the
  framing softly instead of rigidly coupling the camera to a bobbing head.

### 5.2 The obstruction score — how a shot avoids walls

The scoring is a penalty function whose weights ship in config: [FIRAXIS]

```ini
[XComGame.X2Camera_OverTheShoulder]
; Scoring used to determine which Matinee to choose for targeting.  Designed so cross-cutting (crossing
; the screen direction line) is only permissible if the soldier's head is visually blocked.
FiringUnitHeadBlockedScorePenalty=16
FiringUnitWaistBlockedScorePenalty=8 ; If this is not high enough we can get cases where our unit is on the other side of the wall
CantSeeFiringUnitAtAllPenalty=24
TargetedLocationBlockedScorePenalty=64
CrosscutScorePenalty=1000         ; Cross cuts are BAD. never do them
LookAtBackPenalty=0               ; Unless targeting, we don't care about looking at the unit's back
BlockedStartLocationPenalty=32
TransparencyPenaltyPercentage=0.3 ; Percentage of the full penalty values above transparent objects will incur
MinTilesBetweenTraceSamples=0.1
BlockedByPawnsExtentPenalty=0.2
BlockedByPawnsZeroExtentPenalty=0.25
MinZeroExtentPenaltyScalar=0.25
PriorityPenalty[0]=0  ;MatineeSelectionPriority_High
PriorityPenalty[1]=1  ;MatineeSelectionPriority_Med
PriorityPenalty[2]=9  ;MatineeSelectionPriority_Low
PriorityPenalty[3]=12 ;MatineeSelectionPriority_VeryLow
TraceWidth=20 ; 20 seems to work better than 24 at not colliding with geometry that is behind the target!
BlockedStartLocationWidth=4 ; Keep this small, mainly looking to see if the camera is inside something.
```

What the traces test (the scorer itself is native; the trace set is documented
by the penalty struct and config comments): as the candidate matinee's camera
path is sampled — a new sample every **0.1 tiles** of camera travel — a
**20 uu-wide** swept trace is fired at the **shooter's head**, the **shooter's
waist**, and the **target's head** (or targeted location), plus a tight
**4 uu** probe at the camera's own start position "mainly looking to see if
the camera is inside something". Per-sample penalties accumulate and average
into the shot's score. The refinements that make it robust:

- **Head and waist are separate traces with separate weights**, and both
  blocked together earns the extra `CantSeeFiringUnitAtAllPenalty` — the
  config comment names the failure it catches: *the firing unit is on the
  other side of a wall*. A waist blocked by a low wall is fine (that is what
  cover looks like); head *and* waist blocked means the shot is filming
  drywall.
- **The target being blocked is the worst non-veto penalty (64)** — four
  times the shooter's head. You can shoot over your soldier's shoulder
  without seeing much of the soldier; a shot that cannot see the *target* is
  pointless.
- **Transparent occluders pay 30% of the full penalty** — glass degrades a
  shot rather than disqualifying it, so shooting through a window picks the
  window shot only if nothing cleaner exists.
- **Pawns barely count** (0.2–0.25 scalars): another soldier wandering
  through frame is not a wall.
- **The shooter, the target, and any attached units are on an ignore list**
  rebuilt before scoring — the subjects can never block their own shot.

Two more penalties are computed in script, from cinematography rules rather
than traces:

- **Crosscut (1000)** — `WillMatineeCrossCut` tests whether the new shot
  would jump the 180° line relative to the previous camera orientation. The
  penalty scale makes it **a veto wearing a soft rule's clothing**: 1000
  against a next-highest of 64 can never be outbid by preference, yet because
  it is expressed in the same currency it needs no special-case code — and
  the ini comment above states the one designed exception: a crosscut *is*
  permissible when every non-crossing shot cannot see the soldier's head.
  The targeting and reaction-fire variants override it to 0 (a targeting
  camera orbiting to the other flank is not a cut).
- **Looking at the unit's back** — off (0) for firing shots, **6** for the
  targeting camera, where you are lining up the shot and want your soldier's
  face. The facing used is not the pawn's current facing but where the unit
  *will* face: `GetUnitFacing()` resolves the cover step-out side ("when the
  unit is in cover, we put the camera on the same side as the stepout"), and
  for uncovered units defaults to the right side — the comment says "no
  lefties".

| Tier | Values | Meaning |
|---|---|---|
| Preference | 0.2 – 12 | tie-breaks between otherwise fine shots |
| Real problem | 16 – 64 | can't see the shooter's head, can't see the target |
| Veto | 1000 | crossing the 180° line |

### 5.3 Target memory, and switching targets

Two behaviours cover tabbing between targets:

- **Per-target shot memory.** `SavedTargetCams` maps each target to the
  matinee first chosen for it, and tabbing back to a target **always returns
  to its saved shot** — the incumbency rule from CLAUDE.md's hot-loop notes
  applied to cinematography: re-deciding would sometimes decide differently,
  and the player reads that as the camera being indecisive.
- **Suffix-paired continuity.** Artists pair shots by suffix (`_L_1`, `_R_2`);
  when one OTS camera follows another, `SelectClosestMatinee` first looks for
  a candidate with the **same `_L`/`_R` suffix** — same screen side, so the
  cut holds screen direction — and only falls back to nearest-by-distance.

When the target changes under a live camera, the transition duration is
**derived from how far the camera must move**: rotation alpha from the dot
product of the two orientations, location alpha from distance over
`RetargetDurationMaxTiles` (1.6 tiles), whichever is larger, lerped into
**0.5–1.6 s**. Small adjustments are quick, flank-to-flank swings take the
full time.

And the retarget *path* is the best camera trick in the file: instead of
lerping the position (which can drag the camera straight through the
soldier's head), it builds a two-point Hermite curve whose **tangents are the
camera facings crossed with a vertical vector scaled by twice the travel
distance** — so the camera **arcs around the subject**, on the side matching
the yaw direction, with the bulge attenuated to zero as the two facings
approach parallel. [FIRAXIS] `X2Camera_OverTheShoulder.uc:697`

```unrealscript
// Since just lerping between the previous and new target camera location could cause the camera
// to clip through a dude's head, we build a curve for the location to traverse which uses
// the old and new camera orientations as the control points. This will build a nice curve
// around the soldier which won't clip his body in the worst case.
```

The two blend curves ship in config and are both front-loaded — location
reaches 0.53 by t=0.24; rotation **0.65 by t=0.2**. The camera snaps its gaze
to the new subject almost immediately and then drifts into position, which is
how a human operator whips a pan.

### 5.4 What the OTS camera does to the rest of the game

While active it: hides the UI and pathing, plays its looped handheld shake
(`Handheld1_Low` at intensity 1.2), **turns fog of war rendering off**
(`SetFOW(false)` — a cinematic never shows the grey veil), disables building
cutdown (§7), and switches the between-camera-and-subject occluders to
**proximity dither** with the radius set to the camera-to-shooter distance
and the shooter and target on the ignore list. Depth of field focuses the
target by default (the shooter for some cuts), which is where the §9 DOF
parameters land.

---

## 6. Framing a group — the midpoint camera

`X2Camera_Midpoint` answers "make sure everything relevant is in camera" for
ability framing — shooter plus all targets, a grenade's whole blast area, a
Lost horde. It is a `X2Camera_LookAt` subclass, so whatever it computes is
expressed as a lookat point plus a zoom on the ordinary tactical rails — the
framing shot is still recognisably the game camera, and the player's manual
yaw still works (pitch and manual zoom are disabled).

```ini
[XComGame.X2Camera_Midpoint]
AccentZoom=-0.1 ; percentage zoom to add to the camera if the framing still fits (negative numbers zoom in)
FramingFOVPercentage=0.75
MaximumMidpointZoomOut=2800.0
```

The placement algorithm is documented in-source: [FIRAXIS]
`X2Camera_Midpoint.uc:371`

```unrealscript
// Explanation of what is going here (in case the math is hard to understand).
// Since we know the direction the camera is facing in, we can compute the top and
// bottom planes of it's frustum. For proper framing, we want these planes to intersect
// with the nearest and farthest lookat points. If you think about this in reverse, we can
// now say that the place where these two planes intersect is where the camera should go.
// So once we've calculated the desired angles for the top and bottom frustum planes, we
// overlay them on the lookat points and find where they intersect. The camera goes there.
// After that, we just pull it back along it's normal (facing direction) until every point
// lies cleanly within the frustum
```

Step by step: shrink the FOV to 75% (that is the safe-area margin — fit the
subjects into the middle three-quarters of the frame); find the nearest and
furthest focus points along the view direction; slide the bottom frustum
plane onto the near point and the top plane onto the far point; **their
intersection line is the camera position** for the vertical axis; then pull
back along the view normal until the horizontal axis fits too. Convert the
result into lookat + zoom (`TargetZoom = (distance − 1256) / 2600`), apply
`AccentZoom=-0.1` — a 10% push-*in* that adds a little tension if the framing
still holds — and clamp the pull-back at 2800 uu so a spread-out squad cannot
drag the camera into orbit.

Details that keep it from misbehaving:

- **Units contribute two focus points each — head and feet** — so both ends
  of a unit two floors up stay framed, not just its origin. In
  follow-moving-actors mode the head point is dropped: "otherwise head
  bobbing animations will negatively impact the framing camera's motion".
- **Every focus point is validated against the level volume** and clamped
  with a redscreen if outside — a bad point pulls the frame, so garbage-in is
  caught loudly at the door.
- **Zoom is interpolated as a function of travel**, not time: alpha =
  distance-covered / total-distance (or yaw-covered during a user rotation),
  so the zoom-out lands exactly when the pan lands, one motion instead of
  two racing easings. A guard skips this for sub-unit-length moves — "will
  feel like a hitch".
- On activation from the game camera it inherits the player's zoom, adds the
  accent, and **restores the saved zoom on exit** so cinematic framing never
  permanently steals the player's zoom preference.

Cinescript's documentation (§8) notes midpoint framing is the *default*
wrapper for every ability, before and after whatever dramatic cuts play.

---

## 7. Never showing a wall — scoring, plus a world that yields

"How do they handle not viewing a wall" has two halves that share nothing:

**Half one — cinematic cameras avoid walls.** The OTS scoring of §5.2 simply
never picks a shot filming drywall unless every alternative is worse, and
optional shots prefer not to play at all. What little geometry still
intrudes is handled by proximity dither (§5.4). Notably, cinematic cameras
**turn building cutdown off** — `AllowBuildingCutdown()` returns false for
them by design ("generally not desired for cinematic, over the shoulder and
glam style cameras" — the base-class comment), because a glam shot of a
soldier inside a building should show the room, not a dollhouse cross-section.

**Half two — the tactical camera makes the world yield.** The overhead camera
cannot avoid walls; at −38° pitch into a two-storey map it is *always* looking
through structure. So the structure gives way, through a pipeline that runs
every frame in `XComBuildingVisManager`:

### 7.1 Focus points — the contract between cameras and the world

Every `X2Camera` publishes an array of `TFocusPoints`: [FIRAXIS] `X2Camera.uc:42`

```unrealscript
struct native TFocusPoints
{
    var vector vFocusPoint;      // what must be visible
    var vector vCameraLocation;  // from where
    var float fDelay;            // default 0.25s
    var float fCutoutHeightLimit;
};
```

> Basically, visibility to this point will be cleared so you can see it. The
> first item in the array will be considered to have the priority with
> regards to building visibility and building cutdown.

The game camera publishes its lookat; the follow camera publishes the mover
*plus every tile remaining on its path* (§4.1); the midpoint camera publishes
all of its framed subjects; kismet can force-add focus actors for scripted
moments. The manager stamps each point's `fCutoutHeightLimit` to its floor
height + 75 uu, so cutting never slices below the subject's own storey. The
`fDelay` default of 0.25 s debounces re-reveals as focus points move.

### 7.2 What consumes them

`XComBuildingVisManager::TickSpecial` (native — the four subsystem names are
declared in the class's cpptext) runs, in order: [FIRAXIS] `XComBuildingVisManager.uc:76`

```cpp
void CheckFloorsToHide();
void MouseObscuringActorHiding();
void CutoutBoxHiding();
void PeripheryHiding();
```

- **Floor hiding.** Maps are marked up with `XComBuildingVolume`s, each
  holding an array of `Floor`s, each floor listing its `XComFloorVolume`s and
  a **cached list of every resident actor** (meshes, emitters, particle
  components, even ambient sounds — rebuilt at load, not queried per frame).
  When a focus point is inside a building, floors above it hide —
  `ProcessVisibilityChange` takes a cutdown height and cutout height per
  floor. This is the dollhouse cut: hover the cursor into the ground floor
  and the storeys above vanish wholesale, as cached lists, not per-mesh
  traces.
- **Which building is the subject in?** Answered by collision, not queries:
  `XComBuildingVisPOI` is an invisible cylinder (radius 14, height 64 —
  "frequently following a unit so we want to match the unit's collision")
  that is **teleported every tick to the primary focus point**, and its
  Touch/UnTouch events against building and floor volumes maintain the
  current-building, current-floor, inside/outside state. The volumes already
  exist; overlap events maintain the answer for free.
- **Trace hiding.** Actors flagged hideable that obscure the mouse/cursor
  line are hidden per-actor (the `m_aHiddenActors` machinery) — this is what
  removes a lamppost or tree between camera and cursor.
- **The cutout box.** A separate invisible **collision cube**
  (`XComCutoutBox`, literally `ASE_UnitCube` scaled up, `COLLIDE_TouchAll`)
  rides between camera and subject; everything it touches gets
  `SetDitherEnable(true)` and fades to a dithered ghost, minus the ignore
  list (the shooter and target, so a camera pushed close never dithers its
  own subject). Touch/UnTouch bookkeeping means un-dithering is exact — no
  scan to find "whatever we dithered last frame".
- **Periphery hiding.** Volumes for map-edge dressing that should vanish when
  the camera swings low.
- **The cinematic layer.** Actors flagged `bHideInNonCinematicViews` toggle
  as one layer whenever the camera's angle from straight-down crosses **55°**
  — set dressing that exists only for low glam cameras (full ceilings, roof
  detail) is not paid for, or shown, in the overhead view.

Every one of these is gated per-camera: the stack forwards
`AllowBuildingCutdown()` / `AllowProximityDither()` from whichever camera is
active, so the *camera type* decides whether the world yields (tactical: yes)
or the shot avoids (cinematic: yes). The two halves never fight.

---

## 8. Cinescript — the shot list as data

`[XComGame.X2Camera_Cinescript]` is the bulk of `DefaultCamera.ini` (the WOTC
file is 1,649 lines, most of it this section) — a data-driven shot list per
ability type. The WOTC ini opens with full documentation of the DSL; the
shape:

```ini
AbilityCameras=(AbilityCameraType="StandardGunFiring", ShooterTeam=CinescriptShooterTeam_XCom,
    StartBeforeStepout=1,
    CameraCuts[0]=(ShouldAlwaysShow=false, NewCameraType=CinescriptCameraType_OverTheShoulder,
                   MatineeCommentPrefix="CIN_Soldier_FF_Firing"),
    CameraCuts[1]=(CutAnimNotify="DeathCut", FocusPrimaryTarget=True, CutChance=.5,
                   NewCameraType=CinescriptCameraType_OverTheShoulder, MatineeCommentPrefix="CIN_Soldier_OTSDeath"),
    CameraCuts[2]=(CutAfterPrevious=True, NewCameraType=CinescriptCameraType_Exit),
    ...
```

Each cut names a trigger (usually an **animation notify** — the cut fires when
the animation says so, which is how a death cut lands on the death), a
probability, a camera type (`OverTheShoulder` / `Matinee` / `Midpoint` /
`Exit`) and a matinee prefix. Facts from the shipped instructions header worth
recording: [FIRAXIS]

- *"Abilities are always framed by a midpoint camera outside of what
  cinescript does"* — the midpoint wrapper of §6 is the floor under every
  ability; Cinescript cuts layer on top of it, and `Exit` returns to it.
- Prefixes select **families**: `"CIN_KillAlien"` picks among
  `CIN_KillAlien_1`, `_2`, …; and *"if the suffix contains `_L` or `_R`, then
  the game will attempt to pick a camera with the same screen direction"* — 
  the §5.3 continuity rule, stated as authoring guidance.
- `MatineeReplacements` swaps prefixes per character template — Mutons,
  Sectopods, Gatekeepers and turrets each get their own firing coverage
  without a new ability entry.
- Selection filters include shooter team, target team, and **whether the
  target died** — the kill and non-kill versions of the same ability are
  different shot lists.

Distributions across the launch table (54 distinct ability camera types):

| Field | Values (count) |
|---|---|
| `NewCameraType` | `Matinee` (91), `Exit` (69), `OverTheShoulder` (32), `Midpoint` (14) |
| `CutChance` | **.65 (25)**, .5 (13), 1 (4), .6 (2), .75 (1), .3 (1) |
| `TargetType` | `AllParticipants` (10), `AllTargets` (4) |

The number to steal is **`CutChance=.65` as the house default**: a dramatic
cut fires roughly two times in three, so the same ability does not produce
the same film every time, and flat 100% cuts are reserved for one-off
spectacle (`Soldier_HeavyWeapons`). Repetition is the enemy of a cinematic
camera, and the whole mitigation is one float per cut.

---

## 9. Supporting values outside the camera classes

```ini
; DefaultCamera.ini
[XComGame.XComCamera]
DOF_MaxFarBlurAmount=1.0
DOF_MaxNearBlurAmount=0.0
DOF_FocusInnerRadiusRatio=0.9 ; The radius border extends out to a midpoint at this ratio from the shooter to the target, unless it is lower than the next value
DOF_FocusMinInnerRadius=800.0  ; The radius is equal to DOF_FocusInnerRadiusRatio the distance to the target, or this value, whichever is greater.
DOF_FalloffExponent=1.5
DOF_BlurKernelSize=1.0

[XComGame.X2ReactionFireSequencer]
ReactionFireWorldSloMoRate=0.66 ; A value between .33 and 1.0 that will control how much "global" slo motion occurs when showing reaction fire

[XComGame.X2Action_ApplyWeaponDamageToTerrain]
CameraShakeIntensity_Large=0.15
CameraShakeTileThreshold_Large=60
CameraShakeIntensity_Medium=0.08
CameraShakeTileThreshold_Medium=20
CameraShakeIntensity_Small=0.02
CameraShakeTileThreshold_Small=10

[XComGame.X2Action_RevealAIBegin]
FirstSightedDelay=0.75
```

```ini
; XComEngine.ini
[Engine.Engine]
NearClipPlane=10.0

[Engine.LocalPlayer]
AspectRatioAxisConstraint=AspectRatio_MaintainXFOV
```

Three things to pull out:

- **Near clip is 10 uu**, i.e. about a tenth of a tile, against a camera that
  sits 1256 uu away. There is enormous depth precision headroom here; the
  near plane is set for the over-the-shoulder and matinee cameras that get
  close to geometry, not for the tactical view.
- **Depth of field is anchored to gameplay distance, not to a focal length.**
  The in-focus radius is `max(0.9 × distance-to-target, 800 uu)` with a 1.5
  falloff exponent, and near blur is **disabled outright** (`MaxNearBlur=0`)
  — because anything in front of the focus plane during a shot is cover the
  player needs to read.
- **Camera shake is picked by damage radius in tiles**: 10 / 20 / 60 tile
  thresholds map to 0.02 / 0.08 / 0.15 intensity. A ~7.5× intensity range
  across a 6× radius range. During reaction fire, the whole world drops to
  66% speed while the sequence plays.

Also here for reference: slo-mo during OTS shots is data — matinees carry a
slomo track the camera samples per frame and applies to the whole game
(`SampleSlomoTrack` → `SetGameSpeed`), so bullet-time is authored per shot,
not coded.

---

## 10. Launch → War of the Chosen: what Firaxis retuned

The base-game launch ini and the WOTC SDK ini agree on almost everything —
the poses, distances, rates, penalties and curves above are identical. The
diffs are few and every one is legible: [FIRAXIS]

| Value | Launch (Feb 2016) | WOTC SDK | Why |
|---|---|---|---|
| `X2Camera_LookAt.TetherScreenPercentage` | 0.075 | **1.2** | see below |
| `X2Camera_FollowMovingUnit.SkipIfPathAlreadyInSafeZone` | false | **true** | see below |
| `X2Camera_Midpoint.MaximumMidpointZoomOut` | — (absent) | **2800** | cap on group-framing pull-back |
| `X2Camera_Matinee.AllowDynamicBlockDetection` | — (absent) | **true** | matinees gained runtime blockage checks |
| `StandardGunFiring.ExtraAbilityEndDelay` | 1 s | **0** | snappier return to gameplay after shots |
| `X2Camera_OTSReactionFireShooter.BlendDuration` | — (inherited 2 s) | **0.75** | "the camera arrives before the soldier starts shooting" |
| `X2Camera_TheLostReveal` section | — | new | WOTC horde reveal camera |

The first two rows are one change, and the shipped ini documents it inline —
a rare case of a patch note living in the config: [FIRAXIS]

```ini
;<WORKSHOP> Make X2Camera_FollowMovingUnit not move the camera, if the entire unit's movement path is on-camera AMS 2016/04/13
;WAS:
;TetherScreenPercentage=0.075
TetherScreenPercentage=1.2   ; 1.2 corresponds roughly with the edges of the screen.
```

At launch the camera recentred on every move, because the tether was a tight
7.5% window and the follow camera always ran. The April 2016 patch widened
the tether to the whole screen and turned on the skip — **a move whose entire
path is already visible no longer moves the camera at all**. This was the
single most player-visible camera change the game shipped, and it cost two
config values.

---

## 11. What to take into the prototype

Ranked by how much they buy for how little:

1. **60° horizontal / 36° vertical, ~8 tiles up, ~10 tiles back, pitched
   −38°.** The single configuration that reads as "tactics game". Everything
   else is refinement on top.
2. **A camera stack with enum priorities, yield, and child cameras.** Systems
   add and remove cameras; an arbiter picks; nobody touches anyone else's
   camera. The three-aliens-and-a-gas-tank example is the test case.
3. **Rate limits with a derived brake distance, not eased keyframes.** One
   top-speed and one ramp-up per channel, deceleration computed as
   `ramp × speed × 0.4`. The camera can then chase an arbitrarily moving
   target and always feel the same.
4. **Don't move the camera for a move that is already on screen** — the one
   retune Firaxis shipped a patch for. A screen-space tether with hysteresis
   is the required primitive.
5. **Camera selection as a penalty function over authored candidates** with
   preference / problem / veto tiers in one currency, partial penalties for
   transparent occluders, near-zero penalties for pawns, and per-target shot
   memory. This generalises well beyond a shot camera.
6. **Focus points as the camera↔world visibility contract.** Every camera
   publishes "these points must be visible from here"; floor hiding, wall
   cutdown and dither all consume the same array. The follow camera pushing
   its *future path* through the same channel is the pattern at its best.
7. **Frame groups analytically** (frustum-plane intersection + pull-back at
   75% FOV), then express the result as lookat + zoom on the normal camera
   rails so framing shots stay in the game-camera family.
8. **FOV changes at ~1°/s.** Slow enough not to register as an event.
9. **A per-cut probability (house default 0.65) on cinematic camera work** so
   repetition does not set in — and when no clean shot exists, *skip the
   cinematic*, don't play a bad one.
10. **When a chase camera's ideal spot is blocked, search angularly from the
    previous offset, not from the ideal** — the two-call structure in §4.2 —
    and blend offsets over ~0.1 s.

The one thing *not* to copy directly is `AspectRatio_MaintainXFOV` — see §1.

---

## 12. What this note does not establish

- **The native C++.** Everything marked `native` — the zoom→distance curve,
  the matinee re-aiming solve, the obstruction scorer's trace loop, the
  rush cam's offset search, the floor-hiding internals — is described here
  from its declarations, config, comments and call sites, not its body.
  The *shape* of each is well-attested; exact formulas are [inferred].
- **The matinee content.** Every `MatineeCommentPrefix` points at an authored
  camera animation in a package this note has not opened. The scoring picks
  between shots; the shots themselves are art, in map packages.
- **`X2Camera_Cinescript.uc` / `X2Camera_Matinee.uc` internals** (34 KB and
  18 KB of sequencing logic) — §8 describes the data format and its shipped
  documentation, not the sequencer implementation.
- **Strategy-layer cameras** (`XComHeadquartersCamera`, the Avenger and
  geoscape views) are out of scope entirely.
