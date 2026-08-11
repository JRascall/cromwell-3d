# Nuclear Option — damage, the data model, and the sortie loop

The sixth Nuclear Option note, closing the three systems I flagged as the last
things worth reading. Each fills a hole the other notes left open:

- **Damage's *input* side.** All five previous notes describe damage *arriving* at
  a part as `TakeDamage(pierce, blast, fire, impact)` and trace what it does. None
  of them says who computes those four numbers. §§1–4 do.
- **The data model.** Every scoring function, every AI gate and every weapon
  decision reads fields off a handful of ScriptableObjects. §5 is the schema, and
  it turns out to rest on a single dot product.
- **The airbase.** Everything in the AI procedures note calls into it and I
  documented the call sites without the implementation. §6 is the sortie loop.

Same source and caveats: read from the decompiled `Assembly-CSharp.dll` of the
retail Mono build. **No developer talk, blog or paper exists.** Tags: **[CODE]**
read from the assembly, **[PATCH]** the release notes, **[inferred]** my reading.
Nothing here comes from running the game.

The organising observation:

> **Everything expensive in this game is a scalar with a physically-derived
> exponent, and everything categorical is a dot product.** Blast radius is
> `yield^(1/3)`; overpressure falls as the inverse cube of *scaled* distance; shots
> required is `damageTolerance / pK`; armour is a subtractive threshold and a
> divisive tolerance. And "is this a target for me, and how much does it threaten
> me" — the question every AI in the game asks constantly — is a five-element
> vector dotted with a four-element one. **There is no target-type enum anywhere in
> this codebase**, and §5.2 is why that matters.

