# Nuclear Option — audio for things that move faster than sound

The fifth note on Nuclear Option, and the narrowest. It covers one question:
**how do you make a vehicle sound right when it is moving at 600 m/s, and how do
you make a battlefield sound right when the things making noise are ten kilometres
away?**

That is three separate hard problems, and most games solve none of them:

1. **Doppler at high Mach breaks.** Unity's built-in Doppler is a linear
   frequency shift derived from relative velocity along the listener axis. At
   400 m/s it produces pitch shifts that sound like a broken tape deck, and the
   worst case is the exact geometry a flight game spends most of its time in — a
   chase camera directly behind a departing aircraft.
2. **Sound takes time to arrive.** A bomb five kilometres away is a flash and
   then, fifteen seconds later, a rumble. Playing it instantly is the single
   biggest tell that a war game's audio is fake.
3. **A supersonic aircraft is silent until it isn't.** You do not hear it
   approach. It passes, and *then* the shock front arrives.

Nuclear Option solves all three, **with no audio middleware whatsoever** and about
250 lines of game code. §2 is the negative finding that makes that worth reading.

Same source and caveats: read from the decompiled `Assembly-CSharp.dll` of the
retail Mono build. **No developer talk, blog or paper exists.** Tags: **[CODE]**
read from the assembly, **[inferred]** my reading. Nothing here comes from
listening to the game — I have read the code that schedules and parameterises the
sounds, not heard the result.

Related: [`nuclear_option.md`](nuclear_option.md) (the engines and rotors whose
state drives most of this), [`nuclear_option_combat.md`](nuclear_option_combat.md),
[`nuclear_option_command.md`](nuclear_option_command.md),
[`nuclear_option_control.md`](nuclear_option_control.md),
[`broken_arrow_audio.md`](broken_arrow_audio.md) (**the A/B** — the same problem
space with FMOD and Resonance bought in), [`broken_arrow.md`](broken_arrow.md).

---

## 1. The shape of it

**[CODE]** Everything hangs off `SoundManager`, a `MonoBehaviour` loaded from
Resources that owns four one-shot sources and **eight mixer groups**:

| Mixer group | What routes to it |
|---|---|
| `EffectsMixer` | most world sound — servos, hits, scrapes, guns, gear |
| `HeavyEffectsMixer` | explosions, sonic booms, rotor strikes |
| `InterfaceMixer` | UI |
| `RadarWarningMixer` | RWR tones |
| `MissileAlertMixer` | launch warning |
| `JammedNoiseMixer` | jamming static |
| `MenuMixer`, `MusicMixer` | front end, score |

**[inferred] The split that matters is `Effects` versus `HeavyEffects`.** They are
separate groups so that explosions and booms can be compressed, ducked and
filtered independently of the continuous machinery noise — which is the difference
between a nearby detonation *punching through* the engine note and simply being
added to it. Splitting the warning tones into three further groups means the RWR
can be kept audible under everything else by mixer routing rather than by volume
hacks in the gameplay code, and each gets its own player-facing volume slider.

Volume control is `PlayerPrefs` → `AudioHelper.LinearToDecibel(v) = 20·log₁₀(v)`
→ `AudioMixer.SetFloat`. **[inferred]** Correct, and worth noting only because
setting a mixer's dB parameter from a linear 0–1 slider without the log conversion
is one of the most common audio bugs in Unity projects: the top 20% of the slider
does almost nothing and the bottom 20% is a cliff.

---

## 2. There is no audio middleware

**[CODE]** I searched the whole assembly and the shipped manifest: **no FMOD, no
Wwise, no Steam Audio, no Resonance, no spatializer plugin, no reverb zones, and
no occlusion system.** Every sound in the game is a stock Unity `AudioSource`
routed through a stock `AudioMixer`, with `AudioLowPassFilter` used in exactly one
place (§4).

**[inferred] Two things follow, and they point in opposite directions.**

The first is the thesis of [`nuclear_option.md`](nuclear_option.md) §16 appearing
again. [`broken_arrow.md`](broken_arrow.md) ships FMOD Studio *and* Resonance —
two audio packages — for a game whose audio problem is genuinely easier (an RTS
camera is stationary and above everything). Nuclear Option, whose audio problem is
much harder, bought nothing. And the three hard problems above are solved in
`ExplosionAudioManager` (97 lines), `SonicBoomManager` (122 lines) and about
thirty lines spread across `Unit`, `JetNozzle` and the camera states. **The
interesting problems in this space were not the ones middleware solves.**

