# Nuclear Option — command, batteries and surface units

The third note on Nuclear Option, closing the gaps listed in
[`nuclear_option_combat.md`](nuclear_option_combat.md) §9. Where that note covers
one aircraft and its weapons, this one covers **everything above and beside it**:
the shared world model every AI reads, the target-scoring library they all share,
SAM and ship fire-control batteries, gun mounts in full, the seekers I had only
summarised, the helicopter AI, ground vehicles (whose *driving logic runs inside a
Burst job*), ship AI and navigation, and the strategic layer that decides what
gets built and where it goes.

Same source and caveats: read from the decompiled `Assembly-CSharp.dll` of the
retail Mono build. **No developer talk, blog or paper exists.** Tags: **[CODE]**
read from the assembly, **[PATCH]** the release notes, **[inferred]** my reading.
Nothing here comes from running the game.

The organising observation for this note:

> **There is one world model, one target-scoring function, and one reservation
> mechanism, and every AI in the game shares all three.** A fighter pilot, a SAM
> battery, a destroyer's fire control and an anti-radiation missile all read the
> same `TrackingInfo` dictionary, all call the same `CombatAI.AnalyzeTarget`, and
> all coordinate by incrementing the same two `sbyte` counters on the target they
> chose. **The coordination is not a system — it is two bytes.** §1 and §2 are
> those two bytes, and everything from §3 onward is a consumer of them.

Related: [`nuclear_option.md`](nuclear_option.md) (vehicle physics),
[`nuclear_option_combat.md`](nuclear_option_combat.md) (sensors, guidance, air AI),
[`nuclear_option_control.md`](nuclear_option_control.md) (input, AI procedures,
ground locomotion, wind),
[`nuclear_option_audio.md`](nuclear_option_audio.md) (audio),
[`ai_state_machines.md`](ai_state_machines.md),
[`nav_architecture.md`](nav_architecture.md) and
[`navigation.md`](navigation.md) (§8.2 is a shipped counterpoint),
[`spatial_queries.md`](spatial_queries.md).

---

## 1. The shared world model

**[CODE]** `TrackingInfo` is 90 lines and is the entire knowledge model of the
game. Every faction owns a `Dictionary<PersistentID, TrackingInfo> trackingDatabase`
on its `FactionHQ`; every sensor writes to it; every AI reads from it.

```csharp
public class TrackingInfo {
    public GlobalPosition lastKnownPosition;
    public float          lastSpottedTime;
    public PersistentID   id;
    public sbyte          attackers;         // ← units currently prosecuting this target
    public sbyte          missileAttacks;    // ← missiles currently in flight at it
    private Unit          unit;

    public bool Observed() => Time.timeSinceLevelLoad - lastSpottedTime < 4f;

    public GlobalPosition GetPosition() {
        if (Time.timeSinceLevelLoad - lastSpottedTime < 4f && TryGetUnit(out var u))
            lastKnownPosition = u.GlobalPosition();      // fresh: use the truth
        return lastKnownPosition;                        // stale: use the memory
    }

    public float GetStrategicPriority()
        => unit.definition.typeIdentity.strategic / (1 + missileAttacks + attackers);
}
```

**[inferred] Three things do a great deal of work here.**

**Fog of war is one timestamp and a four-second grace.** `GetPosition()` returns
the live position if the target was seen within 4 s, and the frozen last-known
position otherwise. There is no separate "believed position" that has to be
propagated, no per-observer belief state, no decay curve — the *same object* is
either a window onto the truth or a memory, decided by one comparison. Everything
downstream (missile datalink midcourse, AI target selection, the map display)
inherits fog of war for free by calling `GetPosition()` instead of
`unit.GlobalPosition()`.

**The freshness rule is also the accuracy rule.** `IsTargetPositionAccurate(unit,
tolerance)` is used all over the AI as a gate — "only shoot at this if we know
where it is to within 1000 m" — and it is implemented against the same timestamp
and last-known position. So *the decision to fire and the aimpoint the missile
gets are derived from the same record*, and they cannot disagree.

**Coordination is two `sbyte`s.** `attackers` counts units currently prosecuting
the target; `missileAttacks` counts missiles in the air at it. Both are
incremented by whoever commits and decremented when they stop, and both appear in
the denominator of every scoring function in the game:

```csharp
threat /= (float)(1 + 2 * Mathf.Max(trackingInfo.missileAttacks + trackingInfo.attackers, 0));
strategicPriority = strategic / (1 + missileAttacks + attackers);
```

**[inferred] That is the whole of multi-unit target deconfliction.** No blackboard,
no assignment solver, no squad coordinator. Every shooter independently scores
every target, and the act of committing makes that target less attractive to
everyone else. It is decentralised, it degrades gracefully (a shooter that dies
mid-commitment leaves a stale count, which decays as the missile self-destructs
and decrements it), and it is two bytes per tracked unit.

The `QueuedAttack` struct in `FireControl` (§4) even gives the counter
RAII-ish semantics — it increments `attackers` in its constructor and decrements
in *both* `Fire()` and `Cancel()`, so a plan that is abandoned releases its
reservation.

Updates flow in through one path: any detector that succeeds calls
`attachedUnit.NetworkHQ.RpcUpdateTrackingInfo(target.persistentID)`, so detection
is **shared faction-wide the instant any sensor sees anything.** There is no
per-unit knowledge; the faction knows or it does not.

---

## 2. `CombatAI` — one scoring function for every shooter

**[CODE]** A static class, ~370 lines, called by the aircraft AI, the SAM and ship
fire control, and the missile intercept logic. `AnalyzeTarget` returns an
`OpportunityThreat` pair — *how worth attacking is it* and *how dangerous is it to
me* — and it is structured as **a cascade of hard rejections followed by
multiplicative scoring**.

```csharp
OpportunityThreat ot = weaponStation.CalcOpportunityThreat(unit.definition, analyzer);
float opportunity = ot.opportunity, threat = ot.threat;

if (opportunity == 0f) return new OpportunityThreat(0f, threat);
if (trackingInfo.missileAttacks > weaponInfo.CalcAttacksNeeded(unit))
                                                    return (0f, threat);   // already saturated
threat /= 1 + 2 * max(missileAttacks + attackers, 0);                      // §1's deconfliction

float minAlt = targetRequirements.minAltitude * targetDistance / targetRequirements.maxRange;
if (unit.radarAlt < minAlt || unit.radarAlt > maxAltitude) return (0f, threat);
if (unit.speed > targetRequirements.maxSpeed)             return (0f, threat);
if (minIR    > 0f && !unit.HasIRSignature())              return (0f, threat);
if (minRadar > 0f && !unit.HasRadarEmission())            return (0f, threat);
if (weaponInfo.armorTierEffectiveness + optimism < unit.definition.armorTier) return (0f, threat);

// ... then scoring:
if (unit is Missile m) {
    if (m.targetID == analyzer.persistentID) { threat *= 3f; opportunity *= 3f; }   // it's aimed at ME
    else opportunity *= m.InterceptPriority(analyzer, 5000f, costPerRound);
}
opportunity *= InterceptViability(unit, analyzer, weaponStation, maxRange, dist, targetMaxSpeed);
opportunity *= Mathf.Lerp(1f, 0f, targetDistance / (maxRange * maxRangeMultiplier));
opportunity *= (lineOfSight || maxRange <= 10000f) ? 1 : 100;
opportunity *= 1f + Mathf.Sqrt(unit.definition.value) * 0.01f;
```

