# Broken Arrow — the same audio problem, bought instead of built

The control experiment for [`nuclear_option_audio.md`](nuclear_option_audio.md).
That note read a game that solved fast-vehicle and long-distance battlefield audio
**with no middleware at all**, in about 250 lines of stock Unity. This one reads a
game in the same engine, from the same era, that bought **FMOD Studio and Google
Resonance** and ships **3.0 GB of audio banks**.

The question the two notes answer together is not "which is better". It is
**what changes about your codebase when the audio design moves out of it**, and the
answer turns out to be sharper than I expected: with middleware, *the interesting
parts become invisible*. Doppler curves, distance attenuation, layer crossfades and
mix ducking are all authored in FMOD Studio and baked into banks. I can read the
taxonomy and I can read the code that drives it, but I cannot read the behaviour —
and neither can anyone else on the team who is not the sound designer. §11 is what
that trade actually buys and costs.

> **Method and its limits.** Broken Arrow is **IL2CPP**, so there is no assembly to
> decompile. Everything here is read from the retail install: bank files and their
> sizes, the FMOD `.strings.bank` string table, the IL2CPP metadata identifier
> table, the native plugin DLLs, and the shipped `GameLogs/`.
>
> The governing caveat from [`broken_arrow.md`](broken_arrow.md) §4.1 applies with
> full force. **The metadata identifier table proves a name was compiled in, not
> that it is used.** The FMOD Unity integration compiles its entire 178-function
> P/Invoke binding whether or not the game calls any of it. So this note grades
> evidence: **[BUILD]** compiled in or present on disk, **[TRACE]** observed in a
> runtime log and therefore demonstrably ran, **[inferred]** my reading. Where
> BUILD and TRACE disagree, TRACE wins.
>
> Nothing here comes from listening to the game.

Related: [`nuclear_option_audio.md`](nuclear_option_audio.md) (the A/B),
[`broken_arrow.md`](broken_arrow.md) (the parent note — same install, same method),
[`ruse.md`](ruse.md).

---

## 1. The manifest

**[BUILD]**

| | |
|---|---|
| Middleware | **FMOD Studio** (`fmodstudio.dll`) + the FMOD Unity integration |
| Spatialiser | **Google Resonance Audio**, as an FMOD plugin (`resonanceaudio.dll`, 840 KB) |
| Also present | `xaudio2_9redist.dll` (the Windows XAudio2 redistributable) |
| Banks | **30 `.bank` files, 3,038 MB total** |
| Install | 54 GB |
| Engine | Unity, IL2CPP, HDRP |

**[inferred] The 3.0 GB is the number to hold on to.** Broken Arrow's audio alone
is **1.3× the entire Nuclear Option install** (2.3 GB, of which audio is a small
fraction of `data.unity3d`). Audio is 5.6% of Broken Arrow's install — which sounds
modest until you notice that Nuclear Option's *whole game* would fit inside it
twice.

`resonanceaudio.dll` is specifically the **FMOD plugin build** of Resonance, not
the standalone Unity one — its exports are `FMODGetPluginDescriptionList`,
`FMOD_ResonanceAudioListener_GetDSPDescription`,
`FMOD_ResonanceAudioSource_GetDSPDescription` and
`FMOD_ResonanceAudioSoundfield_GetDSPDescription`, and internally it carries
Google's `vraudio::AmbisonicBinauralDecoderNode` and
`vraudio::AmbisonicMixingEncoderNode`. So it is registered as a DSP inside FMOD's
graph rather than sitting beside it. **[inferred]** That is the correct way to
combine the two, and it means the spatialisation choice is made per-event in FMOD
Studio by the sound designer, not in code.

---

## 2. Where three gigabytes went

**[BUILD]** Every bank, by size:

