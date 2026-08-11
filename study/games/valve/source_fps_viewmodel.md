# The Source first-person view model — camera, render, lighting, animation

How Valve draw a gun in the player's hands that sits *over* the world without
being part of it, and still get bob, sway, lighting, cloaking, cosmetics and a
full animation graph out of it.

Everything marked **[VALVE]** is transcribed from the Source SDK 2013 tree, which
is the shipping Team Fortress 2 client and server code. Paths in this note are
relative to `src/` in that tree. **[EPIC]** is Epic's own documentation.
**[COMMUNITY]** is forum/blog consensus, used only where no first-party
statement exists. **[inferred]** is reasoning from the code rather than a
statement in it.

> **Where the source came from.** `ValveSoftware/source-sdk-2013`, `master`,
> pushed 2026-08-06, extracted to `E:/Game Development/Tools/source-sdk-2013-master`.
> The Steam install of *Source SDK Base 2013 Multiplayer* is engine binaries and
> content only — it contains no C++. The game code is the GitHub repo, and it is
> the real TF2 client: `game/client/tf/`, `game/shared/tf/`.

---

## 1. The answer, before the detail

The user-facing problem is: **the weapon must never be clipped by a wall, but it
must not look like a sprite pasted on the frame.** Valve's answer is five
independent decisions, and it is important that they are independent — most
engines get one or two of them and inherit the failure modes of the rest.

| Decision | Mechanism | File |
|---|---|---|
| The weapon is not in the world's spatial index at all | Its own list, `m_ViewModels`, not the BSP leaf tree | `game/client/clientleafsystem.cpp:634` |
| It is drawn in a **second 3D view** after the scene | `render->Push3DView( viewModelSetup, ... )` | `game/client/viewrender.cpp:1090` |
| That view has **its own FOV and its own near plane** | `fovViewmodel` = 54°, `zNearViewmodel` = 1 | `game/client/view.cpp:729,643` |
| It cannot poke through geometry because its **depth is remapped into the front 10%** of the buffer | `pRenderContext->DepthRange( 0.0f, 0.1f )` | `game/client/viewrender.cpp:1109` |
| It is lit at the **player's body**, not at its own position | `pInfo->pLightingOrigin = &GetOwner()->WorldSpaceCenter()` | `game/shared/tf/tf_viewmodel.cpp:284` |

The depth-range line is the whole trick, and it is worth stating precisely what
it does, because it is routinely described wrongly.

**It does not disable the depth test.** The view model is still depth-tested and
depth-written, against itself, normally. What changes is the *mapping* from clip
space to the depth buffer: instead of writing to `[0,1]`, it writes to `[0,0.1]`.
Every world pixel already in the buffer, unless it was within the first 10% of
the world's own depth range, is numerically further away. So the weapon wins
every comparison against the world, whilst still resolving its own overlaps
correctly — the barrel occludes the hand, the hand occludes the sleeve.

That is the distinction that a naive "draw it last with depth test off" misses,
and it is why the Source view model has correct self-occlusion whilst a
depth-test-off hack does not.

Valve's own comment, verbatim:

```cpp
// HACK HACK:  Munge the depth range to prevent view model from poking into walls, etc.
// Force clipped down range
if( bUseDepthHack )
    pRenderContext->DepthRange( 0.0f, 0.1f );
```

`game/client/viewrender.cpp:1106-1109` [VALVE]

The `bUseDepthHack` name and the `HACK HACK` are honest: this is a cheap trick,
and §5 covers what it costs.

---

## 2. The camera

The camera is computed once per frame in `CViewRender::SetUpViews()`
(`game/client/view.cpp:628`), and the view model transform is computed **in the
same function, from the same eye transform, before anything renders**. That
ordering matters and is the first thing an engine tends to get wrong: if the
weapon's transform is computed in a different pass from the camera's, they
disagree by a frame and the gun swims.

### 2.1 The chain

```
CViewRender::SetUpViews()                       view.cpp:628
 └─ pPlayer->CalcView( origin, angles, zNear, zFar, fov )     baseplayer_shared.cpp:1533
     ├─ CalcVehicleView   — if in a vehicle
     ├─ CalcObserverView  — if spectating
     └─ CalcPlayerView                                        baseplayer_shared.cpp:1582
         ├─ view->DriftPitch()          — auto-centre when using a controller
         ├─ eyeOrigin = EyePosition()   — origin + view offset (stand/crouch)
         ├─ SmoothViewOnStairs()        — the step-up lerp
         ├─ CalcViewRoll()              — lean into strafing
         ├─ += m_Local.m_vecPunchAngle  — recoil, networked and predicted
         ├─ vieweffects->CalcShake() / ApplyShake( ..., 1.0 )
         ├─ += GetPredictionErrorSmoothingVector()
         └─ fov = GetFOV()
 └─ (FOV offset computed)                                     view.cpp:724-729
 └─ pPlayer->CalcViewModelView( ViewModelOrigin, ViewModelAngles )   view.cpp:767
```

Four things in `CalcPlayerView` are worth copying wholesale [VALVE]:

- **`SmoothViewOnStairs`** and **`GetPredictionErrorSmoothingVector`** are both
  guarded by `if ( !prediction->InPrediction() )`. Cosmetic smoothing must not
  run inside the prediction loop, or it feeds back into the simulation and the
  error it is smoothing becomes permanent. Every "why does my FPS camera jitter
  when I resimulate" bug is this.
- **Punch angle is networked, interpolated *and* predicted**
  (`m_vecPunchAngle` + `m_vecPunchAngleVel`, `c_baseplayer.cpp:166-174,333-337`).
  Recoil is a simulation quantity, not a camera effect, because the server
  resolves shots with it.
- **Shake is applied to the camera at `1.0` and to the view model at `0.1`**
  (`baseplayer_shared.cpp:1626`, `baseviewmodel_shared.cpp:414`). The weapon
  deliberately does *not* track the screen shake, which is what makes an
  explosion read as the world moving rather than the gun moving.
- The whole thing is one function returning `(origin, angles, fov)`. There is no
  camera component tree, no spring arm, no blend stack.

### 2.2 Two FOVs, and how they are reconciled