**[inferred] Four of these deserve calling out.**

**The minimum engagement altitude scales with range.**
`minAltitude · targetDistance / maxRange` — a SAM that can engage something at
50 m when it is 2 km away requires it to be at 500 m when it is 20 km away. That
is a one-line model of terrain masking and radar horizon *for the decision layer*,
consistent with the actual horizon test in the sensor layer, and it means low
flying degrades an enemy battery's willingness to shoot as well as its ability to
see.

**`InterceptViability` asks whether the shot can physically arrive:**

```csharp
Vector3 los     = NormalizedDirection(analyzerPos, targetPos);
Vector3 closing = target.rb.velocity + max(target.speed, targetMaxSpeed) * los;
float   ratio   = Dot(-los, closing) / weaponStation.WeaponInfo.GetMaxSpeed();
return Mathf.Min(maxRange * (1f + ratio) / targetDist - 1f, 1f);
```

It projects the target's escape rate onto the missile's top speed and shrinks the
effective range accordingly. A target running away at half your missile's speed
halves your reach; a target closing extends it. **The AI therefore declines
tail-chase shots it cannot win**, which is the single most common thing amateur
missile AI gets wrong.

**A 100× bonus for beyond-visual-range, non-line-of-sight weapons.** Blunt, but it
means a long-range SAM battery prefers the targets only it can service instead of
competing with the guns for something close.

**`GetExclusionRadius` is cube-root blast scaling, cached:**

```csharp
float r = Mathf.Pow(missile.GetYield(), 0.3333f) * 13f;    // memoised per WeaponInfo
```

Yield is an energy, radius goes as the cube root — correct — and the result is
cached in a dictionary keyed by `WeaponInfo` because it is a `GetComponent` away
otherwise. This radius becomes an `ExclusionZone` that friendly aircraft avoid
(§7.3 of the combat note) and that the nuclear targeting logic respects.

### 2.1 Distributing a salvo across targets

```csharp
private static void DistributeTargets(Aircraft aircraft, List<TargetAttack> attacks, List<Unit> outTargets) {
    attacks.Sort((a, b) => aircraft.definition.ThreatPosedBy(b.unit.definition.roleIdentity)
                   .CompareTo(aircraft.definition.ThreatPosedBy(a.unit.definition.roleIdentity)));
    while (attacks.Count > 0)
        for (int i = attacks.Count - 1; i >= 0; i--) {
            outTargets.Add(attacks[i].unit);
            if (--attacks[i].attacks <= 0) attacks.RemoveAt(i);
        }
    outTargets.Reverse();
}
```