| Bank | MB | | Bank | MB |
|---|---:|---|---|---:|
| `Music_Campaign` | 352.6 | | `Music_Game1SoftScript` | 56.0 |
| `Dialog` | 293.0 | | `Music_Loading` | 22.3 |
| `Voice_Command` | 291.3 | | `Voice_Commannd_BB_DLC` | 19.8 |
| `Music_Hangar` | 242.8 | | `SFX_Unit_BB_DLC` | 19.1 |
| `Music_Base_Missions` | 218.7 | | `Music_Game2HardScript` | 15.9 |
| `Music_Multi3A` | 186.7 | | `Music_Victory-Defeat` | 8.1 |
| `Music_Engine_Zombies!` | 180.3 | | `Zombies_Mission` | 7.1 |
| `Music_Multi4G` | 171.1 | | `UI` | 4.5 |
| `Music_Mutli1E` | 167.6 | | `Master` | 4.3 |
| `Music_Multi2D` | 157.6 | | `Music_Transitions` | 4.1 |
| `Ambience` | 142.9 | | `Cut_BB_DLC` | 3.5 |
| `Dialog_BB_DLC` | 140.6 | | `SFX_Weapon_BB_DLC` | 2.8 |
| `SFX_Unit` | 126.9 | | `0 Screen` | 2.6 |
| `SFX_Weapon` | 116.1 | | `Master.strings` | 0.2 |
| `BB_Mission` | 80.0 | | `CustomDialog` | 0.0 |

Grouped:

| Category | MB | Share |
|---|---:|---:|
| **Music** (11 banks) | **1,578** | **51.9%** |
| **Voice and dialogue** (4 banks) | **745** | **24.5%** |
| Ambience | 143 | 4.7% |
| **SFX — units and weapons** (4 banks) | **265** | **8.7%** |
| Mission, cutscene, UI, master, misc | 307 | 10.1% |

**[inferred] This is the single most surprising number in the note. The actual
gunfire, engines and explosions — the thing you would assume dominates a war
game's audio — is 8.7% of the audio budget. Music is six times larger.**

Read carefully, that is not a criticism, it is a statement about what kind of game
this is. An RTS camera sits above the battlefield; the player hears a wash of
combat, not individual weapons at close range. What carries the experience minute
to minute is the score and the voice — 76% of the budget between them. A
first-person flight sim has the opposite distribution because the player is *in*
the machine and there is no score playing over a dogfight.

**[inferred] The corollary is uncomfortable and worth stating.** The three problems
[`nuclear_option_audio.md`](nuclear_option_audio.md) opens with — Doppler at high
Mach, sound travel delay, supersonic silence — are problems you only have if the
player's ear is near a fast vehicle. Broken Arrow bought two audio packages and
spent 3 GB, and **none of that budget goes anywhere near those three problems**,
because it does not have them. The middleware is not solving the hard audio
problem; it is solving the *large content* problem.

`CustomDialog.bank` is 0.0 MB — an empty bank shipped as a hook (see §7).

---

## 3. The bank taxonomy is the loading strategy

**[BUILD]** The naming is systematic and it is a loading plan, not a filing
system:

- **`Master.bank` + `Master.strings.bank`** — always loaded. The strings bank
  (164 KB) holds the event-path table.
- **One bank per music context** — `Music_Campaign`, `Music_Hangar`,
  `Music_Base_Missions`, `Music_Loading`, `Music_Victory-Defeat`,
  `Music_Transitions`, and **four numbered multiplayer banks**
  (`Music_Multi2D`, `Music_Multi3A`, `Music_Multi4G`, `Music_Mutli1E`).
- **Two "script" banks** — `Music_Game1SoftScript` (56 MB) and
  `Music_Game2HardScript` (16 MB).
- **`Music_Engine_Zombies!`** (180 MB) and `Zombies_Mission` — a whole music engine
  for a side mode.
- **`SFX_Unit` / `SFX_Weapon` split**, each with a `_BB_DLC` sibling.
- **`Voice_Command`, `Dialog`, `CustomDialog`**, plus `_BB_DLC` variants.
- **`0 Screen.bank`** — the leading zero forces it to sort first; **[inferred]** the
  front-end bank, loaded before anything else.