```cpp
// 54 degrees approximates a 35mm camera - we determined that this makes the viewmodels
// and motions look the most natural.
ConVar v_viewmodel_fov( "viewmodel_fov", "54", FCVAR_ARCHIVE, ..., true, 0.1, true, 179.9, true, 54, true, 70, NULL );
```

`game/client/view.cpp:108-111` [VALVE] — TF2 builds only; every other Source
game has this as `FCVAR_CHEAT`. The trailing `54, 70` are the competitive
clamp: players may widen it, but only within that band.

The world FOV is 75 by default and changes with zoom, sprint, and class. The
view model FOV must move *with* those changes without simply being them:

```cpp
float fDefaultFov = default_fov.GetFloat();
float flFOVOffset = fDefaultFov - viewEye.fov;
viewEye.fovViewmodel = g_pClientMode->GetViewModelFOV() - flFOVOffset;
```

`game/client/view.cpp:724-729` [VALVE]

So the view model FOV tracks the world FOV **by delta, not by ratio**. Scope in
and the world FOV drops 55°; the weapon FOV drops the same 55°, so the weapon
appears to zoom by the same absolute amount whilst keeping its own 54° base.

Both are then converted from a 4:3 reference to the real aspect by the same
horizontal-preserving transform:

```cpp
float ScaleFOVByWidthRatio( float fovDegrees, float ratio )
{
    float t = tan( fovDegrees * ( 0.5f * M_PI / 180.0f ) );
    t *= ratio;
    return 2.0f * ( 180.0f / M_PI ) * atan( t );
}
```

`game/client/view.cpp:990-997` [VALVE]

Note the asymmetry at `view.cpp:1083-1084`: the **world** FOV is scaled by
`limitedAspectRatio`, capped at 1.85:1 when `sv_restrict_aspect_ratio_fov` is
on, because a very wide monitor is a competitive advantage. The **view model**
FOV is scaled by the true `aspectRatio`, uncapped — the gun's shape is
cosmetic, so it is allowed to follow the monitor. Two FOVs that look like they
should share a code path, deliberately not sharing one.

---

## 3. Where the weapon actually sits — bob, lag, lowering

`CBaseViewModel::CalcViewModelView` (`game/shared/baseviewmodel_shared.cpp:385`)
takes the eye transform and produces the view model's transform. The order is
fixed and each stage is additive:

```cpp
pWeapon->AddViewmodelBob( this, vmorigin, vmangles );   // 1. walk cycle
CalcViewModelLag( vmorigin, vmangles, vmangoriginal );  // 2. turn sway
AddViewModelBob( owner, vmorigin, vmangles );           // 3. off-hand bob
vieweffects->ApplyShake( vmorigin, vmangles, 0.1 );     // 4. 10% of screen shake
SetLocalOrigin( vmorigin );
SetLocalAngles( vmangles );
```

Bob and lag are wrapped in `if ( !prediction->InPrediction() )` for the same
reason as the camera smoothing.

### 3.1 Bob

`CalcViewModelBobHelper` / `AddViewModelBobHelper`,
`game/shared/tf/tf_weaponbase.cpp:4713,4801` [VALVE]. Structurally it is Quake's
bob, kept:

- The bob phase advances by `dt * bob_offset` where `bob_offset` is speed
  remapped `0..320 → 0..1`. **The clock is scaled by speed, not the amplitude
  alone** — so stopping freezes the cycle rather than shrinking it in place.
- Speed changes are rate-limited to `320 units/s²` (`flmaxSpeedDelta`) before
  they reach the bob, so landing or being blast-jumped does not snap the phase.
- The vertical and lateral cycles run at different rates (`flBobCycle` vs
  `flBobCycle*2`), which is what makes it read as a figure-of-eight footfall
  rather than a bounce.
- Both are clamped asymmetrically, `[-7, +4]`.
- The result drives **five** channels, at very different weights:
  forward `×0.4`, up `×0.1`, roll `×0.5`, pitch `×-0.4`, yaw `×-0.3`, right `×0.2`.
  The rotational channels are weighted higher than the translational ones. That
  is the part usually missed: a bob that only translates looks like an elevator.
- One `BobState_t` lives **on the view model** (`tf_viewmodel.h:71`), not on the
  weapon, so switching weapons does not restart the walk cycle.

### 3.2 Lag / sway — two implementations, and TF2 uses neither by default

The base implementation (`baseviewmodel_shared.cpp:463`) integrates a
`m_vecLastFacing` vector toward the current facing at `5.0/s`, with a catch-up
multiplier once the error passes `g_fMaxViewModelLag = 1.5`, and displaces the
origin along the *difference*. The catch-up exists because without it a fast
joystick turn slams `m_vecLastFacing` and the weapon pops — the comment says so.

It then adds a pitch-driven offset:

```cpp
VectorMA( origin, -pitch * 0.035f, forward, origin );
VectorMA( origin, -pitch * 0.03f,  right,   origin );
VectorMA( origin, -pitch * 0.02f,  up,      origin );
//FIXME: These are the old settings that caused too many exposed polys on some models
```

`baseviewmodel_shared.cpp:514-517` [VALVE] — looking down pulls the weapon
back and to the side so you see the top of it rather than its unmodelled
underside. The `FIXME` is the honest admission that this is tuned per-model and
these values are the wrong ones for some.

TF2 overrides it (`tf_viewmodel.cpp:85`) with a history-buffer version — record
angles into `m_LagAnglesHistory`, interpolate back `cl_wpn_sway_interp` seconds,
displace by the difference — and then **disables it**:

```cpp
// TODO:  Turning this off by setting interp 0.0 instead of 0.1 for now since we have a timing bug to resolve
ConVar cl_wpn_sway_interp( "cl_wpn_sway_interp", "0.0", ... );
```

`tf_viewmodel.cpp:64-65` [VALVE]. Shipped TF2 has no weapon sway, by accident,
since 2008. Worth knowing before treating the shipped look as the designed one.

### 3.3 Lowering and min-mode

`CTFViewModel::CalcViewModelView` (`tf_viewmodel.cpp:130`) adds two more:

- **Lowering** — `m_vLoweredWeaponOffset.x` approaches `cl_gunlowerangle` (90°)
  at `cl_gunlowerspeed`, added to pitch. When it arrives, `DrawModel` returns
  early and stops drawing entirely (`tf_viewmodel.cpp:250-254`). The animation
  and the render both participate; neither alone is enough.