**[inferred] Round-robin, not greedy.** Each target has a required number of shots
(`CalcAttacksNeeded`, from the weapon's pK against that armour tier); the loop
walks the list repeatedly issuing *one* shot per target per pass. So a six-missile
salvo at three targets goes 1-2-3-1-2-3, not 1-1-2-2-3-3. Then it reverses so the
highest-threat target is serviced first. **The ordering matters because the salvo
launches sequentially and the first missiles away are the ones that arrive.**

`ThreatPosedBy(roleIdentity)` is a per-aircraft-definition matrix — a fighter
ranks enemy fighters above trucks; a CAS aircraft the reverse — so "what should I
shoot first" is authored data on the airframe, not a global priority table.

### 2.2 Bravery

```csharp
if (target != null && bestOpportunity * bravery * 2f < 0.35f
 && HQ.GetAircraftThreat(target.persistentID) > bestOpportunity * bravery * 2f
 && range > weaponInfo.targetRequirements.maxRange * 2f) {
    target = null;                                    // not worth it, and it's far away
}
```

**[inferred]** A three-condition refusal: *the opportunity is poor*, *the target is
better defended than the opportunity justifies*, and *it is more than twice my
weapon range away so I would have to commit to get there*. All three must hold.
`bravery` is a per-unit scalar, so the same code produces a cautious AI and a
reckless one.

`GetSafeStandoffDist` complements it — it walks the tracking database for known
ground threats, takes `maxRange × 1.2` for each, and returns the largest
penetration depth. That is how the AI knows how far to sit off a defended area
**from the same shared world model**, so it only respects SAMs the faction has
actually detected.

---

## 3. Gun mounts, in full

### 3.1 Rate of fire is an accumulator with sub-frame timing

**[CODE]**

```csharp
if (queuedBullets < 1f && Time.timeSinceLevelLoad - lastFired > fireInterval) queuedBullets = 1f;
queuedBullets = (ticksSinceTriggerPull < 2)
              ? queuedBullets + Time.fixedDeltaTime * fireRate * 0.01667f      // rpm → rps
              : 0f;
queuedBullets = Mathf.Min(queuedBullets, bulletsLoaded);
while (queuedBullets >= 1f) {
    queuedBullets -= 1f;
    SpawnBullet(queuedBullets * fireInterval);        // ← sub-tick age of this round
    ammo--; bulletsLoaded--;
}
```

**[inferred] `SpawnBullet(queuedBullets * fireInterval)` is the detail worth
copying.** A 6,000 rpm cannon fires 100 rounds per second, i.e. two rounds per
50 Hz tick. Spawning both at the tick boundary produces visible *pairs* of tracers
with a gap — the classic minigun-looks-wrong artefact. Passing the fractional
remainder as a time offset lets `BulletSim` advance each round by its correct
sub-tick age, so the stream is evenly spaced regardless of fire rate or frame
rate. The first round is also released immediately on trigger pull
(`queuedBullets = 1f` when the interval has elapsed), so there is no fire-rate
latency on the first shot.

### 3.2 Barrel heat degrades three things at once

```csharp
float cooling = (coolingPerSecond + coolingPer100kph * gun.attachedUnit.speed * 0.036f) * dt;
heat -= cooling;
overheatFactor = Mathf.Clamp(heat / maxHeat - 1f, 0f, max);
if (overheatFactor > 0f) {
    gun.fireRate      = baseFireRate    - firerateDegradation * overheatFactor;
    gun.bulletSpread  = baseSpread      + accuracyDegradation * overheatFactor;
    gun.muzzleVelocity = info.muzzleVelocity - velocityDegradation * overheatFactor;
}
// and, past the limit, damage:
public void GunFired() {
    if (overheatFactor > 1f) { heat += heatPerShot * (overheatFactor - 1f); /* barrel damage FX */ }
    heat += heatPerShot;
}
```

**[inferred] Three things.** Cooling scales with **airspeed** (`coolingPer100kph ·
speed · 0.036`) — ram-air cooling, so a gun that overheats in the hover recovers
in a dive. Overheating degrades rate, dispersion **and muzzle velocity**
simultaneously, so an overheated gun is worse in every way rather than just
slower; and lower muzzle velocity feeds straight into the range-dependent damage
term (§3.4), so the effect compounds. And past `overheatFactor > 1` the heat
*added per shot grows with the overheat*, which is a positive feedback that makes
a genuinely destroyed barrel unrecoverable — with a hard ceiling on the degradation
(`Mathf.Min(baseFireRate/firerateDegradation * 1.01f, muzzleVelocity/velocityDegradation)`)
so the numbers cannot go negative.

### 3.3 Recoil, ejection, tracers

```csharp
if (attachedUnit.LocalSim && attachedUnit.rb != null)
    attachedUnit.rb.AddForceAtPosition(-transform.forward * recoilImpulse, recoilTransform.position,
                                       ForceMode.Impulse);
```

Recoil is a **real impulse at the gun's position**, so a wing-mounted cannon yaws
the aircraft and a nose gun does not. It composes with everything in
[`nuclear_option.md`](nuclear_option.md) — the fly-by-wire has to trim it out, and
you can feel it.

Tracers are issued by a **counter, not a random roll**:

```csharp
tracerSeed++;
if (tracerSeed > tracerRatio) { tracerSeed -= tracerRatio; tracer = true; }
```

so a 1-in-5 tracer ratio is exactly every fifth round, and the stream reads as
evenly spaced. A random roll would clump.

### 3.4 The projectile

Covered in [`nuclear_option_combat.md`](nuclear_option_combat.md) §5.1; the two
points worth repeating here are that **damage is proportional to kinetic energy**
(`pierceDamage · v² / v₀²`, so range degrades penetration automatically) and that
each round carries a `reliability = Random.value` rolled **at spawn**, so the
outcome is decided at fire time and is consistent for that round rather than
re-rolled at impact.

A gun with a `guidedProjectile` spawns a `Missile` instead of a bullet and breaks
out of the muzzle loop — so guided shells and gun-launched missiles share the
whole mount, magazine, heat and reload model.

---

## 4. Fire control: SAM and ship batteries

**[CODE]** `FireControl` (616 lines) is the battery brain. It has five target
acquisition modes (`parentUnitTargetDetector`, `assignedTargetDetectors`,
`searchForRadar`, `datalink`, `strategicStrike`), so the same component drives a
self-contained SAM vehicle, a battery slaved to a separate search radar, a ship's
missile system fed by the faction datalink, and a strategic launcher.

### 4.1 The planning cycle

```
HQTargetAssessment()          every N seconds
   ├─ HQ.GetTargetsWithinRange(...)            from the shared tracking database
   ├─ CombatAI.AnalyzeTarget per target        the shared scorer (§2)
   ├─ reject: already in the salvo list; enough missiles already inbound;
   │          I already have a missile in the air at it
   ├─ new FireControlTarget(...) → shotsRequired = CalcAttacksNeeded − (missileAttacks + attackers)
   └─ sort ascending by combined score
PlanSalvo()                   builds queuedAttacks
   └─ round-robin over weapon stations, consuming shotsRequired from the best target down
   └─ await Delay(planningTimePerFire × iterations)      ← the battery takes TIME to plan
LaunchSalvo()
```

**[inferred] Three good decisions.**

**`shotsRequired` is computed net of what is already committed.** A target that
needs three hits and already has two missiles inbound is worth one more shot, not
three. The subtraction happens once, at plan time, against the shared counters
from §1.

**Ammunition is allocated round-robin across launchers, not per target.** The loop
walks `subscribedWeaponStations` cyclically while decrementing each target's
requirement, so a salvo drains launchers evenly and a battery that loses one cell
degrades gracefully.

**The battery's reaction time is proportional to the size of the salvo it planned**
(`planningTimePerFire * iterations`). That is a lovely bit of implicit modelling —
a two-missile engagement launches quickly, a twelve-missile mass raid takes
noticeably longer to answer — and it costs one multiply on a delay that had to
exist anyway.

`DistributeTurretTargets` does the same job for gun turrets, and
`DeployOrStow` handles launcher erection with an async sequence, so a stowed SAM
has a real time-to-first-shot.

### 4.2 The reservation with a destructor

```csharp
private struct QueuedAttack {
    public QueuedAttack(Unit target, TrackingInfo ti, WeaponStation ws) {
        ...; ti.attackers++;                       // commit
    }
    public bool StillValid(FactionHQ hq) => target != null && !target.disabled
                                         && target.NetworkHQ != null && target.NetworkHQ != hq;
    public void Fire(Unit u)   { weaponStation.Fire(u, target); trackingInfo.attackers--; }
    public void Cancel(Unit u) { trackingInfo.attackers--; }
}
```

**[inferred]** A plan that is made reserves; a plan that fires or is abandoned
releases. `StillValid` re-checks between planning and launching, because in the
seconds the battery spent planning the target may have died or changed sides. This
is the smallest correct implementation of "reserve, then verify, then commit" I
have seen in a game AI, and it is fifteen lines.

---

## 5. The seekers I had only summarised

### 5.1 SARH — and the reason `GetEvasionPoint()` is virtual

**[CODE]** `SARHSeeker` computes its tracking strength from **the launching
radar's scan point**, not its own:

```csharp
radarSource      = missile.owner.radar as Radar;
radarSourcePoint = radarSource.GetScanPoint();
...
trackingStrength = radarReturn.GetRadarReturn(radarSourcePoint.position, null, missile,
                                              dist, clutter, radarParams, triggerWarning: false);
```

with the same horizon test, the same clutter formula and the same
`GetSignalStrength` as everything else — but evaluated **from the shooter's
geometry**. Lose the illumination (the shooter turns away, dies, is jammed, or the
target breaks line of sight to *the shooter*) and `SearchMode()` coasts on
`knownPos += knownVel · dt` until `lockPersistence` expires.

And then:

```csharp
public override GlobalPosition GetEvasionPoint()
    => missile.radar != null ? missile.radar.GetScanPoint().GlobalPosition()
                             : missile.GlobalPosition();
```

**[inferred] This is the best small piece of design in the combat code.**
`GetEvasionPoint()` is the base class's answer to *"what should a defender
manoeuvre against?"* — and for an active-radar missile it is the missile, while for
a semi-active missile it is **the illuminating aircraft**. The AI's notch
computation (combat note §7.5) uses `GetEvasionPoint()` and therefore
**automatically notches the correct object for each missile type**, with no
per-type logic in the evasion code. One virtual method turns a whole class of
tactical distinction into polymorphism.

`SendTargetInfo` also gates the lead: `missile.IsArmed() ? GetLeadVectorWithAccel(...)
: Vector3.zero` — an unarmed missile in its first seconds flies at the raw target
position rather than a lead point, which keeps the initial trajectory sane.

### 5.2 ARM — and salvo deconfliction across radars

**[CODE]** `ARMSeeker` builds its target list **from the RWR**:

```csharp
missile.onRadarPing += ARMSeeker_OnRadarPing;
...
public void ARMSeeker_OnRadarPing(Aircraft.OnRadarWarning source) {
    if (Vector3.Angle(source.emitter.position - transform.position, missile.rb.velocity - emitterVel)
        < maxTargetAngle && !recentReturns.Contains(source.radar))
        recentReturns.Add(source.radar);
}
```

Every radar that illuminates the missile is added to `recentReturns`. Every 4.5 s
it re-scores:

```csharp
float score = 10000f / (dist * (angleOffBoresight + 1f));
score /= (trackingData.missileAttacks + 1);           // ← don't pile onto an already-targeted radar
```

**[inferred] The `missileAttacks` divisor is §1's coordination reappearing in a
missile.** A salvo of anti-radiation missiles spreads itself across the emitters
rather than all going for the loudest one. And the implementation detail is
charming: before scoring, the seeker **decrements its own current target's counter
and re-increments it afterwards**, so it does not penalise its own target for its
own attack when deciding whether to switch.

Shut the radar down and `TrackCurrentTarget()` returns null (`!targetedRadar.activated`,
or a line-of-sight check every 0.25 s), at which point the missile goes inertial:

```csharp
targetDrift += Random.insideUnitSphere * (inertialDrift * Time.deltaTime / 2f);
```

so it continues to the last known position with an accumulating error. **Shutting
down works, and the longer you stay dark the more it misses by.**

### 5.3 Optical, laser, inertial

`OpticalSeeker` and its four variants (`Bomb`, `HighDrag`, `Shell`,
`CruiseMissile`) share the `GetTargetParameters()` / `SendTargetInfo()` shape and
differ mainly in flight profile and fin deployment timing. `LaserSeeker` tracks a
`LaserDesignator` spot and requires the designating unit to keep the spot on
target. `InertialSeekerShell` and `BallisticMissileGuidance` have no seeker at all
and fly a computed trajectory to a coordinate.

### 5.4 Two trajectory modifiers worth stealing

**`TopAttack`** — 49 lines:

```csharp
public Vector3 ApplyTopAttack(GlobalPosition missilePos, GlobalPosition targetPos, float speed) {
    if (!InRange(missilePos, targetPos, maxRange)) return Vector3.zero;
    Vector3 horiz = targetPos - missilePos; horiz.y = 0f;
    return Mathf.Clamp01(horiz.magnitude * 0.5f / minRange - 0.2f) * Amount * Vector3.up;
}
public bool ShouldUseTopAttack(Unit target)
    => armorSelective ? (target.definition.armorTier >= 5f && target.maxRadius < 10f) : true;
```

An upward aimpoint offset that **fades to zero as the missile closes**, so the
missile climbs and then dives. And `armorSelective` means it only bothers for
heavily armoured *small* targets — a tank, not a building — which is exactly when
attacking the thin top plate is worth the energy.

**`JinkEvasion`** — 44 lines:

```csharp
if (targetDist < minRange || targetDist > maxRange || speed < minSpeed) return Vector3.zero;
if (jinkOffset == Vector3.zero || Time.timeSinceLevelLoad - lastJink > period) {
    jinkOffset = Vector3.ProjectOnPlane(Random.insideUnitSphere, targetPos - missilePos)
                        .normalized * amount;
    jinkOffset.y = flat ? 0f : Mathf.Max(jinkOffset.y, 0f);       // never jink downward (unless sea-skimming)
}
return jinkOffset * targetDist;
```

A random offset **perpendicular to the line of sight**, re-rolled every `period`,
and **scaled by range** — so it is an angular jink that shrinks automatically as
the missile closes and vanishes at impact. Bounded by a minimum speed (no jinking
when you cannot afford the drag) and a range band (not in the terminal phase). The
`y ≥ 0` clamp stops it flying into the ground, and `flat` disables the vertical
component entirely for sea-skimmers.

**[inferred]** Both are `[Serializable]` plain classes with no `MonoBehaviour`
overhead, taking positions and returning an offset, so any seeker can opt in with
one field and one `+=`. That composability is only possible because of the
aimpoint architecture in combat note §2.4.

---

## 6. Helicopter AI

### 6.1 The autopilot blends hover and forward flight by speed

**[CODE]** `AutopilotHelo.AutoAim` computes *two* error triples and lerps between
them on normalised airspeed:

```csharp
float blend = Mathf.SmoothStep(0f, 1f, aircraft.speed / aircraftParameters.maxSpeed);

float pitchHover   = -GetAngleOnAxis(desiredTilt, transform.up, transform.right);
float pitchForward = -GetAngleOnAxis(waypointDelta, velocity + forward * 20f, transform.right);
float pitch        = Mathf.Lerp(pitchHover, pitchForward * 4f, blend);
//  ... same shape for yaw and roll ...
hoverController.ApplyInputs(controlInputs, new Vector3(pitch, yaw, roll), localAngularVelocity);
```

**[inferred] One controller covers hover, transition and cruise** because the
*error definition* changes rather than the controller. In the hover, "pitch error"
means the difference between the current attitude and a commanded tilt vector; in
forward flight it means the angle between the flight path and the waypoint. Both
are angles in the same units, so a single PID consumes either, and the lerp makes
the transition continuous.

The desired tilt vector is itself a small assembly:

```csharp
Vector2 tilt = tiltPID.GetOutput(new Vector2(toDest.x, toDest.z), dt);
Vector3 desiredTilt = Vector3.up
                    + new Vector3(Clamp(tilt.x, -maxTilt, maxTilt), 0f, Clamp(tilt.y, -maxTilt, maxTilt))
                    + obstacleNormal * terrainAvoidanceUrgency * 0.2f;
```

— a PID on horizontal position error producing a lean angle, plus **terrain
avoidance folded directly into the attitude command** as a push along the obstacle
normal, rather than existing as a separate override behaviour.

### 6.2 The autopilot reads the rotor's physics state back

This is the part I did not expect:

```csharp
foreach (RotorShaft shaft in rotorShafts) {
    vrs   += shaft.GetVRSFactor();                            // vortex ring state, from the blade model
    droop += Mathf.Min(shaft.GetRPM() - nominalRPM, 0f);      // rotor RPM shortfall
}
vrs /= rotorShafts.Length;  droop /= rotorShafts.Length;

if (vrs > 0.4f)                                               // ← VRS ESCAPE
    destination = aircraft.GlobalPosition() + flatForward * 10000f;
...
throttle += 0.5f * terrainAvoidanceUrgency;
throttle += vrs * 2f;                                         // more collective while in VRS
if (aircraft.radarAlt > 7f) throttle += Mathf.Min(droop, 0f);  // ← trade height for rotor RPM
```

**[inferred] The AI is closing a loop around the emergent physics from
[`nuclear_option.md`](nuclear_option.md) §7.7.** `GetVRSFactor()` is the state the
blade-element model computes when the rotor descends into its own downwash; past
0.4 the AI **abandons its destination and flies straight ahead**, which is the
real VRS recovery (gain translational airflow). And the droop term is real
technique too: above 7 m it lets a drooping rotor *reduce* collective, trading
altitude to preserve rotor RPM, because a rotor that bogs down cannot be recovered
by pulling harder.

Tail rotor failure has its own branch:

```csharp
tailRotorFailure = tailRotor.GetRPM() < 1000f;
if (tailRotorFailure) {
    altitudeHold = 0f;
    if (radarAlt > 10f && gearState == LockedRetracted) SetGear(deployed: true);
    if (radarAlt < spawnOffset.y + 0.2f) { brake = 1f; throttle = 0f; return; }
    if (radarAlt < 20f && speed < 15f)   { throttle = 0f; brake = 1f; return; }
}
```

Gear down, fly it on, chop the collective at the bottom — a run-on landing
procedure, triggered by reading the tail rotor's actual RPM.

Compound helicopters get their pusher managed on the same pass
(`customAxis1 = f(distance, speed)`, forced to 0.5 when the rotor is drooping or
the destination is behind, and main-rotor collective capped at 0.5 above 80 m/s).

### 6.3 Masking, and the gun-run orbit

**[CODE]** `AIHeloCombatState` differs from the fixed-wing AI in two revealing
ways.

**It checks line of sight from 5 m below itself:**

```csharp
lineOfSight = currentTarget.LineOfSight(aircraft.transform.position - Vector3.up * 5f, 5f);
```

**[inferred]** So "can I see the target" is evaluated as though the helicopter were
lower than it is — a deliberate pessimism that biases the AI toward unmasking
before it commits.

**Its altitude accumulator has the opposite asymmetry to the fixed-wing one:**

```csharp
desiredHeight += needLineOfSight ? 10 : -20;      // climbs at 10, descends at 20
```

The aeroplane's `ManageAltitude` climbs at 20 and descends at 10 (combat note
§7.7). **[inferred] The helicopter's default is to sink toward cover and it climbs
only when it must see something; the aeroplane's default is to keep energy.** Same
mechanism, opposite sign, and it produces completely different-looking flying from
one constant.