The second is what is *missing* because of it. There is no occlusion — a hill
between you and a firefight does not muffle it. There is no reverb — a valley and
an open plain sound identical. There is no HRTF; spatialisation is Unity's
default panning with hand-set `spread` and `rolloffMode` per source. For a game
where the player is usually a kilometre up in a noisy cockpit, those are cheap
things to not have, and the *arrival-time filtering* in §4 covers the single most
audible case (distance) that reverb and occlusion would otherwise be doing. But it
is a real ceiling: this audio cannot ever tell you that you are flying down a
canyon.

---

## 3. Problem one: Doppler that does not tear itself apart

### 3.1 Sounds opt in, and they are toggled as a set

**[CODE]** `Unit` keeps a list:

```csharp
private List<AudioSource> dopplerSounds;
public void RegisterDopplerSound(AudioSource s)   => dopplerSounds.Add(s);
public void DeregisterDopplerSound(AudioSource s) => dopplerSounds.Remove(s);

public void SetDoppler(bool enabled) {
    foreach (AudioSource s in dopplerSounds) {
        s.dopplerLevel  = enabled ? 0.6f : 0f;
        s.spatialBlend  = enabled ? 1    : 0;
    }
}
public void SetSoundsMuted(bool muted) {
    foreach (AudioSource s in dopplerSounds) s.mute = muted;
}
```

Registrants are the things whose pitch should shift with motion: engines, guns
(`Gun` registers on first fire and deregisters when the source is released),
control-surface airflow (`ControlSurface.splitSource`), ground vehicle engine and
idle loops. Everything else — cockpit warnings, servo clicks, canopy, ejection,
chaff and flare dispensers, the AoA horn, the jammed-radar noise — is created with
`dopplerLevel = 0f` explicitly at construction and never joins the list.

**[inferred] The opt-in list is the whole design.** It gives one place to flip the
entire acoustic character of a vehicle, and §3.2 is why you want that.

### 3.2 The camera decides whether Doppler exists

**[CODE]** `SetDoppler` is called from exactly one kind of place — camera state
transitions:

```csharp
// CameraChaseState / CameraOrbitState / CameraCockpitState, on entering:
cam.followingUnit.SetDoppler(enabled: false);
// ... and on leaving:
cam.followingUnit.SetDoppler(enabled: true);
```

**So the aircraft you are flying, or chasing, or orbiting, has no Doppler and its
sounds are `spatialBlend = 0` — fully 2D, non-positional, centred.** Every other
aircraft in the world keeps `dopplerLevel = 0.6` and full 3D positioning.

**[inferred] This is the correct answer and it is two lines.** The problem it
solves: in a chase camera, the camera is moving at nearly the aircraft's velocity,
so relative velocity is *nearly* zero — but "nearly" is the issue. Any camera lag,
any spring on the follow, any manoeuvre that swings the camera, produces a
fluctuating relative velocity of tens of metres per second along the listener
axis, and Unity converts that directly into a wobbling pitch. The engine note
*warbles* whenever you manoeuvre. Turning Doppler off for the followed unit
removes the artifact entirely; making the sound 2D at the same time removes the
matching problem of the engine panning left and right as the camera swings.

And it costs nothing in realism, because **from inside or behind your own
aircraft, there is genuinely no Doppler shift** — you are travelling with the
source. The physically correct answer and the artifact fix are the same answer.

`0.6f` rather than `1.0f` for everything else is the second half. **[inferred]** A
full-strength Doppler on a target closing at Mach 1.6 is a pitch multiplier of
around 2.5×, which sounds like a cartoon. Scaling to 0.6 keeps the *direction* and
the *timing* of the effect — the characteristic fall as a jet goes past — while
staying inside the range where the pitched audio still sounds like the thing it is
a recording of.

### 3.3 Doppler is damped further along the thrust axis

**[CODE]** `JetNozzle.AudioEffects`, called every physics step:

```csharp
Vector3 camToNozzle = FastMath.NormalizedDirection(cameraPos, nozzlePos);
camFacing = Vector3.Dot(camToNozzle, thrustTransform.forward);
directionalVolumeMult = Mathf.Lerp(0.5f, 2f, camFacing);
thrustAudio.volume = thrustRatio * thrustMaxVolume * directionalVolumeMult;
if (thrustAudio.dopplerLevel > 0f)
    thrustAudio.dopplerLevel = Mathf.Max(1f - camFacing * 2f, 0.01f);
```

and the same two values are passed down into each `Afterburner.Audio`.

`camFacing` is +1 when the camera is directly behind the aircraft looking up the
exhaust, and −1 when directly in front. **[inferred] Two independent effects fall
out of one dot product.**

**The exhaust is four times louder from behind.** `Lerp(0.5f, 2f, camFacing)`, and
because Unity's `Lerp` clamps its interpolant, the whole forward hemisphere gets a
flat 0.5 and the rear hemisphere ramps 0.5 → 2.0. A jet approaching head-on is
quiet and then extremely loud as it passes — which is exactly right, because jet
exhaust noise really is strongly directional, and it is the single cheapest way to
make a flypast feel like a flypast.

**Doppler is scaled to near zero exactly where it would be worst.**
`max(1 − 2·camFacing, 0.01)` reaches 0.01 at `camFacing = 0.5`, i.e. anywhere
within 60° of directly astern. That is the geometry where the source's velocity is
most nearly along the listener axis, which is where Unity's Doppler shift is
largest — and it is also, not coincidentally, the geometry of every tail chase and
every missile view. **[inferred] The fix is applied where the artifact lives and
nowhere else**, so a beam-aspect flypast keeps its full Doppler and a stern chase
does not tear.

---

## 4. Problem two: sound arrives late, and arrives dull

**[CODE]** `ExplosionAudioManager` is 97 lines and is the best thing in the audio
code.

```csharp
public ManagedExplosion(AudioSource source, AudioLowPassFilter filter, float yield) {
    startTime   = Time.timeSinceLevelLoad;
    propagation = Mathf.Pow(yield, 0.3333f) * 0.5f;      // start at the fireball radius
}

public bool InRange(Vector3 listenerPosition) {
    propagation += 340f * Time.deltaTime;                 // the wavefront expands
    if (FastMath.InRange(listenerPosition, xform.position, propagation)) { Play(); return true; }
    return false;
}

public void Play() {
    float travelTime = Time.timeSinceLevelLoad - startTime;
    audioSource.bypassListenerEffects = travelTime > 0.1f;
    filter.cutoffFrequency = Mathf.Clamp(22000f / travelTime, 1000f, 22000f);
    audioSource.Play();
    if (yield > 0f) {
        float d2 = Vector3.SqrMagnitude(xform.position - cameraPos);
        CameraStateManager.i.ShakeCamera(Mathf.Clamp01(yield * 100f / d2), 0f);
    }
}
```

Every explosion registers a **pending** sound rather than playing one. The manager
grows each pending explosion's radius at 340 m/s per frame and plays it the moment
the sphere reaches the listener. **[inferred] Four details make this more than the
obvious version.**

**The wavefront starts at the fireball radius**, `yield^(1/3) · 0.5` — the same
cube-root blast scaling used for exclusion zones
([`nuclear_option_command.md`](nuclear_option_command.md) §2). A large explosion is
already loud at the edge of its own fireball, so it should not have to propagate
from a point. It also means a nuke going off nearby is heard essentially at once.

**The low-pass cutoff is `22000 / travelTime`, clamped to 1–22 kHz.** So a sound
that took 0.1 s to arrive is unfiltered; one that took 5 s is cut at 4.4 kHz; one
that took 22 s or more is cut at the 1 kHz floor. **That is atmospheric absorption
— high frequencies attenuate faster over distance — modelled with a single
divide.** A distant bomb is a muffled thud and a close one is a sharp crack, and
the transition is continuous. Getting this from one expression, on a filter that is
created per explosion and configured once at the moment of arrival, is about as
cheap as the effect can possibly be.

**`bypassListenerEffects` flips on past 0.1 s of travel**, taking distant
explosions out of whatever the listener's effects chain is doing — so a far-off
rumble is not also being chorused or filtered by the cockpit processing.