**[inferred] Two things follow.** The `_BB_DLC` suffix on five separate banks means
**DLC audio is additive at the bank level** — the DLC ships four extra banks and the
base game's banks are untouched, which is exactly what bank granularity is for and
is much harder to arrange if your audio is Unity `AudioClip`s inside asset bundles.
And the four numbered multiplayer music banks plus `Music_Transitions` plus a
`Stingers+Transitions/` event group (§4) is a **horizontal re-sequencing music
system** — several full-length interchangeable scores with authored transition
material between them.

The shipped typos (`Music_Mutli1E`, `Voice_Commannd_BB_DLC`) are worth one line:
bank names are *content*, authored by the sound designer in FMOD Studio, and they
end up in the shipped filesystem. Nobody code-reviews a bank name.

---

## 4. The event vocabulary

**[BUILD]** `Master.strings.bank` is a `RIFF`/`FEV `/`FMT ` container whose string
table is **prefix-compressed** — full event paths are reconstructed by
concatenating fragments, so I cannot recover complete paths, but the 5,337
fragments *are* the vocabulary. The root is `event:/BrokenArrowGame/`.

Group fragments recovered:

```
Ambience_Group/   Aircraft_Turbine/   Helicopter_Rotor/   Shell_Explosion_Impact/
Shooting/         Rockets/            Death/              Command/
INFANTRY/  Infantry/  VEHICLE/  VEH/  PLANE/  AIR/  HELO/  HIPS/  DRONES/
ARTY/  BOOMER/  SMOKER/  WALKER/  SNIPERS/  TRUCK/  TANK/  ZOMBIE/
Music/  Stingers+Transitions/  Base_Missions/  Cutscenes/  Cinematics/
Dialog/BalticWar/  MSC_Studio/  The_Professional/  MechanicsSounds/
```

**[inferred] The taxonomy is by *unit role*, not by sound type** — `INFANTRY/`,
`VEHICLE/`, `PLANE/`, `HELO/`, `ARTY/`, `DRONES/` — which is the shape you get when
the sound designer's mental model is "what unit is this" and the game's is the
same. Compare Nuclear Option, where the equivalent grouping is by *component*
(`TurbineEngine`, `JetNozzle`, `RotorShaft`) because there is no designer-facing
taxonomy at all, only classes.

**One authored event per vehicle type.** The engine and rotor fragments are
individually named:

```
AbramsM1A2_Engine   CV-9035_Engine   LAV25_Engine   PANCIR-S1_Engine   URAL_Engine
T-14_Armata_Engine  LEOPARD_Engine   XA-180_Engine  TGB-1111_Engine    KOZLIK_Engine
IL76_Turbine_New    SU25_Yak_Turbine_New   SU57_Turbine_New   Harrier_A6_Turbine_New
C17_Turbine         B52_Strato_Turbine     TU-95_Turbine      BIG_Turbine
MH-60_Rotor   RAH66_Rotor   CH53_Rotor   AH64Longbow_Rotor   KIOWA_Rotor
V-22_Osprey_Rotor   SilentHawk_Rotor   UH-1_Venom_Rotor   Chenook_Rotor
GENERIC_HELI_DRONE_Rotor
```

**[inferred] That is the FMOD working model in one list.** Each of those is a
Studio event with its own internal layers, parameter automation, distance curves
and randomisation, built by a sound designer. The game does not layer an engine
from RPM and load the way [`nuclear_option_audio.md`](nuclear_option_audio.md) §6
does — it posts `event:/BrokenArrowGame/VEHICLE/AbramsM1A2_Engine` and sets a
parameter. **The layering still happens; it happens inside the bank.** And
`GENERIC_HELI_DRONE_Rotor` is the fallback for anything without bespoke treatment,
which tells you the per-unit events are a deliberate spend rather than a
requirement.

The `_New` suffix on eight turbine events is an audible redo pass preserved in the
naming.