**`GunshipMode` is a standoff orbit in fifteen lines:**

```csharp
float desiredRange = weaponInfo.targetRequirements.maxRange * 0.66f - gunshipTime * 5f;  // closes over time
Vector3 toTarget = targetKnownPosition - aircraftPos;  toTarget.y = 0f;
Vector3 tangent  = Vector3.Cross(toTarget, Vector3.up);

// orbit the way that keeps the turret bearing on the target
if (weaponStation.TurretTraverseRange() < 60f && weaponStation.GetFiringConeDirection(out var aim, out _)) {
    if (Vector3.Dot(aim, aircraft.transform.right) < 0f) tangent *= -1f;
} else if (Dot(tangent, forward) < Dot(-tangent, forward)) tangent *= -1f;

float rangeError = (targetDist - desiredRange) / desiredRange;
if (Mathf.Abs(rangeError) < 0.4f) { rangeError *= 0.5f; if (!lineOfSight) desiredHeight += 20f * dt; }
else rangeError *= 3f;

destination = aircraftPos + Vector3.RotateTowards(tangent, toTarget, rangeError, 1f).normalized * 8000f;
```

**[inferred]** The destination is the **tangent** to a circle around the target,
rotated toward or away from the target in proportion to the range error — so the
helicopter flies a circle at `desiredRange`, spiralling in when too far and out
when too close, with `RotateTowards` doing the blending. The orbit direction is
chosen to keep a limited-traverse turret bearing. And `desiredRange` shrinks by
5 m/s of engagement time, so a gun run that is not producing results closes in.
Fifteen lines for a behaviour most games implement as a scripted attack pattern.