- **Min-mode** — `tf_use_min_viewmodels` applies a per-weapon
  `GetViewmodelOffset()` in eye-space. Note it is blended against the inspect
  animation by `Gain( 1.f - s_inspectInterp, 0.5f )`, so inspecting cancels
  min-mode smoothly rather than fighting it.

---

## 4. The render path

### 4.1 Where it sits in the frame

From `CViewRender::RenderView` (`game/client/viewrender.cpp:2090-2210`) [VALVE]:

```
CSkyboxView                     — 3D skybox
ViewDrawScene(...)              — world, entities, particles, water
render->SceneEnd()
RenderPlayerSprites()
DoImageSpaceMotionBlur()        ← motion blur happens BEFORE the view model
DoPostScreenSpaceEffects()
DrawViewModels( ... )           ← here, viewrender.cpp:2160
DrawUnderwaterOverlay()
ViewDrawFade() / PerformScreenOverlay()
DoEnginePostProcessing()        ← bloom and tonemap happen AFTER
PerformScreenSpaceEffects()
```

Two placements, deliberately different [inferred, from the ordering]:

- **After motion blur** — the weapon is nailed to the camera, so camera motion
  blur applied to it would be wrong. Excluding it costs nothing because it is
  drawn later anyway.
- **Before bloom and tonemapping** — a muzzle flash must bloom, and the weapon
  must sit in the same tonemapped exposure as the scene. Draw it after
  tonemapping and it looks pasted on; this is exactly the artefact the
  two-camera Unity setup produces (§10).

### 4.2 The second view

```cpp
CViewSetup viewModelSetup( viewRender );
viewModelSetup.zNear          = viewRender.zNearViewmodel;   // 1
viewModelSetup.zFar           = viewRender.zFarViewmodel;    // same as world
viewModelSetup.fov            = viewRender.fovViewmodel;     // 54 ± offset
viewModelSetup.m_flAspectRatio = engine->GetScreenAspectRatio();

render->SetColorModulation( one );   // reset — the scene left these dirty
render->SetBlend( 1.0f );

render->Push3DView( viewModelSetup, 0, pRTColor, GetFrustum(), pRTDepth );
  ... DepthRange( 0, 0.1 ) ...
  ClientLeafSystem()->CollateViewModelRenderables( opaqueList, translucentList );
  UpdateRefractIfNeededByList( ... );          // only if something refracts
  DrawRenderablesInList( opaqueList );
  DrawRenderablesInList( translucentList, STUDIO_TRANSPARENCY );
  ... DepthRange( 0, 1 ) ...
render->PopView( GetFrustum() );
```

`game/client/viewrender.cpp:1071-1157` [VALVE]

Points that matter:

- **`Push3DView` with `0` clear flags.** The colour and depth buffers are kept.
  This is not a separate render target composited afterwards — it is the same
  buffer, a new projection matrix, and a remapped depth range.
- **`zNear = 1` versus the world's near plane** (`GetZNear()`, typically 7 for a
  player). A near plane of 1 unit is what lets the weapon be genuinely close to
  the camera without clipping. This is a second, independent fix for the same
  family of problems the depth hack addresses, and both are needed: the depth
  range stops the *world* clipping the weapon, the near plane stops the
  *frustum* clipping it.
- The colour modulation and blend reset at line 1086-1088 is defensive — the
  scene pass leaves per-entity state on the render context.
- Opaque and translucent are two lists and two calls, with `STUDIO_TRANSPARENCY`
  on the second. Sorting within the view model is by list membership, not by
  distance; there are half a dozen pieces at most.
- **Portal opts out entirely**: `bUseDepthHack` is false near a portal, replaced
  by a real depth clear, with the comment *"the depth range hack doesn't work
  well enough for the portal mod (and messing with the depth hack values makes
  some models draw incorrectly)"* (`viewrender.cpp:1092-1097`). The trick has a
  known failure envelope and Valve documented where it ends.

### 4.3 Why the leaf system has a second list

```cpp
if ( IsViewModelRenderGroup( (RenderGroup_t)info.m_RenderGroup ) )
    AddToViewModelList( handle );
```

`game/client/clientleafsystem.cpp:634` [VALVE]

`RENDER_GROUP_VIEW_MODEL_OPAQUE` / `_TRANSLUCENT` renderables go into
`m_ViewModels`, a flat array, and `CollateViewModelRenderables` walks it
directly (`clientleafsystem.cpp:1516`). They are still inserted into the leaf
tree for other purposes, but **they never appear in the leaf-derived render
lists**, and nothing that enumerates renderables per leaf will find them.

This single fact explains most of §6 and §7. It is not a performance
optimisation — it is the structural statement that *the view model is not in the
world*, and every system that reasons about the world therefore cannot see it.

---

## 5. What the depth hack costs

### 5.1 Depth precision

Compressing into `[0, 0.1]` throws away 90% of the depth range, and with a
non-linear depth buffer the loss is not uniform. In practice the view model is
a handful of pieces spanning a few tens of units with a near plane of 1, so
there is precision to spare. **[inferred]** — Valve never state this; they state
the Portal failure instead, which is the case where it does bite.

### 5.2 Anything spawned at a bone is in the wrong place

This is the consequence people hit and rarely diagnose. The muzzle is at an
attachment on the view model, which lives under a 54° projection. A tracer,
muzzle flash, shell or smoke puff is a world-space effect under the 75°
projection. Put the effect at the attachment's world position and it is visibly
off — it will not line up with the barrel on screen.

Valve's fix warps the attachment position so that it projects to the same screen
point under the world projection that it did under the view model's:

```cpp
float worldx = tan( pViewSetup->fov          * M_PI/360.0 );
float viewx  = tan( pViewSetup->fovViewmodel * M_PI/360.0 );

// aspect ratio cancels out, so only need one factor
// the difference between the screen coordinates of the 2 systems is the ratio
// of the coefficients of the projection matrices (tan (fov/2) is that coefficient)
float factorX = viewx ? ( worldx / viewx ) : 0.0f;
float factorY = factorX;

Vector tmp = vOrigin - pViewSetup->origin;
Vector vTransformed( MainViewRight().Dot(tmp), MainViewUp().Dot(tmp), MainViewForward().Dot(tmp) );
vTransformed.x *= factorX;                 // squash X and Y, leave Z
vTransformed.y *= factorY;
vOrigin = pViewSetup->origin + (MainViewRight()*vTransformed.x)
                             + (MainViewUp()   *vTransformed.y)
                             + (MainViewForward()*vTransformed.z);
```