Weapon fragments show the standard modern structure:

```
MINIGUN_Tail        RIFLE_Tail_SmallCaliber      MARKSMAN_SubSonic
GunVehicle          MG_GL_Rifle                  Shell_Explosion_Impact/
```

**[inferred] `_Tail` events are separate from the shot events** — the distant
reflection/decay layer authored and triggered independently of the muzzle report,
which is how a rifle sounds different across a valley than across a room. That is a
technique Nuclear Option does not have and structurally cannot easily add, because
its `Gun` plays a clip rather than posting an event.

**And here is what is *not* in the vocabulary:** no fragment anywhere contains
`doppler`, `distance`, `attenuation`, `falloff`, `near`, `far`, or `LOD`.
(`SubSonic` refers to ammunition, not to flight regime.) **[inferred] Those
concepts are absent from the naming because they are not events — they are curves
on the 3D panner inside each event.** The distance behaviour of every sound in this
game is authored in a GUI and is invisible from outside the bank. That is the
central methodological finding of this note.

---

## 5. What the code actually is

The FMOD binding proves nothing (§0), but the **game's own type names** in the
IL2CPP metadata do, because those are code Steel Balalaika wrote.

### 5.1 They did not use FMOD's own emitter component

**[BUILD]** The game defines its own emitter abstraction:

```
IFmodEmitter                    ← interface
FmodAmbienceEmitter   FmodEngineEmitter   FmodInfantryEmitter   FmodBridgeEmitter
EmitterService
RegisterActiveEmitter   DeregisterActiveEmitter   UpdateActiveEmitter
TryGetEmitter   GetEmptyEmitter   activeEmitters   emitterName   _emitterID
_forestEmitter   _turretEmitter   _waterEmitter   _windEmitter
```