---

## 7. Ground vehicles

### 7.1 The driving logic runs inside a Burst job

**[CODE]** This surprised me. `GroundVehicleJob_Math1` is an `IJobParallelFor`
that contains not just the physics but **the AI**:

```csharp
public void Execute(int i) {
    ref GroundVehicleFields f = ref fields[i].Ref();
    Execute(i, ref f, ref transformValues.GetReadOnlyRef(i));   // state, stationary detection
    UpdateRayCommands(i, ref f, ...);                            // queue the ground-sample raycast
}

private void Execute(...) {
    ...
    if (GroundVehicleJobSettings.ShouldRunInputs(i, shared.Ref().tickOffset))
        Inputs(ref f, ref transform);         // ← steering, throttle, braking, obstacle avoidance
    f.engineOutput = f.inputs.throttle * f.acceleration;
}
```

`ShouldRunInputs(i, tickOffset)` and `ShouldRunSampleGround(i, tickOffset)` are
**round-robin amortisation inside the job** — index `i` against a rolling tick
offset, so only a fraction of vehicles run their AI or re-sample the ground on any
given tick. **[inferred]** That is the same amortisation pattern as the aircraft's
`PartChecker` ([`nuclear_option.md`](nuclear_option.md) §5.1), applied to a
parallel job rather than a main-thread loop, and it means the per-tick cost of a
few hundred vehicles is a fraction of their nominal cost.

The main thread's only jobs are to hand the vehicle its steer point
(`UpdateJobFields_Pathfinder`) and its obstacle list (`UpdateJobFields_Obstacles`),
both of which involve managed structures the job cannot touch.

### 7.2 Speed for the corner

The direct answer for ground vehicles, and it is four lines:

```csharp
SteeringInfo s = fields.steeringInfoNullable.Value;
float k          = Mathf.Max((s.nextWaypointAngle - 10f) * 0.1f, 0.1f);
float speedLimit = Mathf.Clamp(80f / k, 30f, fields.topSpeedOnroad);           // km/h
if (Vector3.Dot(steerDirection, transform.Forward()) < 0.5f) speedLimit = 30f;  // >60° off: crawl

if (fields.speed > speedLimit * 0.277777f)                                      // km/h → m/s
    { fields.inputs.brake += Mathf.Clamp01((fields.speed - speedLimit * 0.277f) * 0.1f);
      fields.inputs.throttle = 0f; }
else fields.inputs.throttle = 1f;
```

**Speed limit is inversely proportional to the corner angle.** A 10° bend imposes
no limit (`k` floors at 0.1, giving 800 km/h, clamped to top speed); a 90° corner
gives `80/8 = 10`, clamped up to the 30 km/h floor. Steering more than 60° off the
current heading forces the crawl speed directly.

**[inferred] And the other half of the mechanism is in the *path* code, not here.**
`PathfindingAgent.GetSteerpoint` only reports a non-zero corner angle when the
vehicle is inside its own braking distance:

```csharp
nextWaypointAngle = (speed * speed * 0.2f < distanceToWaypoint) ? 0f
                  : Vector3.Angle(forward, nextLegDirection);
```

`speed² · 0.2` is a braking-distance proxy. So **the corner is invisible until it
matters, and becomes visible exactly when braking must start.** Two systems, one
signal, and neither needs to know the other's constants. That division —
*navigation decides when to warn, locomotion decides how hard to brake* — is the
cleanest expression of this idea I have seen.

### 7.3 Steering, stuck recovery and bulldozing

```csharp
// proportional steering with a yaw-rate damper
inputs.steering = 0.05f * Mathf.Clamp(GetAngleOnAxis(forward, steerDirection, up), -10f, 10f);
inputs.steering -= angularVelocity.y * 0.2f;

// stuck detection → reverse with a wiggle
if (Mathf.Abs(speed) < 1f && throttle > 0f) stuckTimer += 0.2f;
if (stuckTimer > 2f) { reverseTimer = 3f; stuckTimer = 0f; }
if (reverseTimer > 0f) {
    reverseTimer -= 0.2f;
    inputs.throttle = -1f;
    inputs.steering = Mathf.Clamp(10f * Mathf.Sin(timeSinceLevelLoad), -1f, 1f)
                    * Mathf.Clamp01(Mathf.Abs(speed) * 0.05f) - angularVelocity.y * 0.2f;
}

// bulldoze: stop yielding if we have been yielding too long
bulldozeTimer += throttleInhibit * 0.2f;  bulldozeTimer -= 0.02f;
bulldozeTimer = Mathf.Clamp(bulldozeTimer, 0f, 3f);
if (bulldozeTimer < 1f) inputs.throttle -= Mathf.Min(throttleInhibit, 0.5f);
```