`game/client/c_baseviewmodel.cpp:49-93` [VALVE]

Transform into eye space, scale X and Y by the ratio of the two projections'
`tan(fov/2)`, leave Z alone, transform back. It is exact, it is four dot
products, and there is an inverse (`UncorrectViewModelAttachment`) for going the
other way — needed when something in the world must be converted *into* the view
model's space.

**If you build a view model system and do not build this, every effect that
originates on the weapon will be subtly misaligned, and it will look like an art
bug.** `C_ViewmodelAttachmentModel` overrides it too
(`econ_entity.cpp:903`), because the bonemerged weapon's attachments have the
same problem.

### 5.3 Any world effect that must draw over the weapon

Particles that belong to the weapon are pushed into
`RENDER_GROUP_VIEW_MODEL_TRANSLUCENT` so they join the second pass rather than
being buried by it — `particlemgr.cpp:1279`, `econ_entity.cpp:1774`,
`tf_player_shared.cpp:7591` (crit-boost effect), `tf_weapon_slap.cpp:308`.
Temp entities do the same (`c_te_legacytempents.cpp:3464`). So does the VGUI
screen on a weapon (`c_vguiscreen.cpp:216`). This is a real ongoing tax: every
new effect attached to a weapon must be told which world it lives in.

---

## 6. Lighting

### 6.1 The lighting origin override — the single most important line

```cpp
bool CTFViewModel::OnInternalDrawModel( ClientModelRenderInfo_t *pInfo )
{
    // Correct the ambient lighting position to match our owner entity
    if ( GetOwner() && pInfo )
    {
        pInfo->pLightingOrigin = &( GetOwner()->WorldSpaceCenter() );
    }
    return BaseClass::OnInternalDrawModel( pInfo );
}
```

`game/shared/tf/tf_viewmodel.cpp:279-288` [VALVE]

Source lights a studio model by sampling the light cache — ambient cube plus the
strongest local lights — at **one point**, and `pLightingOrigin` is that point.
By default it is the model's render origin. For a view model the render origin
is the eye, and the eye is a bad sample point: it is a foot from a wall when you
stand against one, it is on the far side of a doorway when you lean out, and it
can be inside solid space entirely. Sampling there gives a weapon that flickers
black as you brush past geometry.

Sampling at the **owner's world-space centre** — the player's chest — gives a
weapon lit exactly as the player's body is lit. That is both more stable and
more *correct*: the answer to "how is this gun lit" is "however the man holding
it is lit". It also guarantees the first-person and third-person views of the
same weapon agree, which is what makes a spectator's view match the player's.

This is a two-line fix that most engines have no equivalent hook for, and it is
worth stealing on its own.

### 6.2 What the view model consequently gets, and does not

| | |
|---|---|
| Ambient cube at the player's chest | **yes** |
| Static and dynamic light sources near the player | **yes**, via the same light cache entry |
| Lightmap-baked bounce for the room the player is in | **yes**, that is what the cache holds |
| Per-pixel lighting from the model's own normals | **yes** — `VertexLitGeneric` is the same shader the world models use |
| Shadows cast onto it by world geometry | **no** — §7 |
| Shadow cast by it onto the world | **no** — §7 |
| Bloom and tonemapping | **yes** — drawn before `DoEnginePostProcessing` (§4.1) |
| Camera motion blur | **no**, by design |
| Refraction behind it | **yes**, `UpdateRefractIfNeededByList` runs the refract update inside the view model pass |

The important row is the fourth. **The weapon is not specially shaded.** It goes
through `VertexLitGeneric` exactly like any world prop, with the standard normal
map, phong, rim and env-map path. Nothing about being a view model changes the
material. What changes is *where the light is sampled* and *how the depth is
written* — and those are the two smallest possible interventions that could
achieve the effect. That is the design lesson: keep the shading path identical
and intervene only in the two places the geometry genuinely differs.

### 6.3 Cloaking, and why the material knows about the view model

The Spy's invisibility is a material proxy, `CInvisProxy::OnBind`
(`tf_viewmodel.cpp:521`), which walks up from the bound entity to find the
owning player. It has a **different curve for the view model than for the world
model**:

```cpp
#define TF_VM_MIN_INVIS 0.22
#define TF_VM_MAX_INVIS 0.5
flWeaponInvis = ( flPercentInvisible < 0.01 ) ? 0.0
              : RemapVal( flPercentInvisible, 0.0, 1.0, TF_VM_MIN_INVIS, TF_VM_MAX_INVIS );
```

`tf_viewmodel.cpp:483-486` [VALVE] — a fully cloaked Spy is 100% invisible to
everyone else but only 50% invisible to himself, and never less than 22% once
cloaking starts. The player must still be able to see his own hands. A gameplay
decision that only exists because the first-person and third-person
representations are separate objects.

---

## 7. Shadows — the honest answer

**Source 1 view models neither cast nor receive shadows, and the reason is
structural rather than a decision.**

### 7.1 It does not receive them

The flashlight — Source's projected texture, and the only per-pixel shadowing
that lands on models — builds its receiver set from **leaves**:

```cpp
BuildFlashlightLeafList( &leafList, shadow.m_WorldToShadow );
...
ClientLeafSystem()->ProjectFlashlight( shadow.m_ClientLeafShadowHandle, nCount, pLeafList );
```

`game/client/clientshadowmgr.cpp:2644-2667` [VALVE]

Renderables are gathered per leaf and offered `ShouldReceiveProjectedTextures`
(`clientshadowmgr.cpp:3452,3535`). The view model is in `m_ViewModels`, drawn
from a flat list that no leaf enumeration touches (§4.3), so it is never a
candidate. There is no `if (IsViewModel()) return false` anywhere — it simply
cannot be reached. **[VALVE]** for the mechanism, **[inferred]** for the
conclusion, which follows from it directly.