**The camera shake is triggered at arrival, not at detonation**, and scales as
`yield · 100 / distance²`. **[inferred] So the shake and the sound are
synchronised by construction**, because they are the same event. A game that
shakes the camera on detonation and delays the audio has just built a very
noticeable bug; here the delay mechanism owns both.

The registration side (`ExplosionAudio`, 40 lines) is a **distance cull at spawn**:

```csharp
float d2 = SquareDistance(cameraPos, transform.position);
foreach (ExplosionSound s in explosionSounds)
    if (d2 < s.source.maxDistance * s.source.maxDistance) {
        s.source.clip = s.clips[Random.Range(0, s.clips.Length)];
        s.source.pitch += Random.Range(-0.2f, 0.2f);
        s.source.dopplerLevel = 0f;  s.source.spatialBlend = 1f;
        ExplosionAudioManager.i.AddExplosionAudio(s.source, lowPassFilter, s.yield);
    }
Destroy(this);
```

**[inferred] An explosion carries *several* layered sounds with different yields
and different `maxDistance`s**, and only the layers whose range reaches the camera
are registered. So a big detonation heard from 8 km contributes only its long-range
low rumble layer, while the same detonation at 200 m contributes the crack, the
debris and the rumble together. Distance-based layer selection, done at spawn, with
no per-frame cost — and the component deletes itself immediately afterwards.

`Shockwave` uses the identical `blastPropagation += 340f * Time.deltaTime`
expression for the *physical* blast wave, and `MushroomCloud` for its visual
expansion. **[inferred] One constant, three systems, and they stay in step because
they all just use 340.**

---

## 5. Problem three: the Mach cone

**[CODE]** `SonicBoomManager` is a static class with a list of supersonic units.
Registration is lazy and comes from the engine, on a 1 Hz slow update:

```csharp
// JetNozzle.SlowUpdate
if (aircraft.speed > LevelInfo.GetSpeedOfSound(aircraft.GlobalPosition().y))
    SonicBoomManager.RegisterUnit(aircraft);
```

and then, per fixed step, driven from `CameraStateManager.FixedUpdate`:

```csharp
float mach      = unit.speed / LevelInfo.GetSpeedOfSound(unit.GlobalPosition().y);
float machAngle = Mathf.Asin(1f / mach) * Mathf.Rad2Deg;                    // μ = asin(1/M)
float bearing   = Vector3.Angle(unitPos - cameraPos, unit.rb.velocity);

if (FastMath.InRange(unitPos, cameraPos, unit.maxRadius)) machAngle = 180f;  // you're on it

if (bearing < machAngle) {                       // ← the listener is INSIDE the Mach cone
    if (!inMachCone) {
        GlobalPosition emissionPoint = unitPos + Vector3.Project(unitPos - cameraPos, -unit.rb.velocity);
        inMachCone = true;
        unit.SetSoundsMuted(muted: false);        // ← the aircraft becomes audible
        sourceObject.transform.position = emissionPoint.ToLocalPosition();
        if (machAngle < 180f && FastMath.OutOfRange(cameraVelocity, unit.rb.velocity, 100f)) {
            source.PlayOneShot(GameAssets.i.sonicBoom, 1f);
            CameraStateManager.i.ShakeCamera(Mathf.Clamp01(1000f / Distance(emissionPoint, cameraPos)), 0f);
        }
    }
} else if (inMachCone) {
    inMachCone = false;
    unit.SetSoundsMuted(muted: true);             // ← the aircraft goes SILENT
}
```

**[inferred] This is a genuinely physical treatment and it is forty lines.**

**The aircraft is muted while you are outside its Mach cone.** That is the correct
and dramatic behaviour, and it is the thing almost no game does: a supersonic jet
approaching you makes no sound at all, because the sound it has made is behind it.
Then the cone edge sweeps over you, the boom fires once, and the engine noise
switches on — all at the same instant, because they are the same state transition.
`SetSoundsMuted` reuses the §3.1 registration list, so "the aircraft's own noises"
is already a defined set.

**The Mach angle is the real one.** `μ = asin(1/M)` — 90° at Mach 1 (the cone is a
plane), narrowing to 30° at Mach 2. So a faster aircraft is silent for longer and
its boom arrives later relative to the flypast, without anyone tuning that.