**[inferred] The bulldoze timer is the deadlock breaker and it is three lines.**
Obstacle avoidance inhibits throttle; the inhibition accumulates; once a vehicle
has been yielding for about a second of accumulated pressure it **stops yielding
and pushes through**. Without it, two vehicles that each yield to the other stand
still forever — the classic convoy deadlock. With it, one of them eventually wins,
and because the timer decays at a fixed rate the behaviour self-clears.

The stuck-recovery wiggle (`sin(time)` scaled by speed) is the standard trick, and
scaling it by current speed means it does nothing until the vehicle is actually
moving backwards.

Disabled vehicles get a slow sinusoidal steering slew so a wreck coasts to a stop
askew rather than in a straight line.

### 7.4 Obstacle avoidance is two behaviours by size

```csharp
if (obstacle.Radius < 8f) {                                     // small: repel and slow
    Vector3 away = -toObstacle.normalized;
    float facing = Mathf.Max(Dot(-away, transform.Forward()), 0f);
    away *= Mathf.Min(100f * (1f + facing) / sqrDist, 0.5f);
    throttleInhibit += facing / Mathf.Max(sqrDist * 0.1f, 2f);
    steerDirection += away;
} else {                                                        // large: steer around the rim
    Vector3 closestApproach = position + Vector3.Project(toObstacle, steerDirection);
    if (closestApproach.y < obstacle.Position.y + obstacle.Top
     && InRange(closestApproach, obstacle.Position, combinedRadius)) {
        Vector3 rimPoint = obstacle.Position
                         + NormalizedDirection(obstacle.Position, closestApproach) * combinedRadius;
        accumulated += NormalizedDirection(position, rimPoint);   // steer at the rim, not away
    }
}
if (count > 0) steerDirection = accumulated;                     // large obstacles REPLACE the steer
steerDirection += repulsion;                                     // small ones are additive
```

**[inferred] Small obstacles push, large obstacles are navigated around.** For a
large obstacle the code projects the obstacle onto the intended path to find the
closest approach, tests whether that point is actually inside the combined radius
*and below the obstacle's top* (so you can pass under a bridge span), and if so
steers at the point on the obstacle's rim rather than directly away — which
produces a tangent path instead of a bounce. And crucially, large-obstacle
steering **replaces** the steer direction while small-obstacle repulsion is
**added**, so a truck avoids a building decisively and jostles past a lamppost.

### 7.5 Navigation: A*, roads, and a carrot

**[CODE]** The navigation stack is small: `NodeGrid` (a uniform grid with a
`traversable` flag and a float `traversability` cost multiplier), `AStar.Node`, a
binary `Heap<T>`, and `Pathfinding.FindPath` — a textbook A* with octile distance
(`14·min + 10·(max−min)`) and cost divided by traversability. Alongside it,
`RoadPathfinding` (`RoadNetwork`, `Road`, `Node`) is a **road graph**, and
`PathfindingAgent` glues them together.

The interesting part is not the search, it is the **steer point**:

```csharp
Vector3 lateral = Vector3.Cross(forward, -Vector3.up) * 2f;      // ← drive on the right
GlobalPosition wp0 = waypoints[0] + lateral;
GlobalPosition wp1 = waypoints[1] + lateral;

GlobalPosition carrot = wp0 + Direction(wp0, wp1).normalized * 20f;    // push past the corner
float d = Distance(position, carrot);
carrot -= Direction(wp0, wp1).normalized * Mathf.Min(d * 0.5f, 20f);   // then pull back by half the range

bool arrived = d < 20f
            || (d < 60f && Vector3.Dot(Direction(position, wp0), forward) < 0f);   // or it's behind us
```

**[inferred] Three details.** The 2 m lateral offset means traffic **drives on the
right** and oncoming convoys pass rather than collide, for the cost of one cross
product. The carrot is pushed 20 m *past* the waypoint toward the next one and
then pulled back proportionally to range, so the vehicle cuts corners smoothly
instead of driving to each waypoint and turning on the spot. And waypoint
acceptance has a second condition — *the waypoint is behind me* — which is what
prevents the classic orbit-around-a-missed-waypoint failure.

There is also a periodic (4 s) sanity check that the next 100 m is not water, with
`StopImmediately()` if a ground vehicle is about to drive into the sea — a guard
against a path that was valid when planned and is not now.

Read against [`navigation.md`](navigation.md) and
[`nav_architecture.md`](nav_architecture.md): this is the *coarse graph* end of
that note's spectrum, with no navmesh anywhere. **[inferred]** For a game whose
ground units drive between towns on roads and whose player is in an aeroplane, a
road graph plus a grid plus local avoidance is exactly the right amount of
navigation, and it is a useful data point that a shipped game at this scale needed
nothing more.

---

## 8. Ships

### 8.1 Throttle is the cosine of the steering error

**[CODE]** `ShipAI.Steer`, on a 0.2 s tick:

```csharp
Vector3 steer = shipSteerpoint?.steerVector.normalized ?? transform.forward;
foreach (Obstacle o in obstacles) {                     // inverse-square repulsion
    float r = ship.maxRadius + o.Radius + 50f;
    if (InRange(o.position, ship.position, r * 8f))
        steer -= (o.position - ship.position).normalized * (1f / (sqrDist / (r * r)));
}
steer = steer.normalized;

inputs.throttle = Vector3.Dot(steer, ship.transform.forward);      // ← the whole speed policy
if (inputs.throttle < 0f && Dot(forward, velocity) < 0f) inputs.throttle = 0f;
inputs.steering = Mathf.Clamp(GetAngleOnAxis(forward, steer, up) * -0.1f, -1f, 1f);
```

**[inferred] `throttle = Dot(steerDirection, forward)` is a one-line speed policy
and it is almost perfect for a ship.** Pointing at the destination gives full
ahead; 60° off gives half; 90° off gives zero, so the ship coasts and lets the
rudder bring the head round; beyond 90° it commands astern — unless it is already
making sternway, in which case it stops rather than fighting itself. Combined with
the rudder authority scaling in [`nuclear_option.md`](nuclear_option.md) §11.6
(`steering × (1 + speed · momentumFactor)`), the result is a ship that carries way
into its turns and does not try to accelerate in the wrong direction.

Waypoint acceptance scales with the hull:
`(unit.maxRadius + 50f) * 5f` — a carrier's acceptance radius is far larger than a
patrol boat's — and the steer point **slides toward the next waypoint in
proportion to how close you are**, which is corner-cutting sized to the vessel's
turning circle without ever computing one.

### 8.2 Shore avoidance invents an obstacle

```csharp
if (Physics.Linecast(keel.position, keel.position + forward * 400f + velocity * 5f, out hit, Statics)) {
    LevelInfo.roadNetwork.TryGetNearestPoint(shipPos, out var nearestRoad, out _);
    LevelInfo.seaLanes.TryGetNearestPoint(shipPos, out var nearestLane, out _);
    Vector3 escape = Vector3.Lerp(nearestLane - shipPos, -(nearestRoad - shipPos), 0.5f).normalized;
    escape.y = 0f;
    shoreObstacle.Transform.position = hit.point - escape * hit.distance * 0.5f;
}
obstacles.Add(shoreObstacle);
```