The only exception in the code is `bLightSpecificEntity`, where a flashlight
targeting one entity adds that entity and its move-children directly
(`clientshadowmgr.cpp:2671-2698`). Since the view model *is* a move-child of
the player, a targeted flashlight is the one path by which it could receive one
— an edge case, not the general lighting path.

### 7.2 It does not cast them

`C_BaseAnimating::ShadowCastType` (`c_baseanimating.cpp:820`) would return
`SHADOWS_RENDER_TO_TEXTURE_DYNAMIC` for a model with pose parameters, so a view
model nominally qualifies. But Source's render-to-texture shadows are projected
into the world through the leaf system, and the caster's shadow is applied to
receivers found *in leaves*. The weapon is at the eye, its shadow would fall
under the player's feet where the player's own shadow already is, and the player
model is not drawn in first person anyway (`ShouldDrawLocalPlayer()`,
`viewrender.cpp:984`).

TF2 additionally suppresses the world model's shadow whenever the weapon is not
actively held:

```cpp
ShadowType_t CTFWeaponBase::ShadowCastType( void )
{
    // Some weapons (fists) don't actually get set to NODRAW when holstered so we
    // need some extra checks
    if ( IsEffectActive( EF_NODRAW | EF_NOSHADOW ) || m_iState != WEAPON_IS_ACTIVE )
        return SHADOWS_NONE;
    return BaseClass::ShadowCastType();
}
```

`game/shared/tf/tf_weaponbase.cpp:4572-4580` [VALVE]

### 7.3 So how does it look lit and grounded?

Because §6.1 does the work. The weapon carries the ambient cube and local lights
of the room the player is standing in, which is 90% of what "correct lighting"
reads as. Standing in shade darkens the gun because the *sample point* is in
shade — not because a shadow map covers it.

**This is the trade the note is really about.** Valve bought clip-freedom and
stable lighting for two lines of code, and paid for it with no shadow
interaction at all. TF2 can afford that: it is stylised, brightly and evenly lit,
and has no dynamic sun. A game with a moving sun and hard shadows cannot, and
that is precisely why Epic eventually built something more elaborate (§10.1).

---

## 8. The animation pipeline

The chain from "player pressed fire" to "the gun's hands move", both directions,
including how it survives prediction.

### 8.1 Activity → sequence

Weapon code never names an animation. It names an **Activity** — a semantic
label — and the model resolves it:

```
CTFWeaponBase::PrimaryAttack()
 └─ SendWeaponAnim( ACT_VM_PRIMARYATTACK )                tf_weaponbase.cpp:783
     └─ CTFWeaponBase::SendWeaponAnim  — inspect-state gate, then base
         └─ CBaseCombatWeapon::SendWeaponAnim              basecombatweapon_shared.cpp:1213
             ├─ TranslateViewmodelHandActivity( act )      ← §8.2, the c_model remap
             └─ SetIdealActivity( act )                    basecombatweapon_shared.cpp:2393
                 ├─ idealSequence = SelectWeightedSequence( ideal )
                 ├─ nextSequence  = FindTransitionSequence( GetSequence(), idealSequence )
                 └─ SendViewModelMatchingSequence( seq )    baseviewmodel_shared.cpp:357
```

Three things fall out of this that are easy to get wrong:

- **`SelectWeightedSequence`** — an activity can map to several sequences with
  weights, so `ACT_VM_PRIMARYATTACK` picks a random one of N fire animations
  with no code involved. Variation is a model-authoring decision.
- **`FindTransitionSequence`** — the model may declare intermediate sequences
  between two states, and the weapon walks the chain via `ACT_TRANSITION` and
  `MaintainIdealActivity` (`basecombatweapon_shared.cpp:2371`), which advances
  only when `IsViewModelSequenceFinished()`. A tiny, data-driven state machine
  living in the `.mdl`, not in code.
- **Draw is exempt** from transitions (`ideal != ACT_VM_DRAW`,
  `basecombatweapon_shared.cpp:2409`) — pulling out a weapon must be immediate.

### 8.2 The activity remap table — how one arm rig serves every weapon

This is the piece that makes TF2's cosmetics economy possible, and Valve's
comment states the problem exactly:

```cpp
// Remaps viewmodel activities to specific ones for the weapon role.
// Needed this for weapons that bonemerge themselves to the hand models to create their viewmodel.
// The hand model needs to have all the animations, and be able to choose the right anims to play for the active weapon.
// We use this acttable to remap the base viewmodel anims to the right one for the weapon.
viewmodelacttable_t s_viewmodelacttable[] =
{
    { ACT_VM_DRAW,          ACT_PRIMARY_VM_DRAW,      TF_WPN_TYPE_PRIMARY },
    { ACT_VM_IDLE,          ACT_PRIMARY_VM_IDLE,      TF_WPN_TYPE_PRIMARY },
    ...
    { ACT_VM_DRAW,          ACT_MELEE_VM_DRAW,        TF_WPN_TYPE_MELEE   },
    ...
};
```

`game/shared/tf/tf_weaponbase.cpp:4282-4508` [VALVE] — roughly 220 rows.

The model that plays animations is the **class's hands**, not the weapon. One
rig per class holds every animation that class can perform. A weapon says
"idle"; the table converts that to "idle, in the primary-weapon role", and the
hands play their primary idle. Add a new shotgun and it needs no animations at
all — it inherits the Engineer's primary set by declaring its role.

`TranslateViewmodelHandActivityInternal` (`tf_weaponbase.cpp:4513`) resolves in
priority order:

1. A per-item **activity override** from the econ item's static data.
2. A per-item **animation slot** override (`GetAnimationSlot()`), which lets an
   item borrow another role's animations — this is how a melee weapon can be
   held like a primary.
3. Otherwise, the weapon's declared role.

### 8.3 Animation events

Sounds, particles and effects are keyed on animation frames in the `.mdl`, and
the view model's dispatch is overridden:

```cpp
void C_BaseViewModel::FireEvent( const Vector& origin, const QAngle& angles, int event, const char *options )
{
    // We override sound requests so that we can play them locally on the owning player
    if ( ( event == AE_CL_PLAYSOUND ) || ( event == CL_EVENT_SOUND ) )
    {
        if ( GetOwner() != NULL )
        {
            CLocalPlayerFilter filter;
            EmitSound( filter, GetOwner()->GetSoundSourceIndex(), options, &GetAbsOrigin() );
            return;
        }
    }
    C_BaseCombatWeapon *pWeapon = GetActiveWeapon();
    if ( pWeapon )
    {
        bool bResult = pWeapon->OnFireEvent( this, origin, angles, event, options );
        if ( !bResult )
            BaseClass::FireEvent( origin, angles, event, options );
    }
}
```