**The boom source is placed at the emission point, not at the aircraft.**
`unitPos + Project(unitPos − cameraPos, −velocity)` walks back along the flight
path to the point where the shock now reaching you was generated. So the boom
arrives from *behind* the aircraft's current position, which is where it should
come from, and the spatialisation is right rather than merely loud.

Two guards. If the camera is inside the unit's own radius, the cone opens to 180°
— you are riding it, so you hear it. And **the boom is suppressed if the camera is
travelling with the aircraft** (`OutOfRange(cameraVelocity, unit.velocity, 100f)`
must hold), so you do not sonic-boom yourself in a chase view every time you cross
Mach 1. **[inferred] That second guard is the kind of thing you only add after
shipping it once without it.**

The boom `AudioSource` is configured for the job: `minDistance = 1000`,
`maxDistance = 5000`, `spread = 20`, `dopplerLevel = 0`, routed to
`HeavyEffectsMixer`. A 1 km minimum distance means it does not attenuate at all
until you are a kilometre from the emission point — correct for a wavefront, and
the opposite of what a default `AudioSource` would do.

Separately, `Aircraft.FixedUpdate` shakes the airframe between Mach 0.99 and 1.00
with an intensity of `0.25 · airDensity`, so the transonic buffet is felt (and
fades at altitude) just before the regime the boom belongs to.

---

## 6. Machinery: two parameters, not one

**[CODE]** Every engine in the game drives its sound from **two separate state
variables**, not from throttle.

**Turbine (turboshaft):**

```csharp
turbineAudio.pitch  = RPMRatio - powerRatio * pitch + pitch;
turbineAudio.volume = running ? (RPMRatio * 0.25f + powerRatio * 0.75f) * volume
                              : (RPMRatio * 0.5f * volume);
```

where `RPMRatio = currentRPM / maxRPM` and `powerRatio = (currentPower/maxPower)²`.

**[inferred] Pitch tracks RPM and volume mostly tracks power, and they are not the
same thing.** A turbine spooling up at idle rises in pitch with little volume; one
under load at constant RPM gets louder without changing pitch. That two-variable
split is what makes a jet sound like a machine responding to a demand rather than
a sample scrubbed by a slider — and it is free, because both variables already
exist in the engine model
([`nuclear_option.md`](nuclear_option.md) §8.3). The `− powerRatio · pitch` term
even *lowers* pitch slightly under load, which is the audible signature of a
turbine bogging down.

**Turbofan** is `pitch = currentRPM / maxRPM * turbineMaxPitch`, with the thrust
layer handled separately in `JetNozzle` (§3.3) and the afterburner as a third layer
in `Afterburner.Audio`, ramped by `afterburnerAmount` over a very narrow throttle
band (`throttleStart = 99.8`). So a jet is **three sound layers** — turbine spool,
thrust, afterburner — each driven by a different state variable.

**Rotor:**

```csharp
rotorSource.pitch  = detached ? 0f : angularSpeedRatio * pitch;
rotorSource.volume = detached ? 0f : angularSpeedRatio * volume * condition;
```

driven by the shaft's actual angular speed and by `condition` — **a damaged rotor
is quieter, and a detached one is silent instantly.**

**Ducted fan:**

```csharp
fanSource.pitch  = rpm / maxRPM * pitch;
fanSource.volume = rpm / maxRPM * (unthrottledVolume + |currentThrust| / maxThrust * throttledVolume);
```

— again RPM for pitch, and a base-plus-load term for volume, so a windmilling fan
still whines.

**Ship** hull and water sounds are driven by `rb.GetPointVelocity(source.position)`
per source, so different points on a hull have different water noise, and hull
groan is `0.25 + clamp01(speed · 0.05)`.

### 6.1 Interior versus exterior

**[CODE]** `IEngine.SetInteriorSounds(bool)` is implemented by `RotorShaft` and
`DuctedFan` with a **clip swap**, not a filter:

```csharp
public void SetInteriorSounds(bool useInteriorSound) {
    if (rotorSource.isPlaying) {
        rotorSource.Stop();
        rotorSource.clip = useInteriorSound ? interiorSound : exteriorSound;
        rotorSource.time = Random.Range(0f, rotorSource.clip.length);   // ← re-phase
        rotorSource.Play();
    } else rotorSource.clip = useInteriorSound ? interiorSound : exteriorSound;
}
```