**[inferred] A 400 m plus five-seconds-of-travel look-ahead, and on a hit the ship
synthesises a *virtual obstacle* placed so that repelling from it pushes the ship
toward navigable water** — the escape direction being a blend of "toward the
nearest sea lane" and "away from the nearest road", roads being a proxy for land.
Rather than adding a special shore-avoidance behaviour, it manufactures an input
to the obstacle repulsion that already exists. That is a good pattern: **express a
new constraint in terms of a mechanism you already have.**

`LandingCraftAI`, `AssaultCarrierAI`, `MobileArtilleryAI` and `RearmVehicleAI` are
thin subclasses over the same steering, differing in their state machines
(beaching, launching aircraft, shoot-and-scoot, rearming) rather than in how they
move.

---

## 9. The strategic layer

**[CODE]** `FactionHQ` is 2,092 lines and is the war. It owns the tracking
database (§1), the unit registry, airbases, depots, exclusion zones, the economy
and the AI deployment loop.

### 9.1 Economy

```csharp
private void DistributeFunds() {
    float excess = factionFunds > excessFundsThreshold
                 ? (factionFunds - excessFundsThreshold) * Clamp01(excessFundsDistributePercent) : 0f;
    float pot = (regularIncome + bonusIncome + excess) * factionPlayers.Count * 0.5f;
    pot = Mathf.Min(pot, factionFunds);
    float perPlayer = pot / Mathf.Max(factionPlayers.Count, 1);
    foreach (Player p in GetPlayers(false)) p.AddAllocation(perPlayer);
    AddFunds(-pot);  bonusIncome = 0f;
}
```

**[inferred]** Two mechanisms in one function. The faction accumulates funds and
pays out a per-player allocation scaled by player count — so a full server has a
bigger total economy but the same per-player rate. And the **excess-funds valve**
means a faction sitting on a surplus distributes it faster, which stops a
dominant side hoarding and stops a losing side being starved by its own
underspending.

Income is earned by named actions — `ReportReconAction` pays for *detecting* units
the faction had not tracked or had stale positions for (`TargetDetector.DetectTarget`
accrues `0.05 · sqrt(value)` for a new contact, `0.01 · sqrt(value)` for refreshing
a stale one, batched until it crosses a threshold). **[inferred] Reconnaissance is
a paid role, and the payment is derived from the state of the shared tracking
database rather than from a mission objective.**

### 9.2 AI force generation

```csharp
private void DeployAIAircraft() {
    float limit = AIAircraftLimit + enemyPlayerCount * addAIPerEnemyPlayer
                                  - friendlyPlayerCount * reduceAIPerFriendlyPlayer;
    if (activeAIAircraft.Count >= limit) return;

    Shuffle(Encyclopedia.i.aircraft);                       // in-place Fisher-Yates
    int reserve = reserveAirframes + friendlyPlayerCount * extraReservesPerPlayer;
    foreach (AircraftDefinition def in aircraft) {
        if (!AircraftSupply.TryGetValue(def, out var supply) || supply.Count <= reserve) continue;
        foreach (var (airbase, _) in airbasesSorted)
            if (airbase.CanSpawnAircraft(def)) { ... airbase.TrySpawnAircraft(...); return; }
    }
}
```

**[inferred] The AI force scales inversely with friendly players and directly with
enemy players** — so a lopsided server is balanced by the bot count rather than by
handicapping anyone, and the effect is smooth rather than a difficulty setting.
The **reserve** is the other half: AI aircraft may only draw from supply *above* a
reserve floor that grows with the number of human players, so **bots never spend
the airframes humans will need.** That is a genuinely thoughtful piece of
multiplayer design expressed as one comparison.

The type is chosen by shuffling the encyclopedia and taking the first affordable
entry, so the AI fields a varied force without a composition table. Airbases are
re-sorted every 30 s (`SortAirbases`, `SortDepots`) and the first one that *can*
spawn the type wins, so production naturally migrates to whichever bases are
intact and forward.

`DeployVehicles` is the same shape for ground units against `VehicleDepot`s, and
both decrement a shared `ModifyUnitSupply` pool — so aircraft and vehicles compete
for the same industrial output.

### 9.3 Escalation

```csharp
private void CheckEscalation() {
    float old = MissionManager.i.currentEscalation, now = factionScore;
    if (now > old) {
        MissionManager.i.NetworkcurrentEscalation = factionScore;
        ReportIfAbove(tacticalThreshold,  "Warning: Tactical nuclear weapons are cleared for deployment");
        ReportIfAbove(strategicThreshold, "Warning: Strategic nuclear weapons are cleared for deployment");
    }
    void ReportIfAbove(float t, string msg) { if (old < t && now >= t) MessageManager.i.RpcAllHQMessage(msg); }
}
```

**[inferred] The escalation ladder is a single global scalar — the highest faction
score seen so far — with two thresholds.** It is monotonic (it only ever rises),
it is shared between factions, and crossing a threshold is announced to everyone.
So nuclear release is a *consequence of how hard the war has been fought*, computed
from the score both sides are already generating, and the game's central dramatic
mechanic costs about twenty lines. The local function with the closure over
`old`/`now` is exactly the right shape for "fire an event on a threshold
crossing".