`game/client/c_baseviewmodel.cpp:136-164` [VALVE]

Two redirections. Sound is emitted **through the owning player's sound source
index**, and only to a `CLocalPlayerFilter` — so a reload click comes from you,
is heard only by you, and does not spatialise from a position two feet in front
of the camera. Everything else is offered to the *weapon* first, which is what
lets a weapon reinterpret a shared animation's events.

`CTFViewModel::ModifyEventParticles` (`tf_viewmodel.cpp:406`) forwards to the
weapon so an item can substitute its own particle for a named one — the
mechanism behind Unusual and festive effects reusing stock animations.

### 8.4 Prediction, parity, and cycle extrapolation

The view model is a **predicted entity** (`BEGIN_PREDICTION_DATA`,
`baseviewmodel_shared.cpp:601`), so firing starts the animation on the client
immediately rather than after a round trip. Three mechanisms keep that honest:

**Animation parity.** A 3-bit counter, networked:

```cpp
#define VIEWMODEL_ANIMATION_PARITY_BITS 3
m_nAnimationParity = ( m_nAnimationParity + 1 ) & ( (1<<VIEWMODEL_ANIMATION_PARITY_BITS) - 1 );
```

`baseviewmodel_shared.cpp:33,363` [VALVE]

Sending a sequence number is not enough — firing twice in a row sends the same
sequence and the client has no way to know it should restart. The parity bump
says "this is a *new* play of that sequence". Three bits is enough because you
cannot get eight behind without other things breaking. `UpdateAnimationParity`
runs at the top of `Interpolate` and resets the animation when it changes
(`c_baseviewmodel.cpp:170`).

**A receive proxy that resets on change.** `RecvProxy_SequenceNum`
(`baseviewmodel_shared.cpp:622`) sets cycle to 0 and stamps `m_flAnimTime` when
the sequence differs; `RecvProxy_Weapon` (line 533) does the same when the
server switches the weapon underneath the client.

**Cycle extrapolation against predicted time.**

```cpp
float elapsed_time = currentTime - m_flAnimTime;
if ( GetPredictable() || IsClientCreated() )
{
    float curtime = pPlayer->GetFinalPredictedTime();
    elapsed_time = curtime - m_flAnimTime;
    if ( !engine->IsPaused() )
        elapsed_time += ( gpGlobals->interpolation_amount * TICK_INTERVAL );
}
if ( elapsed_time < 0 ) elapsed_time = 0;     // "Prediction errors?"
float dt = elapsed_time * GetSequenceCycleRate(...) * GetPlaybackRate();
```

`game/client/c_baseviewmodel.cpp:166-211` [VALVE]

A predicted view model advances against the player's **final predicted time**
plus the sub-tick interpolation fraction, not against `curtime`. Without this
the weapon animates at tick rate whilst the world renders at frame rate, and a
66 Hz reload judders on a 144 Hz monitor. The `elapsed_time < 0` clamp is
labelled `// Prediction errors?` — an admission that it happens.

The whole view model is also flagged `ENTCLIENTFLAG_ALWAYS_INTERPOLATE`
(`baseviewmodel_shared.cpp:44`) so it never has interpolation skipped as an
optimisation.

### 8.5 The bone-access window

```cpp
C_BaseAnimating::PopBoneAccess( "OnRenderStart->CViewRender::SetUpView" );
C_BaseAnimating::PushAllowBoneAccess( true, true, "CViewRender::SetUpView->OnRenderEnd" );
```

`game/client/view.cpp:774-775` [VALVE]

Source maintains a stack of "may bones be read right now", with **separate flags
for normal models and view models** (`c_baseanimating.cpp:3080-3106`), asserting
on violation. View model bones are only valid between `SetUpView` and
`OnRenderEnd`, because before `SetUpView` the view model has no transform — the
camera has not been computed yet.

This is a debugging affordance, not a feature, and it is the kind of thing a
codebase only builds after being bitten. Reading a view model attachment from
game code at the wrong point in the frame gives a *stale* answer, not a wrong
one, and stale answers are nearly impossible to find by inspection. The assert
converts a subtle visual bug into a stack trace.

### 8.6 Per-frame procedural bone overrides

```cpp
void CTFViewModel::StandardBlendingRules( CStudioHdr *hdr, Vector pos[], Quaternion q[], ... )
{
    BaseClass::StandardBlendingRules( hdr, pos, q, currentTime, boneMask );
    if ( pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN )
    {
        int iBarrelBone = Studio_BoneIndexByName( hdr, "v_minigun_barrel" );
        if ( iBarrelBone != -1 && ( hdr->boneFlags( iBarrelBone ) & boneMask ) )
        {
            RadianEuler a;
            QuaternionAngles( q[iBarrelBone], a );
            a.x = pMinigun->GetBarrelRotation();     // simulation state, not animation
            AngleQuaternion( a, q[iBarrelBone] );
        }
    }
}
```

`game/shared/tf/tf_viewmodel.cpp:313-343` [VALVE]

The hook is *after* the animation has been sampled and *before* bones are built
to matrices, and it writes a local rotation directly. The minigun's spin-up is
simulation state, not an animation, so it cannot be a sequence — but it must
compose with whatever sequence is playing. Note the `boneFlags & boneMask` test:
if this bone was not requested at the current LOD, the write is skipped rather
than performed on a bone that will not be used.

`GetBoneControllers` (`c_baseviewmodel.cpp:510`) is the declarative version of
the same idea — the weapon fills in bone controller values each frame.

---

## 9. TF2's c_model architecture — arms and weapon as separate objects

TF2's later weapons ("c_models", for the `c_` filename prefix) are not one
model. The **view model entity holds the class's hands**; the weapon is a
separate client-side entity **bonemerged onto them**.