called from `Aircraft` across all engines when the camera enters or leaves the
cockpit. **[inferred] A separately recorded interior take beats any amount of
filtering of an exterior one** — the cockpit of a helicopter is not the outside
with the treble removed, it is a different sound dominated by gearbox and
structure. The re-randomised `time` on the swap means the loop does not restart at
a fixed phase, so the transition does not click.

The rest of the interior character is done at the mixer, not per source: the
cockpit camera drives `AudioMixerVolume.SetEffectsAudioFilterStrength(cutoff, dry)`
— an `EffectLowPassCutoff` and an `EffectChorusDryMix` on the whole effects bus.

---

## 7. Guns: start, loop, end

**[CODE]** A cannon firing at 100 rounds per second cannot play 100 one-shots.
`Gun` has a two-source scheme:

```csharp
// ShotSound(), called per round
if (sources.Length < 2) {                                     // slow-firing weapon: per-shot one-shots
    sources[0].pitch = pitch + Random.value*pitchVariation - Random.value*pitchVariation;
    sources[0].PlayOneShot(fireSounds[Random.Range(0, fireSounds.Length)]);
    recoilSound.pitch = Random.Range(0.95f, 1.05f); recoilSound.Play();
} else if (!sources[1].isPlaying) {
    sources[0].PlayOneShot(fireStart);                        // fast weapon: the attack transient
}
if (sources.Length > 1 && !sources[1].isPlaying && Time.timeSinceLevelLoad - lastFired < fireInterval*1.1f + dt) {
    sources[1].Play();                                        // the sustain loop
    sources[1].time = Random.Range(0f, sources[1].clip.length);
}

// LoopSounds(), per frame while looping
if (modifyPitch && sources[1].pitch < pitch) sources[1].pitch += dt * pitchClimbRate;   // spin-up
if (Time.timeSinceLevelLoad - lastFired > dt + fireInterval) {
    sources[1].Stop();  sources[0].PlayOneShot(fireEnd);       // the release tail
}
```

**[inferred] A start / loop / end triple**, which is the standard and correct
structure for continuous fire, plus two things that are not standard. `modifyPitch`
ramps the loop's pitch up at `pitchClimbRate` while firing and back down when not —
**a rotary cannon spinning up and coasting down**, from one float. And
`sources[1].time = Random.Range(0, clip.length)` starts the loop at a random phase,
so two aircraft firing simultaneously do not produce a phasing artefact.

The trigger-release detection is `timeSinceLastFired > fireInterval + dt` — i.e.
one missed round plus a frame, so it does not stutter the tail on a frame hitch.

---

## 8. The cheap tricks, which are most of it

**[inferred]** Four idioms account for a large share of the perceived quality, and
all four are nearly free.

**Decorrelate anything you instance many of.** `RandomSound` and
`TurbineFireSound` are 27 and 42 lines and do the same thing: pick a random clip
from an array, jitter pitch and volume by a per-instance amount, and — for looping
clips — **start at a random `source.time`**. That last one is the important one: ten
fires burning on a wreck with the same loop starting at the same phase sound like
one loud fire, and with random phases sound like ten. The same decorrelation
instinct appears elsewhere in the codebase as jittered radar scan phases
([`nuclear_option_combat.md`](nuclear_option_combat.md) §1.1) and counter-based
rather than random tracers, which suggests it is a habit rather than a fix.

**Create sources lazily and destroy them.** `Aircraft.scrapeSource`,
`AeroPart.hitSource`, `ControlSurface.splitSource` and `RotorShaft.strikeSource`
are all `null` until the first time they are needed, then built in code with
explicit `spread`, `minDistance`, `maxDistance`, `rolloffMode`, `dopplerLevel` and
mixer group. A joint-break sound goes further and destroys itself:
`Destroy(audioSource, 3f)`. **[inferred] A game with forty parts per aircraft and
dozens of aircraft cannot afford a pre-made `AudioSource` for every event on every
part**, and the alternative — a global pool — would lose the per-part positioning
these rely on.