Warhead stockpiles are tracked separately (`GetWarheadStockpile`,
`GetWarheadAvailableForAI` — the AI gets a smaller allowance than players), and
firing one creates an `ExclusionZone` (§2's cube-root radius) that friendly
aircraft route around.

---

## 10. What is worth taking

1. **One shared world model, and fog of war as a single timestamp.** (§1.)
   `GetPosition()` returns the truth if seen within N seconds and the memory
   otherwise. Every consumer inherits fog of war by calling it, and belief state
   never has to be propagated or decayed.

2. **Coordinate through counters on the target, not through a coordinator.**
   (§1, §2, §4.2, §5.2.) Two `sbyte`s — `attackers` and `missileAttacks` — appear
   in the denominator of every scoring function, so committing to a target makes
   it less attractive to everyone else. Decentralised, degrades gracefully, and it
   is two bytes.

3. **Give the reservation a lifetime.** (§4.2.) Increment on plan, decrement on
   fire *or* cancel, and re-validate between planning and committing. Fifteen
   lines for "reserve, verify, commit".

4. **One scoring function for every shooter.** (§2.) Hard rejections first, then
   multiplicative modifiers. A fighter, a SAM battery and a ship all call it, which
   is why they behave consistently and why tuning it tunes everything.

5. **Ask whether the shot can physically arrive.** (§2.) `InterceptViability`
   projects the target's escape rate onto the weapon's speed and shrinks effective
   range. It is the cure for AI that takes tail-chase shots it cannot win.

6. **Make the decision layer's constraints agree with the physics layer's.**
   (§2.) The minimum engagement altitude scales with range in the *scorer* the same
   way the radar horizon does in the *sensor*, so the AI's willingness and its
   ability degrade together.

7. **Split "when to warn" from "how to react".** (§7.2.) Navigation reports a
   corner angle only when the vehicle is inside its braking distance; locomotion
   converts that angle into a speed limit. Neither knows the other's constants and
   both stay simple.

8. **Throttle as the cosine of the steering error.** (§8.1.) One dot product gives
   a vehicle that slows for turns, coasts through the tightest part, and does not
   accelerate in the wrong direction.

9. **Break yielding deadlock with an accumulating patience timer.** (§7.3.)
   `bulldozeTimer += inhibition; bulldozeTimer -= constant;` and stop yielding past
   a threshold. Three lines, and it is the difference between a convoy and a
   traffic jam.

10. **Express a new constraint through a mechanism you already have.** (§8.2.)
    Shore avoidance manufactures a virtual obstacle rather than adding a shore
    behaviour, so it inherits all the tuning of the existing repulsion.

11. **Sub-frame timing for anything faster than the tick.** (§3.1.) Pass the
    accumulator's fractional remainder as an age so rounds spawned in the same tick
    are spaced correctly. Also: issue tracers on a counter, not a random roll.

12. **Degrade several properties together.** (§3.2.) Barrel heat costs rate,
    dispersion *and* muzzle velocity, and the last feeds the range-dependent damage
    term, so overheating compounds instead of being a single slider.

13. **Make "what should I manoeuvre against" a virtual method.** (§5.1.)
    `GetEvasionPoint()` returns the missile for an active seeker and the
    *illuminating aircraft* for a semi-active one, so the evasion code notches the
    correct object with no per-type logic.

14. **Small composable trajectory modifiers.** (§5.4.) `TopAttack` and
    `JinkEvasion` are `[Serializable]` plain classes that take positions and return
    an offset. Any seeker opts in with one field and one `+=`, which is only
    possible because guidance is expressed as an aimpoint.

15. **Let the AI close a loop around emergent physics.** (§6.2.) The helicopter
    autopilot reads `GetVRSFactor()` and rotor RPM droop back out of the
    blade-element model and reacts with the real-world recovery technique. The AI
    is a *consumer* of the simulation's derived state, not a parallel model of it.

16. **Same accumulator, opposite asymmetry, different personality.** (§6.3.) The
    aeroplane's altitude accumulator climbs twice as fast as it descends; the
    helicopter's descends twice as fast as it climbs. One constant produces
    "keeps its energy" versus "hides behind terrain".

17. **Scale AI count inversely with player count, and reserve resources for
    humans.** (§9.2.) Two lines that balance a lopsided server without handicapping
    anyone and stop bots consuming the airframes players want.

18. **Amortise inside the job, not just on the main thread.** (§7.1.)
    `ShouldRunInputs(i, tickOffset)` round-robins AI evaluation across ticks from
    inside a Burst `IJobParallelFor`.

And one anti-pattern:

19. **Do not leave `Debug.Log` on a per-event path in a shipped build.** Both
    `ARHSeeker.ARHSeeker_OnChaff` and `SARHSeeker.SARHSeeker_OnChaff` log an
    interpolated string every time chaff is evaluated against a locked missile. It
    is a string allocation and a formatting pass, in the retail build, on an event
    that fires per chaff bundle per missile — and the `gc-max-time-slice=3` in
    `boot.config` says GC pressure was already a concern.

---

## 11. What is still not established

- **Nothing was run or profiled**, and **no authored values were recovered** —
  this build ships with script type trees stripped, so every ScriptableObject and
  prefab tuning value is unreadable. See [`nuclear_option.md`](nuclear_option.md)
  §19. Every number quoted in these three notes is a hardcoded literal in the C#.
- **`Turret.cs`, `WeaponStation.cs`, `WeaponManager.cs` and `AimSolver.cs`** were
  read only where they intersect the paths above. The turret traverse/elevation
  control loop specifically was not read.
- **`OpticalSeekerCruiseMissile` (359 lines)** — terrain-following cruise missile
  guidance — was not read, nor `BallisticMissileGuidance` (210 lines) beyond its
  role.
- **`AIPilotTakeoffState`, `AIPilotTaxiState`, `AIPilotShortLandingState` and the
  helicopter equivalents** were sampled, not read. The taxi state in particular
  (524 lines, including ground collision avoidance and runway queuing) is
  substantial and mostly unread.
- **`MissionManager`, `MissionRunner`, the objective graph and the node-graph
  scripting** were not read at all; §9 covers `FactionHQ` only.
- **`Airbase.cs` (1,600+ lines)**, rearming, repair and the reserve/inventory
  system were read only through the interfaces `FactionHQ` uses.
- **The strategic AI's target selection** (`TryGetStrategicTargets`,
  `AssessHQTargets`, the nuclear release logic) was read at the level of its
  scoring inputs, not its policy.
- **No developer account of any of this exists.** Every "because" is
  **[inferred]** — my reconstruction, not a reported reason.

---

## 12. Where things are

| System | Files |
|---|---|
| Shared world model | `TrackingInfo.cs`, `FactionHQ.cs` (`trackingDatabase`, `RpcUpdateTrackingInfo`, `IsTargetPositionAccurate`), `UnitRegistry.cs`, `PersistentID.cs` |
| Target scoring | `CombatAI.cs`, `OpportunityThreat.cs`, `ThreatItem.cs`, `TargetRequirements.cs`, `WeaponInfo.cs` (`CalcAttacksNeeded`, `CalcOpportunityThreat`) |
| Batteries | `FireControl.cs`, `WeaponStation.cs`, `Turret.cs`, `FiringCone.cs`, `FiringConeChecker.cs` |
| Guns | `Gun.cs`, `BulletSim.cs`, `WeaponManager.cs`, `Hardpoint.cs`, `WeaponMount.cs` |
| Seekers (this note) | `SARHSeeker.cs`, `ARMSeeker.cs`, `OpticalSeeker*.cs`, `LaserSeeker.cs`, `InertialSeekerShell.cs`, `TopAttack.cs`, `JinkEvasion.cs` |
| Helicopter AI | `AutopilotHelo.cs`, `AIHeloCombatState.cs`, `AIHeloLandingState.cs`, `AIHeloTakeoffState.cs`, `AIHeloTransportState.cs`, `CompoundHeloController.cs` |
| Ground vehicles | `GroundVehicle.cs`, `NuclearOption.Jobs/GroundVehicleJob_Math1.cs`, `GroundVehicleJob_Math2.cs`, `GroundVehicleFields.cs`, `GroundVehicleJobSettings.cs`, `ObstaclePosition.cs`, `SampleGroundResult.cs` |
| Navigation | `PathfindingAgent.cs`, `Pathfinding.cs`, `NodeGrid.cs`, `AStar/Node.cs`, `Heap.cs`, `RoadPathfinder.cs`, `RoadPathfinding/{Road,RoadNetwork,RoadNetworkSO,Node}.cs`, `SteeringInfo.cs`, `Obstacle.cs`, `VehicleFormation.cs`, `UnitCommand.cs` |
| Surface AI | `ShipAI.cs`, `LandingCraftAI.cs`, `AssaultCarrierAI.cs`, `MobileArtilleryAI.cs`, `RearmVehicleAI.cs` |
| Strategic layer | `FactionHQ.cs`, `Faction.cs`, `FactionRegistry.cs`, `MissionManager.cs`, `Airbase.cs`, `VehicleDepot.cs`, `Spawner.cs`, `MissionStatsTracker.cs`, `ExclusionZone.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option`,
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development) — the only first-party technical source.
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/).
- **No engineering talk, blog or paper was found.**