```cpp
const char *CTFWeaponBase::GetViewModel( int iViewModel ) const
{
    const CEconItemView *pItem = GetAttributeContainer()->GetItem();
    if ( pPlayer && pItem->IsValid() && pItem->GetStaticData()->ShouldAttachToHands() )
    {
        const char *pszHandModel = pPlayer->GetPlayerClass()->GetHandModelName( iHandModelIndex );
        return pszHandModel;                       // ← the ARMS are the view model
    }
    return GetTFWpnData().szViewModel;             // ← legacy: the WEAPON is the view model
}
```

`game/shared/tf/tf_weaponbase.cpp:651-676` [VALVE]

The weapon model is then created as a client-only entity parented to it:

```cpp
C_ViewmodelAttachmentModel *pEnt = new class C_ViewmodelAttachmentModel;
pEnt->SetOuter( this );
pEnt->InitializeAsClientEntity( pItem->GetPlayerDisplayModel( iClass, team ),
                                RENDER_GROUP_VIEW_MODEL_OPAQUE );
m_hViewmodelAttachment = pEnt;
m_hViewmodelAttachment->SetParent( vm );
m_hViewmodelAttachment->SetLocalOrigin( vec3_origin );
```

`game/shared/econ/econ_entity.cpp:1153-1175` [VALVE]

```cpp
bool C_ViewmodelAttachmentModel::InitializeAsClientEntity( ... )
{
    AddEffects( EF_BONEMERGE );
    AddEffects( EF_BONEMERGE_FASTCULL );
    // Invisible by default, and made visible->drawn->made invisible when the viewmodel is drawn
    AddEffects( EF_NODRAW );
    return true;
}
```

`econ_entity.cpp:843-854` [VALVE]

### 9.1 What this buys

- **One animation set per class, not per weapon.** The Engineer's hands hold
  every animation an Engineer can perform; each weapon declares a role and the
  table in §8.2 picks the right one. New weapons ship with no animation work.
- **Class-correct hands with every weapon.** The Heavy's hands are the Heavy's
  hands even holding a Scout's weapon — because the hands *are* the view model.
- **Cosmetics compose.** Attached models (`DrawEconEntityAttachedModels`,
  `tf_viewmodel.cpp:304`), StatTrak counters
  (`c_baseviewmodel.cpp:326-334`, `tf_weaponbase.cpp:6872`), festive variants
  and Unusual effects are all further bonemerged children. Each is opted in per
  item definition.
- **The gunslinger** replaces the Engineer's hands entirely by returning a
  different `iHandModelIndex` — the whole hand rig swaps, from one attribute.

### 9.2 The `EF_NODRAW` dance

Note the comment at `econ_entity.cpp:851` and the code at
`c_baseviewmodel.cpp:328-333`:

```cpp
pTFWeapon->m_viewmodelStatTrakAddon->RemoveEffects( EF_NODRAW );
pTFWeapon->m_viewmodelStatTrakAddon->DrawModel( flags );
pTFWeapon->m_viewmodelStatTrakAddon->AddEffects( EF_NODRAW );
```

The addon is permanently `EF_NODRAW` and is un-hidden, drawn, and re-hidden
inside its parent's draw. This guarantees it is drawn **exactly once, from
inside the view model pass**, and can never be picked up by any other
enumeration. Crude, and completely reliable — the same defensive instinct as
putting view models in their own list.

### 9.3 Handedness

Flipping is a **negation of the second row of the bone matrix in view space**,
not a mesh mirror:

```cpp
void C_BaseViewModel::ApplyBoneMatrixTransform( matrix3x4_t& transform )
{
    if ( ShouldFlipViewModel() )
    {
        const CViewSetup *pSetup = view->GetPlayerViewSetup();
        AngleMatrix( pSetup->angles, pSetup->origin, viewMatrixInverse );
        MatrixInvert( viewMatrixInverse, viewMatrix );
        ConcatTransforms( viewMatrix, transform, temp );
        temp[1][0] = -temp[1][0];  temp[1][1] = -temp[1][1];      // flip along X
        temp[1][2] = -temp[1][2];  temp[1][3] = -temp[1][3];
        ConcatTransforms( viewMatrixInverse, temp, transform );
    }
}
```

`game/client/c_baseviewmodel.cpp:239-272` [VALVE]

A mirrored matrix reverses winding, so the cull mode is flipped for the draw and
restored after (`c_baseviewmodel.cpp:371-382`, and again for the attachment at
`econ_entity.cpp:856-872`). The commented-out `mScale` matrix version is left in
place with the note that it is *"the slower way to do it"* — same result, three
multiplies instead of four negations.

---

## 10. Why this is hard elsewhere

### 10.1 Unreal

Until recently there was no engine answer, and the shipped workarounds were all
bad in a documented way: a second `SceneCapture` (expensive, and the capture is
lit separately), a custom near clip plane per-primitive, or shrinking the weapon
mesh and parenting it to the camera **[COMMUNITY]**. That last one is the same
idea as Unity's (§10.2) and has the same flaw.

Epic shipped a native solution in UE 5.5 and it has grown since. In current
docs, a primitive gets a **First Person Primitive Type** **[EPIC]**:

| Setting | Behaviour |
|---|---|
| `None` | ordinary |
| `First Person` | rendered with a custom FOV and a scale factor; **does not cast scene shadows** |
| `World Space Representation` | invisible to the first-person camera; **casts shadows and appears in reflections** |

Plus `Enable First Person Field Of View` (a horizontal FOV override) and
`Enable First Person Scale` (shrink toward the camera to avoid intersection).
Self-shadowing is a screen-space trace (`r.FirstPerson.SelfShadow`).

That is worth reading carefully, because it is the same architecture as Source's
with the shadow gap filled: **the first-person mesh and its world-space
shadow-casting proxy are two different objects**. Valve simply omitted the
second one.

The limitations are instructive too **[EPIC]**: the advanced features need a
**GBuffer bit**, which requires static lighting disabled and **does not work on
the mobile or forward renderers**; and without Virtual Shadow Maps there are no
first-person shadows on the ground. So the general solution costs a GBuffer bit
and a deferred renderer — a real price, and the reason it took Epic that long.

### 10.2 Unity

There is no engine feature. The universal recipe is **two cameras**: depth 0
draws the world, depth 1 draws only the weapon layer with `Clear Flags: Depth
only` **[COMMUNITY]**. It solves clipping and breaks almost everything else:

- Real-time shadows on the weapon are lost — the second camera is a separate
  render with its own lighting **[COMMUNITY]**.