`FMODUnity.StudioEventEmitter` — the integration's own per-GameObject component —
is also present **[BUILD]**, but the existence of `IFmodEmitter` with four
implementations, an `EmitterService`, and an explicit
register/update/deregister/**`GetEmptyEmitter`** lifecycle says it is not what
drives gameplay audio.

**[inferred] This is the single most important thing a team learns when they put
FMOD into an RTS.** `StudioEventEmitter` is a MonoBehaviour you attach to a
GameObject; it works beautifully for a door, a campfire or a player character, and
it falls apart at a thousand units, because you get a thousand components each
holding an `EventInstance` and each doing a `set3DAttributes` every frame. The fix
is always the same shape: **a pool of emitters, a service that owns them, and an
active set that gets reassigned to whatever currently matters.** `GetEmptyEmitter`
is the pool acquire; `activeEmitters` is the set; `UpdateActiveEmitter` is the
per-frame reassignment.

The four named singletons — `_forestEmitter`, `_turretEmitter`, `_waterEmitter`,
`_windEmitter` — are **[inferred]** the other half of the same idea: some sounds are
not per-entity at all but one emitter that gets moved to wherever the relevant
thing currently is nearest. One wind emitter, one water emitter, repositioned each
frame, instead of thousands of ambient sources.

### 5.2 Audio is an ECS subsystem

**[BUILD]**

```
AudioSystem            AudioUnitComponent
AudioCommandSystem     AudioCommandComponent      AudioCommand   AudioCommandType
AudioCqcSystem         AudioCqcComponent          AddAudioCqc
AudioInfantrySystem    AudioInfantryComponent
AudioMoveSystem
AudioJob   AudioUpdateJob   AudioFixedUpdate   AudioEvent   AudioBound
AmmunitionSoundsPreset   ChangeSoundSet   ExtraSounds
AddDeathSoundEffects   BuildingDestructionSound   AlertSoundPlay
```

`*System` + `*Component` pairs are the DefaultEcs idiom that
[`broken_arrow.md`](broken_arrow.md) §4.1 established as the live ECS (against the
compiled-but-unused Leopotam). **[inferred] So audio is not a listener bolted onto
gameplay — it is a set of ECS systems iterating entities.** `AudioMoveSystem`
presumably updates 3D attributes for everything that moved; `AudioCqcSystem`
(close-quarters combat) and `AudioInfantrySystem` are specialised passes;
`AudioCommandSystem` handles the command/voice layer. `AudioJob` and
`AudioUpdateJob` mean at least part of it is jobified, and `AudioFixedUpdate`
implies a fixed-rate audio tick separate from the render frame.

**[inferred] `AmmunitionSoundsPreset` and `ChangeSoundSet` are the data seam** —
sound selection is a preset referenced by ammunition type and swappable at runtime,
rather than a field on each weapon. That is the same instinct as
[`broken_arrow.md`](broken_arrow.md) §2's spreadsheet-authored unit database:
**push the mapping into data a non-programmer can edit.**

### 5.3 The music engine is a service with intensity levels

**[BUILD]**

```
IMusicService   MusicService   MusicServiceConfig   MusicServiceGeneratedInjector
IMusicInstance  MusicInstance
MusicIntensityData   MusicAutoIntensityData   MusicIntensityMode   MusicChangeType
GameMusicData   GameMusicDataGroup   GetRandomMusicData
GetMusicEngines   GetMusicEngineEventByName   GetMusicEngineNameByEvent
MusicBaseEvents   BattleMusicReturnTime   MusicSelector
NodeSetMusicIntensity                       ← a mission-editor node
```

**[TRACE]** `Log: [service] reg service IMusicService.` appears **once in every one
of the 49 shipped session logs**. The music service demonstrably runs.

**[inferred] Reading the names: a music *engine* is a selectable score** (matching
the `Music_Multi1E/2D/3A/4G` and `Music_Engine_Zombies!` banks),
`GetMusicEngines` enumerates them, and within an engine the system moves between
**intensity levels** — `MusicIntensityData`, with `MusicAutoIntensityData` for
automatic intensity driven by game state and `MusicIntensityMode` to choose between
automatic and scripted. `BattleMusicReturnTime` is the hysteresis: how long after
combat ends before the score drops back down, which is the one parameter every
adaptive music system needs and most get wrong. `MusicChangeType` and the
`Stingers+Transitions/` event group are the transition material.

`MusicServiceGeneratedInjector` is VContainer (§2 of the parent note) — the music
service is dependency-injected like everything else.

### 5.4 Programmer sounds, and why

**[BUILD]** `CREATE_PROGRAMMER_SOUND`, `DESTROY_PROGRAMMER_SOUND`,
`PROGRAMMER_SOUND_PROPERTIES`.

FMOD programmer sounds let the game substitute an arbitrary audio file into a slot
inside an authored event at runtime — the event keeps its processing, spatialisation
and mix routing, but the audio is chosen by code.

**And the shipped manual says exactly what it is for.** From
`Manual/Data/MissionEditor/2. Node groups/Audio.md`, the *Play audio* node:

> *Audio file.* This field allows you to use own external audio file. Size
> limitation is 15mb. Formats: `.mp3`, `.ogg`, `.wav`.

**[inferred] So a mission author can drop their own `.mp3` into a scenario and it
plays through the FMOD event system**, inheriting the distance attenuation the node
configures (the same node exposes *Min-Max Distance*) and the mix routing of
whatever bus the event sits on. `CustomDialog.bank` at 0.0 MB is the empty bank
that slot lives in. That is a genuinely good use of the middleware, and it is a
capability that would be substantial work to build from scratch.

The same manual documents *Set music intensity* and *Play music* nodes, matching
`NodeSetMusicIntensity` — **the adaptive music system is exposed to mission
authors**, not just to the campaign.

### 5.5 Resonance: present, capable, unproven

**[BUILD]** `FMODUnityResonance`, `FmodResonanceAudio`, `FmodResonanceAudioRoom`,
`ResonanceAudio`, `GetAmbisonicDecoderFloat` / `SetAmbisonicDecoderFloat`, plus the
native DSP descriptions in §1.

`FmodResonanceAudioRoom` is the interesting one — Resonance's **room model**, which
generates early reflections and reverb from a room's dimensions and per-surface
materials, and which is the occlusion-and-reverb capability
[`nuclear_option_audio.md`](nuclear_option_audio.md) §2 identifies as the ceiling
of the no-middleware approach.

**[inferred] But I cannot show it is used, and I am sceptical that it is used
much.** These are the FMOD-Unity Resonance integration's own classes, compiled in
because the package is installed. A room model is for interiors, and this game is
an outdoor RTS viewed from above. `LowPassResonance` appears separately and is
probably a filter parameter name. **[BUILD] not [TRACE]** — and this is precisely
the trap §4.1 of the parent note exists to warn about.

---

## 6. A shipped defect, in the logs

**[TRACE]** Across the 49 shipped session logs in `GameLogs/`:

```
Warning: [FMOD] Please add an 'FMOD Studio Listener' component to your a camera
in the scene for correct 3D positioning of sounds.
FMODUnity.RuntimeManager:Update()
```

It appears in **38 of the 49 sessions**, spanning **June 2025 to April 2026** — ten
months of builds. (Unity deduplicates identical console messages, so "once per log"
means *at least* once, not exactly once.)

**[inferred] What it means:** at some point in most sessions, `RuntimeManager`
runs an `Update` with no `StudioListener` present, and FMOD falls back to a listener
at the origin. Every 3D sound positioned during that window is spatialised wrongly.
The metadata contains `FindAudioListener` and
`FindAudioListenerInCameraIfNull` **[BUILD]**, which suggests the team has their own
listener-attachment logic — and that it has a gap, most plausibly between scene
load and camera setup, or in the front-end scene where it would be inaudible.

I want to be careful about how damning this is. It is a *warning*, it is very
likely confined to a window where nothing 3D is playing, and if it were audible
during gameplay someone would have noticed in ten months. But it is the same
signature as [`broken_arrow.md`](broken_arrow.md) §5.1's 16,056 swallowed
`IndexOutOfRangeException`s: **a diagnostic that fires in almost every session,
gets written to a log nobody aggregates, and survives ten months of releases.**

**[inferred] And it is a specifically *middleware* failure mode.** Nuclear Option
cannot have this bug, because there is no listener component to forget — Unity's
`AudioListener` is on the camera by construction and the game's own audio code
reads the camera transform directly. Buying a package means adopting its setup
contract, and setup contracts are exactly what gets broken by a scene-loading
refactor and never noticed.

---

## 7. Read against Nuclear Option

The two games, same engine, same era, same broad genre space, opposite decisions.

| | Broken Arrow | Nuclear Option |
|---|---|---|
| Middleware | **FMOD Studio + Resonance** | **none** |
| Audio on disk | **3,038 MB in 30 banks** | a fraction of a 2.3 GB install |
| Music share of audio | **51.9%** | one clip per aircraft + a menu track |
| SFX share of audio | 8.7% | effectively all of it |
| Engine sound | **one authored event per vehicle**, layered in Studio | **two state variables** (RPM → pitch, power → volume) in code |
| Weapon structure | shot + **separate `_Tail` event** | start / loop / end clips, code-sequenced |
| Distance behaviour | 3D panner curves **inside each event** | `22000/travelTime` low-pass, in code |
| Doppler | FMOD's, per-event, authored | `0.6` global, toggled by camera state, damped along the thrust axis |
| Travel delay | *not present* | expanding wavefront at 340 m/s |
| Supersonic | *not present* | Mach cone, unit muted outside it |
| Emitters | **in-house `IFmodEmitter` + pooled `EmitterService`** | lazily-created `AudioSource`s per part |
| Update model | **ECS systems** (`AudioMoveSystem`, `AudioCqcSystem`…) + jobs | MonoBehaviour `Update`, gated by `displayDetail` |
| Music | **intensity-driven service, multiple engines, editor-exposed** | `CrossFadeMusic` on takeoff |
| Reverb / occlusion | Resonance available **[BUILD]** | none |
| User-supplied audio | **yes**, 15 MB, mp3/ogg/wav, via programmer sounds | no |
| Shipped audio defect | listener missing in 38/49 sessions **[TRACE]** | none found (but I read code, not logs) |

**[inferred] Four readings.**

**The middleware is not buying them the hard part; it is buying them the big
part.** Nothing in Broken Arrow's audio addresses Doppler artefacts, propagation
delay or shock cones, because an RTS camera does not create those problems. What
FMOD buys is the ability to ship 3 GB of authored content — thirty banks, per-unit
events, an adaptive score, four voice sets — **without a programmer in the loop for
any of it.** That is a content-scale purchase, and it is the right one for a game
whose audio budget is 76% music and voice.

**The two teams wrote roughly the same amount of audio *code*.** Nuclear Option's
250 lines solve three physics problems. Broken Arrow's `EmitterService`, five ECS
audio systems, `MusicService` and preset plumbing are not obviously less code — they
just solve a completely different problem (how do I drive thousands of authored
events efficiently) and none of the behaviour lives there. **Buying middleware did
not remove the audio programming; it relocated it from *what a sound is* to *how
sounds are dispatched*.**

**The invisibility cuts both ways.** I can read every constant in Nuclear Option's
audio and tell you exactly why a distant explosion is muffled. I cannot tell you
anything about how Broken Arrow's tank engine responds to load, because that is a
parameter curve in a `.bank` I would need FMOD Studio and the source project to
open. For the *team*, that is a feature — the sound designer iterates without a
build. For anyone outside it, including a future maintainer, the behaviour is
opaque.

**And the failure modes are different in kind.** Nuclear Option's audio risk is
that a wrong constant silently removes a phenomenon
([`nuclear_option.md`](nuclear_option.md) §7.7 is that bug in the physics). Broken
Arrow's is §6: the integration's setup contract gets broken by something unrelated
and nobody notices for ten months. **Hand-rolled fails by being wrong; bought fails
by being disconnected.**

---

## 8. What transfers

1. **Do not use the middleware's per-object emitter component at scale.** (§5.1.)
   `IFmodEmitter` + `EmitterService` + `GetEmptyEmitter`/`activeEmitters` is the
   pooled-emitter pattern every team arrives at, and arriving at it early is worth a
   lot. The same applies to Wwise's `AkGameObj`.

2. **Some ambient sounds should be one emitter that moves, not many that exist.**
   (§5.1.) `_windEmitter`, `_waterEmitter`, `_forestEmitter`, `_turretEmitter` —
   singletons repositioned to whatever is currently nearest and most relevant.

3. **Let the audio taxonomy match the designer's mental model, not the code's.**
   (§4.) `INFANTRY/`, `VEHICLE/`, `PLANE/`, `HELO/` — because the person authoring it
   thinks in units. Naming events after your class hierarchy makes the bank
   unnavigable for the person who has to fill it.

4. **Split bank granularity along your DLC and mode boundaries from the start.**
   (§3.) Five `_BB_DLC` banks means DLC audio is purely additive. Retrofitting that
   is painful.

5. **Author the distant layer as a separate event.** (§4.) `MINIGUN_Tail`,
   `RIFLE_Tail_SmallCaliber`. A gunshot near you and the same gunshot across a
   valley are two different sounds, not one sound with a filter — and a code-driven
   audio system (like Nuclear Option's) makes that structurally hard to add later.

6. **Adaptive music needs a return-time, and it should be authored.** (§5.3.)
   `BattleMusicReturnTime` is the parameter every adaptive score gets wrong, and
   exposing intensity to the mission editor (`NodeSetMusicIntensity`) means the
   people writing the missions can fix the pacing themselves.

7. **Programmer sounds are how you let users bring audio.** (§5.4.) An empty
   `CustomDialog.bank` plus a 15 MB file limit gives mission authors their own audio
   *inside your mix and spatialisation*, which is far better than playing it raw.

8. **Route sound selection through a data preset.** (§5.2.)
   `AmmunitionSoundsPreset` + `ChangeSoundSet` — the mapping from game concept to
   audio event is data, editable without a programmer, swappable at runtime.

9. **Know which problem you are buying a solution to.** (§7.) FMOD would not have
   helped Nuclear Option with Doppler artefacts or shock cones. Hand-rolled audio
   would not have let Broken Arrow ship 3 GB of authored content and an adaptive
   score. **The question is not "middleware or not", it is "is my audio problem
   content-scale or physics-shape".**

10. **Aggregate your warnings.** (§6.) A middleware setup warning in 38 of 49
    sessions across ten months is the same lesson as the parent note's swallowed
    exception storm: *a diagnostic logged but not counted is a diagnostic not
    detected.*

---

## 9. What is not established

- **I have not heard either game**, and I have not opened a single `.bank`. Every
  statement about how something *sounds* is absent by design.
- **The FMOD Studio source project is not shipped**, so all authored behaviour —
  distance curves, Doppler settings, parameter automation, layer crossfades, mix
  snapshots, bus routing, compression — is unreadable. That is not a gap in my
  method; it is the structural consequence of the approach, and it is §7's third
  reading.
- **Event paths were not fully reconstructed.** The strings bank is
  prefix-compressed and I recovered fragments, not paths. A proper FSB/FEV parser
  would recover the complete event list and the bus and VCA structure; I did not
  write one.
- **`FmodResonanceAudioRoom` is [BUILD] only.** I cannot show Resonance is used at
  all, let alone the room model. §5.5 says so.
- **The 178 FMOD P/Invoke names prove nothing** about which API calls the game
  makes. Only the game's own type names (§5) are evidence, and even those are
  compiled-in rather than observed — except `IMusicService`, which is **[TRACE]**.
- **Bus, VCA and snapshot structure is unknown**, so how the mix ducks (does the
  score drop under voice? does a nearby explosion duck the ambience?) is not
  established.
- **`AudioCqcSystem`, `AudioInfantrySystem` and `AudioMoveSystem` are names**, not
  read implementations. IL2CPP means there is no body to read without a
  reverse-engineering pass on `GameAssembly.dll` that I did not attempt.
- **No developer account of any of this exists**, for either game.

---

## 10. Reproducing this

```
# banks and sizes
BA/BrokenArrow_Data/StreamingAssets/*.bank

# the event vocabulary (prefix-compressed; split on NUL, keep printable runs)
Master.strings.bank        # RIFF / FEV  / FMT  container, 164 KB

# game-side type names (NOT proof of use — see §0)
BA/BrokenArrow_Data/il2cpp_data/Metadata/global-metadata.dat    # 27.9 MB
  regex [A-Za-z_][A-Za-z0-9_]{3,60}, filter out FMOD/UnityEngine/System/Epic prefixes

# runtime evidence
BA/GameLogs/*.log          # 49 sessions; grep FMOD, MusicService

# native plugins
BA/BrokenArrow_Data/Plugins/x86_64/{fmodstudio,resonanceaudio,xaudio2_9redist}.dll

# first-party documentation, shipped
BA/Manual/Data/MissionEditor/2. Node groups/Audio.md
```

---

## Sources

- **The retail install**, `C:\Program Files (x86)\Steam\steamapps\common\broken_arrow`
  — banks, `Master.strings.bank`, `global-metadata.dat`, `Plugins/x86_64/`,
  `GameLogs/`, `boot.config`, and the shipped `Manual/`.
- [`broken_arrow.md`](broken_arrow.md) — the parent note, same install, and the
  **[BUILD]** / **[TRACE]** method this one inherits.
- [`nuclear_option_audio.md`](nuclear_option_audio.md) — the other half of the
  comparison.
- **No engineering talk, blog or paper was found for either game.**