**Attenuate by relevance, not just distance.** `Gun.ShotSound` returns immediately
if `attachedUnit.displayDetail < 1f`; `AeroPart` only plays a hit sound if
`displayDetail > 1f`; `RotorShaft.AnimateRotor` and `DuctedFan.Animate` are gated
the same way. `displayDetail` is the visual LOD scalar, so **the audio LOD is the
graphics LOD**, and a distant dogfight costs nothing in sources.

**Fade rather than stop.** The scrape loop ramps its volume down at 2 units/second
and stops at zero; the collision source lerps to zero and stops below 0.01;
`TurbineFireSound` lerps its volume toward `clamp01(velocity · 0.1)` so a burning
part that comes to rest goes quiet. Hard-stopping a loop clicks.

### 8.1 G-LOC is an audio effect too

**[CODE]** From `GLOC.SimulateGLOC`, alongside the vignette and desaturation:

```csharp
AudioMixerVolume.SetMasterAudioFilterStrength(
    Mathf.Lerp(250f, 11000f, Mathf.Clamp01(num2)) + 11000f * Mathf.Clamp01(num3));
```

where `num2` and `num3` are blood pressure remapped over two slightly offset
ranges. **[inferred] The master bus low-pass closes to 250 Hz as the pilot greys
out** — everything muffles together, including the warning tones, which is exactly
the reported experience and is far more effective than ducking individual sounds.
It is applied at the mixer, so it costs one `SetFloat` per frame and affects
everything automatically, including sounds that did not exist when the effect was
written.

---

## 9. What is worth taking

1. **Turn Doppler off for the unit the camera is attached to, and make its sounds
   2D at the same time.** (§3.2.) Two lines. It removes the warble that a chase
   camera's own motion creates, and it is *also* the physically correct answer,
   because you do not experience Doppler from a source you are travelling with.

2. **Scale Doppler to about 0.6, not 1.0.** (§3.2.) Full-strength Doppler at
   supersonic closure sounds like a broken tape. Keep the direction and timing of
   the effect, lose the extremes.

3. **Damp Doppler further along the axis where the artifact is worst.** (§3.3.)
   `max(1 − 2·dot(camToSource, sourceAxis), 0.01)` — applied where the source's
   velocity is most nearly along the listener axis, and nowhere else.

4. **Directional exhaust from one dot product.** (§3.3.) `Lerp(0.5, 2, camFacing)`
   gives a 4:1 rear-to-front loudness ratio, and it is most of what makes a flypast
   feel like one.

5. **Schedule distant sounds on an expanding wavefront.** (§4.)
   `propagation += 340 · dt`, play when the sphere reaches the listener, start the
   radius at the fireball size. This is the single highest-impact thing in the note
   and it is a dozen lines.

6. **Low-pass by travel time.** (§4.) `cutoff = 22000 / travelTime`, clamped. One
   divide models atmospheric absorption, and it is what turns a delayed sound from
   a *late* sound into a *distant* one.

7. **Trigger the camera shake from the sound's arrival, not the event.** (§4.)
   They are the same event; letting the delay mechanism own both makes
   desynchronisation impossible.

8. **Select sound layers by distance at spawn, then delete the selector.** (§4.)
   An explosion carries several layers with different `maxDistance`s; register only
   the ones that can reach the listener. No per-frame cost.

9. **Mute a supersonic vehicle outside its Mach cone.** (§5.) `μ = asin(1/M)`, and
   the same registration list you use for Doppler is the set to mute. The boom, the
   shake and the engine noise switching on are one state transition.

10. **Place a boom at its emission point, not at the source.** (§5.) Walk back
    along the flight path by the projection of the listener offset onto the velocity
    axis.

11. **Drive machinery sound from two state variables, not from the input.** (§6.)
    Pitch from RPM, volume mostly from power. The variables already exist in any
    engine model worth having, and the split is the difference between a machine
    and a scrubbed sample.

12. **Swap clips for interior versus exterior, do not filter.** (§6.1.) And
    re-randomise the loop phase on the swap so it does not click.

13. **Start / loop / end for continuous fire, with a pitch ramp on the loop.**
    (§7.) Plus a random start phase so two of them do not phase against each other.

14. **Randomise clip, pitch, volume and loop phase for every instanced sound.**
    (§8.) `RandomSound` is 27 lines and is probably the highest quality-per-line in
    the audio code.