Related: [`broken_arrow_damage.md`](../broken_arrow/broken_arrow_damage.md) (**the A/B** — the same
problem divided almost exactly the opposite way),
[`nuclear_option.md`](nuclear_option.md) (what damage *does* once it arrives), [`nuclear_option_combat.md`](nuclear_option_combat.md),
[`nuclear_option_command.md`](nuclear_option_command.md) (the scoring functions
that read §5's schema), [`nuclear_option_control.md`](nuclear_option_control.md)
(the AI procedures that drive §6),
[`nuclear_option_audio.md`](nuclear_option_audio.md) (§3's propagation constant is
shared with the audio delay).

---

## 1. Four damage channels and a two-knob armour model

**[CODE]** `ArmorProperties` is 28 lines and is the whole armour system:

```csharp
public float pierceArmor, blastArmor, fireArmor;              // subtractive thresholds
public float pierceTolerance = 1f, blastTolerance = 1f, fireTolerance = 1f;   // divisive scales
public float overpressureLimit = 5f;

public float CalcNetDamage(float pierce, float blast, float fire, float impact) {
    return Mathf.Max(pierce - pierceArmor, 0f) / pierceTolerance
         + Mathf.Max(blast  - blastArmor,  0f) / blastTolerance
         + Mathf.Max(fire   - fireArmor,   0f) / fireTolerance
         + impact;                                             // impact ignores armour entirely
}
```

**[inferred] Two knobs per channel, and they mean different things.** *Armour* is a
threshold — damage below it does nothing at all, which is what produces the
"bounces off" behaviour and makes a 20 mm cannon useless against a tank no matter
how many rounds you land. *Tolerance* is a divisor — it scales what does get
through, which is how you express "this thing is tough" separately from "this
thing is armoured". A lightly-armoured but structurally robust airliner has low
`pierceArmor` and high `pierceTolerance`; a tank has the reverse.

**Impact damage bypasses armour completely.** Flying into the ground, being
detached at speed, or a rotor blade strike all arrive as `impact` and are added
raw. **[inferred]** That is correct and deliberate — armour is a defence against
*weapons*, not against the planet.

`overpressureLimit` is a separate threshold used by the shockwave path (§3), so a
part can be immune to blast damage but still be torn off by the pressure wave.

Every `IDamageable` implements the same interface:

```csharp
void TakeDamage(float pierce, float blast, float amountAffected, float fire, float impact, PersistentID dealer);
void ApplyDamage(float netPierce, float netBlast, float netFire, float netImpact);
void TakeShockwave(Vector3 origin, float overpressure, float blastPower);
ArmorProperties GetArmorProperties();
```

**[inferred] The `TakeDamage` / `ApplyDamage` split is the networking seam.**
`TakeDamage` runs on the server, applies armour, and issues
`aircraft.RpcDamage(index, new DamageInfo(...))`; `ApplyDamage` runs on every
client with the *already-reduced* numbers. So armour is resolved once,
authoritatively, and the visual and physical consequences are replicated — which
is why the wing effectiveness, joint break forces and compartment flooding in
[`nuclear_option.md`](nuclear_option.md) §5.1 and §11.4 are consistent across
clients even though the flight model is not server-authoritative.

`amountAffected` is a fifth parameter that only the blast path uses — the fraction
of the part exposed to the wave (§3.2).

---

## 2. Kinetic damage: energy, not hits

**[CODE]** From `BulletSim.Bullet.TrajectoryTrace`:

```csharp
float pierce = info.pierceDamage * velocity.sqrMagnitude / (info.muzzleVelocity * info.muzzleVelocity);
```

**Damage is proportional to kinetic energy at impact**, so range degrades
penetration automatically through the drag model, and a round fired from a
diving aircraft hits harder than one from a climbing aircraft because the
platform's velocity was added at the muzzle. No damage-falloff curve exists
anywhere.

`ImpactDetector` handles the other kind of kinetic damage — things hitting the
ground:

```csharp
accel = (rb.velocity - velocityPrev) / Time.fixedDeltaTime;
if (accel.sqrMagnitude > gLimit * gLimit * 82.81f)                 // 82.81 = 9.1², so gLimit is in g
    damageable.TakeDamage(0f, 0f, 1f, 0f, accel.magnitude, PersistentID.None);
```

attached to spawned debris and detached parts, self-destructing after five seconds
at rest. **[inferred] A g-threshold on the frame-to-frame velocity change** — the
same quantity the server uses to validate aircraft movement
([`nuclear_option.md`](nuclear_option.md) §13) and the same one that kills an
ejected pilot ([`nuclear_option_control.md`](nuclear_option_control.md) §7.3).
Three different systems, one measure of "that was too violent".

`AeroPart.OnCollisionEnter` computes its own version from the collision impulse:

```csharp
float damage = (collision.impulse.magnitude / Time.fixedDeltaTime / (rb.mass * 9.81f)
                - impactDamage.threshold) * impactDamage.multiplier;
```

— impulse converted to g, minus a per-part threshold, times a multiplier.
`ImpactDamage` is a two-field `[Serializable]` class, which is the entire
authoring surface for "how fragile is this part in a crash".

---

## 3. Blast

### 3.1 The wave is a real expanding sphere

**[CODE]** `Shockwave.Start`:

```csharp
blastPower       = Mathf.Pow(yieldKilotons * 1000000f, 0.3333f);   // cube root of yield in kg TNT
blastRadius      = blastPower * 13f;
blastPropagation = blastPower * 0.5f;                              // starts at the fireball
```

and `Update`:

```csharp
blastPropagation += 340f * Time.deltaTime;
float scaled      = Mathf.Max(blastPropagation / blastPower, 1f);   // Hopkinson-Cranz scaled distance
float overpressure = 25000f / (scaled * scaled * scaled);           // ← 1/Z³
if (overpressure <= 0.5f) { influencedObjects.Clear(); return; }     // done
```

**[inferred] `blastPropagation / blastPower` is scaled distance** — real distance
divided by the cube root of yield, which is the standard Hopkinson-Cranz scaling
law under which all explosions of all sizes have the same overpressure-versus-
scaled-distance curve. Overpressure then falls as its inverse cube, which is the
right near-field shape. **So one yield number produces a physically-scaling blast
of any size**, from a 20 mm HE shell to a strategic warhead, with no per-weapon
curve — and the same `yield^(1/3)` appears in the exclusion radius
([`nuclear_option_command.md`](nuclear_option_command.md) §2), the audio
propagation start ([`nuclear_option_audio.md`](nuclear_option_audio.md) §4) and
the mushroom cloud.

The **340 m/s propagation is shared with the audio system**, so the sound of a
blast and its physical effect arrive together by construction.

### 3.2 Per-object exposure and the impulse cap

**[CODE]** Candidates are gathered **once**, at `Start`, with a single
`OverlapSphereNonAlloc` at twice the blast radius into a static 4,096-collider
buffer. Each becomes an `InfluencedObject` caching its collider, rigidbody,
`IDamageable` and an `averageRadius` from its bounds. Then, per frame, each is
tested against the growing wavefront and removed once hit:

```csharp
blastPropagation += averageRadius;                                   // the wave reaches its near face
if (SqrMagnitude(bounds.center - origin) > blastPropagation²) return false;

float area     = MathF.PI * averageRadius * averageRadius;
float exposure = Mathf.Clamp(Mathf.Max(blastPower², blastPropagation²) / area, 0f, 10f);
exposure /= 1f + armorProperties.blastArmor * 0.02f;

damageable.TakeDamage(0f, overpressure, Mathf.Clamp01(exposure), 0f, 0f, ownerID);

float impulse = area * Mathf.Clamp01(exposure) * overpressure * 25f;
impulse = Mathf.Min(impulse, blastYield * 200f, 60f * mass);          // ← three caps
rb.AddForceAtPosition((bounds.center - origin).normalized * impulse, bounds.center, ForceMode.Impulse);
```

**[inferred] Four things.**

**The gather happens once and the damage arrives progressively.** So a bomb in a
vehicle column destroys the nearest vehicles first and the far ones a beat later,
which is both correct and free — the alternative (an overlap query per frame as
the radius grows) would be enormously more expensive.

**Each object's own radius advances the wavefront.** A carrier is hit when the
wave reaches its hull, not its centre.

**`exposure` is an area-normalised term clamped to 10 then to 1**, and blast
armour divides it rather than subtracting — so armour reduces *how much of you the
blast couples to*, distinct from `blastArmor`'s subtractive role inside
`CalcNetDamage`. The same field does two different jobs on two different paths,
which is a small wart.

**The impulse has three independent caps**: proportional to yield, and never more
than `60 · mass` (i.e. **no object ever gets more than 60 m/s of delta-v from a
blast**, since `ForceMode.Impulse` divides by mass). **[inferred] That last cap is
a stability rail** — without it a small-mass part near a large detonation acquires
a velocity that breaks the solver and the network validator in the same frame.

Buildings additionally get `RegisterRecentExplosion(origin, yield)` so they can
accumulate structural damage across multiple hits.

### 3.3 Blast shadowing, in fifteen lines

**[CODE]** `Explosion.SimulateForce` is a *separate*, simpler path used for
non-shockwave detonations:

```csharp
float radius = Mathf.Pow(yield, 0.3333f) * 20f;
int n = Physics.OverlapSphereNonAlloc(position, radius, colliderBuffer);
// build a dictionary of collider → IDamageable for everything in range
foreach (var (collider, damageable) in candidates) {
    if (!Physics.Linecast(position, collider.transform.position, out hit, ~ExclusionZonesMask)) continue;
    if (candidates.ContainsKey(hit.collider) && !alreadyHit.Contains(hit.collider))
        candidates[hit.collider].TakeShockwave(position, blastPower, blastPower);   // ← the FIRST thing hit
    alreadyHit.Add(hit.collider);
}
```

**[inferred] The trick is that it damages `hit.collider`, not the candidate it was
casting at.** A ray toward each candidate returns whatever it hits *first*, and
only that object takes the blast. So an object standing behind another is shielded
by it, automatically, and the `alreadyHit` list stops the front object being
damaged once per candidate behind it. **Blast shadowing for one linecast per
candidate and no line-of-sight bookkeeping** — a crude model (it shadows by
collider centre, so partial cover is all-or-nothing) but a real one, and it costs
nothing beyond the casts.

`TakeShockwave` on an `AeroPart` then does:

```csharp
float size  = (collisionSize.x + collisionSize.y + collisionSize.z) * 0.3333f;
float force = Mathf.Min(wingArea + dragArea + size*size, blastPower * blastPower) * overpressure;
force = Mathf.Min(force, 60f * rb.mass);                                  // the same 60 m/s cap
Vector3 dir = Vector3.Lerp(NormalizedDirection(origin, position),
                           Random.insideUnitSphere.normalized, overpressure * 0.001f);
rb.AddForceAtPosition(dir * force, liftNormal.position, ForceMode.Impulse);
```

**[inferred] The coupling area is the part's *aerodynamic* area** — `wingArea +
dragArea` — which is exactly right, because the thing that catches a blast wave is
the same thing that catches airflow, and it means no separate blast-area field
needs authoring. And the direction is **randomised in proportion to overpressure**,
so a near-miss throws parts in a scattered pattern rather than radially, which is
what wreckage actually does.

`SwashRotor.TakeShockwave` uses `overpressureLimit` as a hard gate — blades survive
below it and are damaged above.

---

## 4. Hit validation: the last piece of the authority model

**[CODE]** `HitValidator` is the answer to a question the networking note raised
and did not close: **if bullets are simulated on the client
([`nuclear_option.md`](nuclear_option.md) §13), what stops a client claiming
arbitrary hits?**

```csharp
// The server logs each shooter's firing ray, at most one per 0.1 s, keeping 5 seconds
public static void LogFiring(PersistentID shooter, Vector3 position, Vector3 velocity);

// and validates a claimed hit against them:
public bool HitPlausible(Vector3 hitPosition, Vector3 hitVelocity) {
    float dist = Vector3.Distance(hitPosition, ray.origin);
    if (Vector3.Angle(hitPosition - ray.origin, ray.direction) * Mathf.Clamp01(dist * 0.01f) < 10f)
        return dist < 3000f;
    return false;
}
```

**[inferred] The angular tolerance is scaled by range, and that is the clever
part.** The test is `angle × clamp01(dist/100) < 10°`. Beyond 100 m the hit must
be within 10° of some ray the shooter fired in the last five seconds. Inside 100 m
the tolerance opens up linearly — at 10 m, any angle under 100° passes. That is
correct, because at close range the shooter's own motion between the 0.1-second
log snapshot and the impact dominates the geometry, and a strict cone would reject
legitimate point-blank hits. A hard 3 km cap bounds the whole thing.

**[inferred] This is the same philosophy as the aircraft snapshot validator:
plausibility, not reproduction.** The server never simulates the bullet. It keeps a
cheap rolling record of where each shooter was pointing and asks whether the
claimed hit is consistent with any of it. Five seconds of history at 10 Hz is
fifty rays per active shooter — trivial — and it makes the obvious cheat (claim
hits on anything) fail while leaving the legitimate case untouched. Failures are
logged with the owning player's name rather than silently dropped, which is the
right call for something that will occasionally false-positive.

---

## 5. The data model

### 5.1 `UnitDefinition` — one ScriptableObject per unit type

**[CODE]** 140 lines, and it is the complete description of a thing that can exist
in the world:

| Group | Fields |
|---|---|
| Identity | `typeIdentity`, `roleIdentity`, `jsonKey`, `unitName`, `bogeyName`, `description`, `code` |
| Sensing | `visibleRange`, `iconRange`, **`radarSize`** (the RCS from the sensor model) |
| Presentation | `friendlyIcon`, `hostileIcon`, `mapIcon`, `mapOrient`, `iconSize`, `mapIconSize` |
| Objectives | `captureCapacity`, `captureStrength`, `captureDefense` |
| Physical | `length`, `width`, `height`, `mass`, `IsObstacle` |
| Combat | `value`, `manpower`, **`armorTier`**, **`damageTolerance`** |
| Spawning | `unitPrefab`, `spawnOffset`, `CanSlingLoad` |
| Lifecycle | `disabled`, `isEventContent`, `dontAutomaticallyAddToEncyclopedia` |
| Editor | `minEditorHeight`, `maxEditorHeight` |

**[inferred] Three notes.** `jsonKey` carries a comment — *"dont change jsonKey
after it is set"* — because it is the mission-file identity; the setter even
throws outside the editor. `mass` is not authored but cached from the prefab
(`CacheMass()` reads `unitPrefab.GetComponent<Unit>().GetPrefabMass()`), so the
data and the physics cannot disagree. And `isEventContent` gates April Fools units
behind a flag checked at every enumeration site — a shipped mechanism for content
that exists but is normally invisible.

`AircraftDefinition : UnitDefinition` adds only three fields —
`aircraftParameters` (the flight tuning), `aircraftInfo` (five display numbers:
`emptyWeight`, `maxSpeed`, `stallSpeed`, `maneuverability`, `maxWeight`) and
`restRotation`. **[inferred] The subclass is thin because everything an aircraft
*is* lives on the prefab as components**, which is the §2 lesson from
[`nuclear_option.md`](nuclear_option.md) applied to the data layer too.

### 5.2 The targeting matrix is one dot product

**[CODE]** This is the piece worth the whole section.

```csharp
public struct TypeIdentity {        // WHAT I AM — blended, 0..1 each
    public float surface, air, missile, radar, strategic;
    public float ThreatPosedBy(RoleIdentity threat) =>
          surface * threat.antiSurface
        + air     * threat.antiAir
        + missile * threat.antiMissile
        + radar   * threat.antiRadar;
}

public struct RoleIdentity {        // WHAT I KILL — blended, 0..1 each
    public float antiSurface, antiAir, antiMissile, antiRadar;
    public float OpportunityAgainst(TypeIdentity target) =>
          target.surface * antiSurface + target.air * antiAir
        + target.missile * antiMissile + target.radar * antiRadar;
}
```

**[inferred] A five-vector dotted with a four-vector, read in both directions, and
that is the entire targeting taxonomy of the game.** There is no target-type enum,
no `IsAircraft()`, no per-weapon-per-type effectiveness table.

Why it matters:

- **Units are blends, not categories.** A SAM vehicle has `surface = 1` *and*
  `radar = 1`, so it is simultaneously a ground target for a bomb and a radar
  target for an anti-radiation missile, with no special case. A radar-equipped
  warship is `surface = 1, radar = 0.6`. A cruise missile is
  `missile = 1, surface = 0.2`.
- **Weapons are blends too.** A multirole missile might be `antiAir = 1,
  antiSurface = 0.3`, and the dot product automatically makes it a good but not
  ideal choice against a truck.
- **The same numbers answer both questions.** `ThreatPosedBy` asks "how dangerous
  is that weapon to me"; `OpportunityAgainst` asks "how good am I against that
  target". They are the same dot product read from the two ends, which is why
  `CombatAI.AnalyzeTarget` can return an `OpportunityThreat` *pair* from a single
  evaluation ([`nuclear_option_command.md`](nuclear_option_command.md) §2).
- **The alternative scales terribly.** An enum plus a lookup table is 4 × 5 = 20
  entries per weapon, authored by hand, and every new unit type is a schema change
  and a pass over every weapon. Here a new unit type is five floats.
- **`strategic` is deliberately not in the dot product.** It appears only where
  strategic value is the question — nuclear target selection multiplies opportunity
  by it, and `TrackingInfo.GetStrategicPriority` divides it by the attacker count.
  It is a fifth dimension of *what a thing is* that no weapon has a matching
  *anti-* term for, which is exactly right: nothing is "anti-strategic", strategic
  is a reason to pick a target rather than a capability.

### 5.3 `WeaponInfo` and `TargetRequirements`

**[CODE]** `WeaponInfo` carries the ballistics (`muzzleVelocity`, `maxSpeed`,
`dragCoef`, `gravMult`), the damage (`pierceDamage`, `blastDamage`,
`armorTierEffectiveness`, `airburstHeight`), the economics (`costPerRound`,
`massPerRound`, `value`), the signature (`visibilityWhenFired`), and — the one
number the whole AI rests on:

```csharp
public float pK;
public float CalcAttacksNeeded(Unit target)
    => Mathf.Max(target.definition.damageTolerance, 0.1f) / Mathf.Max(pK, 0.01f);
```

**[inferred] Shots required is one division of two authored floats.** It feeds the
salvo planner, the reservation counters, the AI's "is this target already
saturated" check and the fire-control battery's ammunition allocation. Every
multi-shooter coordination decision in the game reduces to this ratio.

Alongside that sit **twenty boolean capability flags** — `gun`, `missile`, `bomb`,
`glideBomb`, `laserGuided`, `boresight`, `nuclear`, `strategic`, `energy`,
`jammer`, `troops`, `cargo`, `sling`, `rearmGround`, `rearmShip`, `overHorizon`,
`useWeaponDoors`, `hideInDisplay`… **[inferred]** Blunt, and the honest read is
that it is a capability bitset spelled out longhand. It works because the flags are
mostly consumed by `if` statements in the AI's mode selection rather than combined,
but it is the one part of the schema that would not scale — and it sits directly
next to `RoleIdentity`, which is the elegant solution to the same class of problem.

`TargetRequirements` is eleven floats and is the hard-rejection gate from
[`nuclear_option_command.md`](nuclear_option_command.md) §2: `lineOfSight`,
`minAltitude`, `maxAltitude`, `minRange`, `maxRange`, `maxSpeed`, `minIR`,
`minRadar`, `minAlignment`, `minOwnerSpeed`, `minValue`. **[inferred] Every one of
those is a *veto*, not a score** — which is why the scoring cascade can be written
as a sequence of early returns followed by multiplications, and why adding a new
constraint means adding a field and one `if` rather than rebalancing a weighted
sum.

---

## 6. The airbase and the sortie loop

**[CODE]** `Airbase` is 2,057 lines built from four nested types: `Runway`,
`VerticalLandingPoint`, `Hangar` (separate file) and `RunwayUsage`.

### 6.1 Runways are directionless until asked

```csharp
public struct RunwayUsage { public Runway Runway; public bool Reverse; }

public Runway.RunwayUsage? GetTakeoffRunway(Aircraft aircraft, float takeoffDist) {
    // nearest runway that is long enough
    foreach (Runway r in runways)
        if (r.Takeoff && r.Length >= takeoffDist) {
            var d = r.GetDistance(aircraft.transform);        // returns distance AND which end
            if (d.Distance < best) { best = d.Distance; chosen = r; reverse = d.Reverse; }
        }
}

public Runway.RunwayUsage? RequestLanding(Aircraft aircraft, RunwayQuery query) {
    // runway requiring the SMALLEST heading change
    foreach (Runway r in runways)
        if (r.Landing && r.IsSuitable(query)) {
            var a = r.GetAngle(aircraft.transform);           // returns angle AND which end
            if (a.Angle < best) { best = a.Angle; chosen = r; reverse = a.Reverse; }
        }
}
```

**[inferred] A runway has no active direction; the direction is chosen per
aircraft, per request.** Departures pick the nearest runway long enough for them;
arrivals pick the one requiring the least turning. Both return which *end* to use.
That is much simpler than modelling an active runway assignment, it handles wind
implicitly (an aircraft on a downwind approach naturally selects the other end),
and it means two aircraft can legitimately use opposite ends of the same runway —
which is why §6.2's occupancy model has to be explicit.

`RunwayQuery` is `{ RunwayQueryType flags, MinSize, LandingSpeed, TailHook }` — so a
carrier-capable aircraft can filter for arrested-landing surfaces and a heavy can
filter for length, through one struct.

### 6.2 Occupancy: a queue, a list, and crossings computed from geometry

```csharp
public bool IsAvailableForTakeoff(Aircraft querier) {
    if (landingList.Count > 0) return false;                        // ← landings always win
    if (takeoffQueue.TryPeek(out var first)) {
        if (first == null || first.disabled) takeoffQueue.Dequeue(); // self-cleaning
        return first == querier;                                     // strict FIFO
    }
    if (OtherAircraftUsingRunway(querier)) return false;
    foreach (Runway crossing in crossingRunways)
        if (crossing.OtherAircraftUsingRunway(querier)) return false;
    return true;
}
```

**[inferred] Four decisions.** Landings unconditionally block departures, because
an aircraft on final has no option and one holding short does. The takeoff queue is
strict FIFO with **self-cleaning of dead entries at the head** — a destroyed
aircraft at the front cannot deadlock the airfield. `crossingRunways` is computed
from geometry at setup (`FindCrossingRunways` / `CrossesRunway`) rather than
authored, so an intersecting layout is safe without anyone marking it up. And
`ClearForTakeoff` recurses into crossing runways **exactly one level**
(`checkCrossing: false`), which bounds the recursion on a mutually-crossing
layout — a small, deliberate correctness detail.

`AllowSimultaneousTakeoff` changes when the queue advances: normally at
`RegisterTakeoffLeftRunway` (the previous aircraft is airborne), or at
`RegisterStartTakeoff` (the previous aircraft has begun its roll) if the airfield
permits it. One bool, two very different airfield throughputs.

The geometric predicates that support this are all short and worth listing, because
between them they are the whole of "where is this aircraft relative to my runway":
`AircraftOnRunway` (within half a runway width plus the aircraft's radius of the
nearest centreline point), `AircraftOnApproach` (near an end, pointing at the
midpoint with `Dot > 0.8`), `AircraftApproachingRunway` (the nearest centreline
point, extended, is ahead and within range), `AircraftDistanceFromRunway`
(`float.MaxValue` if past either end — i.e. not abeam).

### 6.3 The glideslope is 6% and it leads a moving deck

```csharp
public float GetGlideslopeError(Aircraft aircraft, float timeToTouchdown) {
    Vector3 td = GetTouchdownPoint().ToLocalPosition();
    Vector3 horizontal = new Vector3(aircraft.position.x - td.x, 0f, aircraft.position.z - td.z);
    float targetY = td.y + aircraft.definition.spawnOffset.y + 0.06f * horizontal.magnitude;
    return aircraft.transform.position.y - targetY;
}
```

**A hardcoded 0.06 gradient — a 3.4° glideslope**, near enough the real 3° ILS
standard, plus the aircraft's own ground clearance so the error is zero at wheel
contact rather than at the fuselage datum. And every approach geometry function
takes `timeToTouchdown` and offsets by `Runway.GetVelocity() * timeToTouchdown` when
the runway has a rigidbody — **so the same code lands an aircraft on a carrier
under way by leading the deck**, with the lead shrinking to zero as the aircraft
arrives.

`VerticalLandingPoint` is the VTOL equivalent: a reservation with a landing queue,
an `IsAvailable()` check, an approach point, its own `GetVelocity()` for moving
ships, and `FindCrossingRunways` so a pad on a runway does not conflict with fixed
wing traffic.

### 6.4 Hangars, priority and delayed spawning

```csharp
public TrySpawnResult TrySpawnAircraft(Player player, AircraftDefinition def, LiveryKey livery,
                                       Loadout loadout, float fuelLevel) {
    foreach (Hangar h in hangars)                                    // priority-sorted
        { var r = h.TrySpawnAircraft(player, def, livery, loadout, fuelLevel); if (r.Allowed) return r; }
    return default;                                                  // Allowed = false
}
private void SortHangarsByPriority() => hangars.Sort((a, b) => b.GetPriority().CompareTo(a.GetPriority()));
```

with `TrySpawnResult { bool Allowed; Hangar Hangar; bool delayedSpawn; }`.

**[inferred] Spawn permission is delegated to the individual hangar, sorted by
priority, first one that accepts wins.** So a base's parking capacity, its
type restrictions and its occupancy are the hangar's business, and the airbase is
just a dispatcher. `delayedSpawn` lets a hangar accept an aircraft it cannot
produce immediately — the doors have to open, or a slot has to clear — which is how
`FactionHQ.DeployAIAircraft` can commit supply without blocking.

### 6.5 The loop, end to end

Putting the six notes together, one AI sortie is:

```
FactionHQ.DeployUnits (periodic)
  └ SortAirbases / SortDepots every 30 s
  └ DeployAIAircraft: limit = AIAircraftLimit + enemies·k − friendlies·k
      └ shuffle the encyclopedia, take the first type whose supply exceeds the
        human reserve floor
      └ Airbase.TrySpawnAircraft → first hangar by priority that accepts
Pilot: PilotParkedState → AIPilotTaxiState
  └ Airbase.GetTaxiNetwork → PathfindingAgent → SteeringInfo (speed for corners)
  └ yielding, hold-short, WaitingForTakeoffClearance → Runway.QueueTakeoff
AIPilotTakeoffState
  └ Runway.IsAvailableForTakeoff (landings win, FIFO, crossings clear)
  └ RegisterStartTakeoff → rotate on TRUE airspeed → RegisterTakeoffLeftRunway
AIPilotCombatModes
  └ FactionHQ.trackingDatabase → CombatAI.AnalyzeTarget → attack mode
  └ reserve via TrackingInfo.attackers / missileAttacks
  └ AutopilotPlane.AutoAim (turn-rate limit) → ControlsFilter → ControlInputs
AIPilotLandingState
  └ Airbase.RequestLanding (smallest heading change) → RunwayUsage
  └ 6% glideslope, √(W/S) approach speed, clamped integrator, deck lead
  └ Runway.RegisterLanding → TryGetExitTaxiPoint → taxi to a hangar
Rearmer / Repairer, registered with FactionHQ.RearmMissionController
  └ back to the hangar, supply decremented, ready for the next sortie
```

**[inferred] Every arrow in that chain is a call I have now read**, and the notable
thing is how few of them are bespoke. The taxi network is the same `RoadNetwork`
ground vehicles drive on. The steering is the same `SteeringInfo` struct. The
target scoring is the same `CombatAI` a SAM battery uses. The reservation is the
same two bytes. **The sortie loop is mostly existing systems wired together, and
the only genuinely airbase-specific code is runway occupancy and the glideslope.**

---

## 7. What is worth taking

1. **Two knobs per damage channel: a subtractive threshold and a divisive
   tolerance.** (§1.) "What bounces off" and "how much what gets through hurts" are
   different properties and conflating them is why so many damage models feel
   wrong.

2. **Let impact damage bypass armour.** (§1.) Armour defends against weapons, not
   against terrain.

3. **Split `TakeDamage` (server, applies armour) from `ApplyDamage` (everyone,
   already reduced).** (§1.) Armour resolves once, authoritatively; consequences
   replicate. It is the seam that lets a client-authoritative physics game have a
   server-authoritative damage model.

4. **Damage from kinetic energy, not from a falloff curve.** (§2.)
   `pierce · v²/v₀²` makes range, dive angle and platform speed all matter for free.

5. **One measure of "that was too violent", used everywhere.** (§2.) The
   frame-to-frame velocity delta kills debris, kills ejected pilots, damages
   crashing parts and validates network movement.

6. **Scale a blast by `yield^(1/3)` and decay overpressure as `1/Z³` in scaled
   distance.** (§3.1.) Hopkinson-Cranz means one yield number produces a correct
   blast of any size, with no per-weapon curve, and the same cube root is reusable
   for exclusion radii, audio and visuals.

7. **Gather blast candidates once, then damage them as the wavefront passes.**
   (§3.2.) Progressive destruction outward, at a fraction of the cost of re-querying.

8. **Cap blast impulse at a fixed delta-v.** (§3.2.) `min(…, 60f * mass)`. A
   stability rail that prevents a small part near a big bomb from breaking the
   solver and the network validator in the same frame.

9. **Blast shadowing by casting at each candidate and damaging whatever you hit
   first.** (§3.3.) Fifteen lines, one linecast per candidate, no visibility
   bookkeeping.

10. **Use the aerodynamic area as the blast coupling area.** (§3.3.) The thing
    that catches airflow is the thing that catches a pressure wave; no new field.

11. **Validate claimed hits against a rolling log of firing rays, with an angular
    tolerance that opens up at close range.** (§4.) Plausibility, not reproduction —
    the same philosophy as the movement validator, and it is fifty rays per shooter.

12. **Replace target-type enums with two small vectors and a dot product.** (§5.2.)
    Units and weapons both become blends; the same numbers answer "how good am I
    against it" and "how dangerous is it to me"; a new unit type is five floats
    instead of a schema change and a pass over every weapon. **This is the single
    most transferable idea in the note.**

13. **Keep the fifth dimension out of the dot product.** (§5.2.) `strategic` has no
    matching *anti-* term because nothing is anti-strategic — it is a reason to pick
    a target, not a capability. Recognising which axes pair and which do not is what
    keeps the matrix small.

14. **Make requirements vetoes, not weights.** (§5.3.) Eleven floats, each an early
    return. Adding a constraint is a field and an `if`, not a rebalancing exercise.

15. **Derive data from the prefab rather than authoring it twice.** (§5.1.)
    `CacheMass()` reads the mass off the prefab's components, so the data model and
    the physics cannot disagree.

16. **Let the direction of a runway be chosen per request, not assigned.** (§6.1.)
    Departures take the nearest long-enough one, arrivals the least-turning one, and
    both return which end. Wind and traffic are handled implicitly.

17. **Compute conflicts from geometry, and bound the recursion.** (§6.2.) Crossing
    runways are found by intersection test at setup, and the clearance check recurses
    exactly one level.

18. **Self-clean queue heads.** (§6.2.) A destroyed aircraft at the front of the
    takeoff queue must not be able to deadlock an airfield, and the fix is two lines
    at the peek site.

19. **Lead a moving landing surface by `velocity × timeToTouchdown`.** (§6.3.) One
    term added to every approach geometry function turns a runway into a carrier
    deck.

20. **Delegate spawn permission to the sub-location and sort by priority.** (§6.4.)
    The airbase dispatches; the hangar decides. `delayedSpawn` lets a commitment be
    accepted before it can be fulfilled.

---

## 8. What is not established

- **Nothing was run or profiled**, and **no authored value was recovered** — the
  build ships with script type trees stripped
  ([`nuclear_option.md`](nuclear_option.md) §19). Every number here is a hardcoded
  literal in the C#. In particular I can describe `TypeIdentity` and `RoleIdentity`
  but not a single unit's actual five floats.
- **`Hangar.cs` was read only through the interface `Airbase` uses.** Parking
  capacity, door sequencing, type restrictions and the delayed-spawn path are
  unread.
- **`Rearmer` (242 lines), `Repairer`, `RearmMissionController` and
  `RearmVehicleAI`** were read at the registration level only — the actual rearm
  and repair rates, and how a rearm mission is assigned and driven, are unread.
- **`FragmentManager` (252 lines), `DebrisManager`, `WarheadStorage` (213) and
  `SubmunitionDispenser` (186)** were not read. Cluster munition dispensing and
  cook-off in particular are likely to be interesting.
- **`Building.cs` and `Capture.cs`** — structural damage accumulation
  (`RegisterRecentExplosion`) and the capture mechanic — were not read.
- **`BlastManager`** (the global `_Global_BlastMap` texture the terrain and water
  shaders read) was seen only as a call site.
- **`Encyclopedia.cs` and the loadout system** (`Loadout`, `StandardLoadout`,
  `LoadoutSelector`, `Hardpoint`, `WeaponStation`'s 553 lines) were read only where
  the AI touches them.
- **No developer account exists.** Every "because" is **[inferred]**.

---

## 9. Where things are

| System | Files |
|---|---|
| Damage core | `ArmorProperties.cs`, `IDamageable.cs`, `DamageInfo.cs`, `DamageablePart.cs`, `UnitPart.cs`, `PartDamageTracker.cs`, `ImpactDamage.cs`, `ImpactDetector.cs`, `OnReportDamage.cs`, `IReportDamage.cs` |
| Blast | `Shockwave.cs`, `Explosion.cs`, `NuclearOption.Effects/BlastManager.cs`, `MushroomCloud.cs`, `WarheadStorage.cs`, `SubmunitionDispenser.cs` |
| Debris | `FragmentManager.cs`, `DebrisManager.cs`, `Wreckage.cs`, `WreckCollector.cs`, `DamageEffect.cs`, `DamageParticles.cs`, `DamageMaterial.cs` |
| Validation | `HitValidator.cs`, `BulletSim.cs` |
| Data model | `UnitDefinition.cs`, `AircraftDefinition.cs`, `ShipDefinition.cs`, `VehicleDefinition.cs`, `BuildingDefinition.cs`, `MissileDefinition.cs`, `SceneryDefinition.cs`, `TypeIdentity.cs`, `RoleIdentity.cs`, `WeaponInfo.cs`, `TargetRequirements.cs`, `AircraftInfo.cs`, `AircraftParameters.cs`, `Encyclopedia.cs`, `DefinitionWriters.cs`, `INetworkDefinition.cs`, `IHasJsonKey.cs` |
| Loadouts | `StandardLoadout.cs`, `LoadoutSelector.cs`, `Hardpoint.cs`, `HardpointSet.cs`, `WeaponStation.cs`, `WeaponManager.cs`, `WeaponMount.cs`, `MountedMissile.cs`, `MountedCargo.cs` |
| Airbase | `Airbase.cs`, `Hangar.cs`, `HangarDoor.cs`, `HangarLighting.cs`, `RunwayQuery.cs`, `RunwayQueryType.cs`, `RunwayType.cs`, `ArrestorGear.cs`, `TailHook.cs`, `OpticalLandingSystem.cs`, `AirbaseOverlay.cs`, `AirbaseMapIcon.cs` |
| Sortie support | `Rearmer.cs`, `Repairer.cs`, `RearmMissionController.cs`, `RearmVehicleAI.cs`, `Refueler.cs`, `VehicleDepot.cs`, `UnitStorage.cs`, `ReserveReport.cs`, `NuclearOption/OwnedAirframe.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option`,
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development).
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/).
- **No engineering talk, blog or paper was found.**