- Post-processing runs per camera, so the weapon is either excluded from bloom
  and tonemapping or gets its own second pass at a different exposure.
- Deferred lighting is computed twice, or the weapon camera falls back to
  forward, and the two disagree.

The alternative advice is "make the weapon tiny and keep it inside the player's
collider" **[COMMUNITY]**, which is the same as Epic's *First Person Scale*
without the FOV override — it keeps one camera and therefore keeps lighting and
post, but it distorts the weapon's perspective and only pushes the clipping
problem back rather than removing it.

### 10.3 The comparison, condensed

| | Source 1 (TF2) | Unity two-camera | UE 5.5+ |
|---|---|---|---|
| Never clips geometry | depth range `[0,0.1]` | depth-only clear | per-primitive FOV + scale |
| Its own FOV | `fovViewmodel`, tracked by delta | second camera's FOV | `First Person FOV` |
| Same lighting as the world | yes — one shading path, moved sample point | **no** | yes |
| In the scene's post/tonemap | yes | **usually no** | yes |
| Casts shadows into the world | **no** | no | yes, via a world-space proxy mesh |
| Receives shadows | **no** | no | self-shadow (screen space) |
| Cost | two lines and a second `Push3DView` | a whole extra camera | a GBuffer bit; deferred only |

Valve's is the cheapest and gets four of six. The two it misses are the two that
matter most in a realistically-lit game, and that is the honest summary.

---

## 11. What this means for `cromwell`

Judged against the three target genres, this is **FPS-specific and a third-person
shooter wants none of it** — a third-person weapon is an ordinary attachment on
an ordinary skeleton, in the world, with no special case at all. An RTS wants
none of it either. So the question is not "should the engine have a view model
system" but "which of these ideas are general".

**Three are general and worth taking now:**

1. **The lighting-origin indirection.** A model's light sample point should be a
   settable field, defaulting to the render origin, not hard-wired to it. It
   costs one pointer in the render-info struct and it is the fix for view
   models, for anything attached to a moving platform, for a model straddling a
   lighting boundary, and for keeping a group of props consistently lit. This is
   `pLightingOrigin` and it is two lines. It should exist before there is a
   caller for it.

2. **A renderable that is not in the spatial index.** `m_ViewModels` is the
   statement "this thing is drawn but is not in the world", and the engine
   benefits from having that concept explicitly rather than as a flag checked in
   nine places. Debug gizmos, editor overlays, held-item previews and the dev
   panel's 3D content all want it. Our `study/topics/agents/spatial_queries.md` discipline of
   "derived data must not be reachable by the slow path" is the same instinct.

3. **The attachment-warp function (§5.2).** Any time two projections coexist in
   one frame, positions must be converted between them, and the conversion is
   four dot products and one ratio. Worth having as a utility the moment a second
   projection exists — including for our own case of drawing UI-space geometry
   at world positions.

**One is worth knowing about and not building:** the depth-range trick itself.
It is trivially cheap and would be correct to use if this project ever gets a
first-person mode, but building it before then is speculative. Note it needs a
`DepthRange` on whatever abstraction sits over GL — `glDepthRange` is a state we
do not currently touch.

**One is a warning.** §7 is the cost, and Epic's answer (§10.1) shows what
paying it properly costs: a GBuffer bit and a deferred renderer. If a
first-person mode ever matters here, **decide the shadow question first**,
because it determines the architecture. The Source answer is only acceptable in
a game whose lighting is flat enough that nobody notices — and per
`study/games/valve/source2_rendering.md`, this renderer is aiming somewhere considerably
less forgiving than TF2.

**And one is a general lesson, independent of first-person anything.** The
reason the Source view model works is that it changes **two** things — where
light is sampled and how depth is written — and changes *nothing* about the
shading path. The material, the shader, the bone pipeline and the animation
system are all byte-identical to a world model's. Every engine that struggles
with this problem struggles because it forked the pipeline instead. That is the
same rule as this project's derived-cache discipline in CLAUDE.md: the fast path
may only skip work that provably does nothing, and the slow path stays the
original code, unmodified.

---

## Sources

**Primary — code, all [VALVE]**, `ValveSoftware/source-sdk-2013` @ `master`,
pushed 2026-08-06:

| Area | Files |
|---|---|
| Camera | `game/client/view.cpp`, `game/shared/baseplayer_shared.cpp` |
| Render path | `game/client/viewrender.cpp`, `game/client/clientleafsystem.cpp` |
| View model core | `game/shared/baseviewmodel_shared.cpp`, `game/client/c_baseviewmodel.cpp` |
| TF2 view model | `game/shared/tf/tf_viewmodel.cpp`, `game/client/tf/clientmode_tf.cpp` |
| Animation | `game/shared/basecombatweapon_shared.cpp`, `game/shared/tf/tf_weaponbase.cpp` |
| Bonemerged c_models | `game/shared/econ/econ_entity.cpp` |
| Shadows | `game/client/clientshadowmgr.cpp`, `game/client/c_baseanimating.cpp` |

**Secondary:**

- [Epic — First Person Rendering, UE 5.8 documentation](https://dev.epicgames.com/documentation/unreal-engine/first-person-rendering?lang=en-US) **[EPIC]**
- [Sahil Dhanju — Render First-Person Meshes with a Separate FOV](https://sahildhanju.com/posts/render-first-person-fov/) **[COMMUNITY]**
- [GameDev.net — First person weapon with different FOV in a deferred engine](https://www.gamedev.net/forums/topic/605666-first-person-weapon-with-different-fov-in-deferred-engine/) **[COMMUNITY]**
- [Unity Discussions — Rendering of first person weapon model](https://discussions.unity.com/t/rendering-of-first-person-weapon-model/719196) **[COMMUNITY]**
- [Unity Answers — First person weapons clip with terrain geometry](https://answers.unity.com/questions/50879/first-person-weapons-clip-with-terrain-geometry.html) **[COMMUNITY]**
- [Peripeteia — First person renderer](https://90s.graphics/2021/03/13/first-person-renderer/) **[COMMUNITY]**

**Related notes in this directory:** `source2_rendering.md` for where this
renderer's lighting is going and why §7's trade would not survive it;
`spatial_queries.md` for the derived-cache discipline §11 refers to.