15. **Make the audio LOD the graphics LOD.** (§8.) One scalar (`displayDetail`)
    already exists and already means "how much does this thing matter right now".

16. **Do the pilot's physiological state at the mixer.** (§8.1.) A master low-pass
    closing to 250 Hz affects everything, including sounds written later.

17. **Convert linear slider values to decibels.** (§1.)
    `20·log₁₀(v)`. Trivial, and its absence is one of the most common Unity audio
    bugs.

And the honest caveat:

18. **This ceiling is real.** No occlusion, no reverb, no HRTF (§2). Terrain does
    not muffle anything and a canyon sounds like a plain. The arrival-time filter
    covers the most audible case, and if you need the rest, that is what the
    middleware is for.

---

## 10. What is not established

- **I have not heard the game.** Every judgement above is from reading the code
  that schedules and parameterises sounds. I cannot tell you whether the 0.6
  Doppler scaling or the 4:1 exhaust ratio actually sound right, only that they are
  what the code does and why they are plausible.
- **No authored values.** Clips, mixer graph internals, `pitch`/`volume`
  multipliers, `maxDistance`s, `spread`s and the `AudioMixer` asset's own effect
  chain all live in assets whose type trees are stripped
  ([`nuclear_option.md`](nuclear_option.md) §19). Every number quoted here is a
  hardcoded literal in the C#.
- **The `AudioMixer` graph itself was not inspected** — I know the eight groups by
  name and the three exposed parameters (`EffectLowPassCutoff`,
  `EffectChorusDryMix`, `MasterLowPassCutoff`), not what compression, EQ or ducking
  sits on each bus. That is likely where a further chunk of the character lives.
- **`MusicManager`** and the adaptive music (there is a `takeoffMusic` per aircraft
  and a `CrossFadeMusic` call on becoming airborne) were not read.
- **Cockpit-specific audio** beyond the mixer filter call — voice warnings
  (`WindowsTTS`, `Interop.SpeechLib`, `OnReportDamage.audioReport`) and the AoA
  tone were only seen in passing.
- **No developer account exists.** Every "because" is **[inferred]**.

---

## 11. Where things are

| System | Files |
|---|---|
| Core | `SoundManager.cs`, `AudioMixerVolume.cs`, `AudioHelper.cs`, `AudioMenu.cs`, `MusicManager.cs` |
| Doppler control | `Unit.cs` (`RegisterDopplerSound`, `SetDoppler`, `SetSoundsMuted`), `CameraChaseState.cs`, `CameraCockpitState.cs`, `CameraOrbitState.cs`, `CameraTVState.cs`, `CameraStateManager.cs` |
| Delayed arrival | `ExplosionAudioManager.cs`, `ExplosionAudio.cs`, `Shockwave.cs`, `MushroomCloud.cs` |
| Supersonic | `SonicBoomManager.cs`, `JetNozzle.cs` (`SlowUpdate` registration, `AudioEffects`), `Aircraft.cs` (transonic buffet) |
| Engines | `TurbineEngine.cs` (`Animate`), `Turbofan.cs`, `Turbojet.cs`, `JetNozzle.cs` (`Afterburner.Audio`), `RotorShaft.cs`, `DuctedFan.cs`, `ConstantSpeedProp.cs`, `PropFan.cs`, `Transmission.cs` |
| Weapons | `Gun.cs` (`ShotSound`, `LoopSounds`), `Missile.cs` (`flightSound`, `basePitch`, `maxPitchSpeed`), `Laser.cs` |
| Airframe | `ControlSurface.cs` (split airflow), `AeroPart.cs` (hits, joint breaks), `Aircraft.cs` (`ThrowSparks`/scrape), `LandingGear.cs`, `Canopy.cs`, `EjectionSeat.cs` |
| Ambient & one-shots | `RandomSound.cs`, `TurbineFireSound.cs`, `Lightning.cs`, `Ship.cs` (water and hull sources) |
| Pilot state | `GLOC.cs` (master low-pass), `AoADisplay.cs`, `CockpitWarningLights.cs`, `WindowsTTS.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option`,
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2, and `ScriptingAssemblies.json`
  for the absence of audio middleware.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development).
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/).
- **No engineering talk, blog or paper was found.**
