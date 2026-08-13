# Kerbal Space Program 1 — reference notes

How a two-person team in 2011 built a solar system you can fly a
thirty-part rocket around — and why almost every famous problem the game
has is the **same** problem wearing a different hat.

The short version, and the reason this note is worth reading rather than
just admiring: **KSP contains two complete simulations of the same universe, and
its entire architecture is the seam between them.** One is analytic — Keplerian
orbits in double precision, position as a closed-form function of time, evaluable
at any instant for free. The other is numeric — Unity's PhysX, single precision,
iterative, 50 Hz, and it only knows how to take a small step forward. Time warp,
the floating-point Kraken, Krakensbane, ScaledSpace, patched conics, physics
easing and the wobbling rockets are not seven systems. They are **seven places
where those two universes have to agree**, and the game's whole engineering
history is the story of making the handovers survive contact with players.

> **Read alongside:**
> [`kerbal_space_program_flight.md`](kerbal_space_program_flight.md) — **the
> implementation half of this note.** This one is architecture: why KSP is
> shaped the way it is. That one is the path from a key press to a force —
> `FlightCtrlState`, SAS, engines and gimbals, the control-surface geometry
> test, lifting surfaces, RCS, the atmosphere as a physical model and as an
> O'Neil scattering shell, and a build order. Read this first, build from that.
> [`elite_dangerous.md`](elite_dangerous.md) — the other
> quadtree-cube planet in this folder, and the sharpest available contrast.
> Frontier and Squad independently reached the *identical* terrain topology
> (§7.7), then split on where to evaluate it, for a reason that is entirely
> about queries rather than pixels.
> [`space_engineers.md`](space_engineers.md) — the other "big world, small
> floats" game. SE **partitions** space into physics clusters; KSP **rebases** a
> single origin. §5.4 argues that difference is forced by how many things each
> game actually simulates, not by taste.
> [`map_scale.md`](../../topics/scale/map_scale.md) for the family of extent
> problems this belongs to; [`lod_systems.md`](../../topics/world/lod_systems.md)
> for the ladder §7 climbs.

---

## 0. Sourcing, and the caveat that governs everything below

**KSP is closed-source, ships under IL2CPP-free Mono but with no published
engine documentation, and Squad gave no GDC talk and published no paper.** There
is no equivalent here of Capcom's ten years of decks or Wube's Friday Facts.
What there *is*, unusually, is a lot of **shipped plain text** and a modding
community that had to reverse the internals to work at all.

There is **no local install on this machine** — Steam, GOG and the usual paths
were checked and KSP is not present — so unlike
[`simcity3000.md`](../strategy/simcity3000.md) or the Broken Arrow notes, nothing
here was read out of a retail build. Everything is published material.

| tag | source | strength |
|---|---|---|
| **[HARVESTER]** | **Felipe "HarvesteR" Falanghe**, KSP's creator, in his own dev blog (`kerbalspace.tumblr.com`, 2011–12) and in a **Physics Forums thread he started in March 2012** asking how to solve patched conics. The single best source on this game, and the origin of §2 and §7.1. | **Strong.** His own words, contemporaneous with the code being written. No numbers, no profiles. |
| **[CFG]** | KSP's shipped **`Physics.cfg`** (`PHYSICSGLOBALS`), read from a community dump. ~370 lines of the game's actual tuning constants, including every aerodynamic curve, the thermal model, buoyancy, joint break factors and the vessel range table. | **Strong.** These are the real shipped numbers. Some are unlabelled and their meaning has to be argued. |
| **[API]** | The **KSP API doxygen** (`anatid.github.io/XML-Documentation-for-the-KSP-API`), generated from the shipped assemblies. Class names, method signatures, field names and enum members are **real**; the prose descriptions are community-written and hedge with "presumably". | **Strong for structure, weak for prose.** A signature like `SolveSOI_BSP(...)` is evidence; a comment saying "presumably" is not. |
| **[CODE]** | Third-party source that manipulates stock internals and therefore documents them by construction: **Kerbal Joint Reinforcement** (ferram4 — reads and rewrites stock `PartJoint`s), **pqsmods-standalone** (Kopernicus — a faithful reimplementation of the stock PQSMod pipeline), **Kopernicus**, **kOS**. | **Strong.** KJR cannot set `j.angularXDrive.positionSpring` on a stock joint unless stock joints have that field. |
| **[SQUAD]** | Squad devnotes, changelogs, and — flagged as such — **KSP2** developer statements about the physics model KSP2 inherited unchanged from KSP1. | Moderate. Marketing-adjacent, but specific where it matters. |
| **[COMMUNITY]** | The KSP wiki, forum reverse-engineering, and the drag-model reconstruction in `Ren0k/Project-Atmospheric-Drag`, which re-derives stock aerodynamics from `Physics.cfg` and `PartDatabase.cfg` step by step. | Mixed. The drag reconstruction is excellent and checkable; the Kraken taxonomy is folklore. |
| **[inferred]** | Our reading. Not anybody's word. | — |

**The one thing to hold on to:** where this note describes the *orbital* model
and the *early terrain history*, that is HarvesteR's own account and it is
unusually candid. Where it describes the **joint solver's internals** (§4), there
is no Squad statement at all — the mechanism is assembled from what KJR does to
stock joints, from what `Physics.cfg` contains, and from how PhysX is known to
work, and §4.7 says which parts are argument rather than evidence.

---

## 1. The one thing that matters most

**A KSP vessel is in exactly one of two states at any instant, and they are
simulated by completely different code.**

| | **on rails** | **off rails** |
|---|---|---|
| position from | `Orbit.getPositionAtUT(t)` — closed form | integrating forces at 50 Hz |
| precision | `double`, `Vector3d`, `QuaternionD` | `float`, Unity `Vector3` |
| gravity | not applied; it is *baked into the conic* | `μ/r²` applied to every rigidbody, every step |
| joints | none — the vessel is one frozen transform | one Unity `ConfigurableJoint` per attachment |
| cost | O(1) per vessel per frame, at **any** time rate | O(parts) per vessel per 20 ms of *simulated* time |
| can it be evaluated at t + 10 years? | **yes, instantly** | no, only by stepping there |

**[HARVESTER]** The choice was deliberate and he stated its reason plainly in
2012, while building the trajectory planner:

> "The planetarium only simulates two-body gravity, and switches gravity sources
> for spacecraft based on the SOI they are in. **This was a decision made to make
> it possible to use deterministic propagation methods at all times**, for both
> the spacecraft and any celestial body."

And then the sentence that is the whole design:

> "**This also means a patched conic trajectory isn't an approximation in this
> system, but the actual trajectory the ship will fly.**"

**[inferred]** Read that as a specification, not a boast. In a game with n-body
gravity, the plotted trajectory and the flown trajectory are *different
computations* and will disagree; the plot is a forecast and forecasts drift. In
KSP they are the **same equations evaluated in different orders**, so the
predicted encounter with Duna in 214 days is not a prediction — it is a
statement about a function. Every piece of KSP's UI that players describe as
"trustworthy" (the manoeuvre node, the encounter markers, the closest-approach
readout) is trustworthy because of that one sentence.

**[inferred]** Four consequences, and they are the rest of this note:

1. **An entire solar system of vessels costs nothing.** Only the one you are
   looking at leaves the analytic universe. 200 satellites are 200 Kepler
   evaluations a frame. §6.7 is the distance table that decides.
2. **Time warp is free — and only for on-rails vessels.** Advancing `t` by an
   hour costs the same as advancing it by a millisecond. That is why warp goes to
   100,000× and physics warp stops at 4×. §6.
3. **Everything hard is a handover.** The Kraken is not one bug; it is the name
   players gave to the class of failure where the two universes disagree at the
   seam. §5 and §6.4.
4. **And the one system with no analytic counterpart inherits every one of
   PhysX's weaknesses undiluted.** That system is the vessel's structure, and
   that is why rockets wobble. §4.

---

## 2. The analytic universe

### 2.1 One gravity source, ever

**[COMMUNITY/inferred]** A vessel is inside exactly one body's **sphere of
influence**, and that body is the *only* source of gravity acting on it. The SOI
radius is the standard Laplace value

    r_SOI = a · (m / M)^(2/5)

with `a` the body's semi-major axis about its parent. Kerbin's is **84,159,286 m**
against a 600 km body radius — so the SOI is 140 planetary radii, and the Mun at
12 Mm is comfortably inside it.

**[inferred]** The boundary is **hard**. Cross it and the acceleration on your
ship changes discontinuously: one frame you are being pulled by Kerbin, the next
only by the Mun. Real spacecraft experience no such thing. KSP accepts the
discontinuity because the alternative destroys the property in §1 — an n-body
field has no closed-form propagator, and without a closed-form propagator there
is no free time warp, no exact trajectory plot and no O(1) idle vessel. **The SOI
is not a physics simplification, it is the precondition for the architecture.**

This is also why Lagrange points do not exist in stock KSP, why a low Mun orbit
never decays, and why the Principia mod — which replaces the whole thing with a
real n-body integrator — has to reimplement time warp, the trajectory display and
the manoeuvre planner from scratch. It is not adding a feature; it is deleting
the assumption everything else was built on.

### 2.2 The Kepler solver

**[API]** `Orbit` carries the six classical elements —
`inclination`, `eccentricity`, `semiMajorAxis`, `LAN`,
`argumentOfPeriapsis`, `meanAnomalyAtEpoch` — plus `epoch`, and the constructor
takes exactly those:

```csharp
Orbit(double inc, double e, double sma, double lan, double w, double mEp,
      double t, CelestialBody body)
```

The propagation chain is the textbook one and each step is a public method:

    UT  →  mean anomaly M  →  eccentric anomaly E  →  true anomaly ν  →  r, v

with the only hard step exposed as

```csharp
double solveEccentricAnomaly(double M, double ecc, double maxError, int maxIterations)
```

**[inferred]** Two things are worth noticing in that signature. It takes an
**error tolerance and an iteration cap**, which means Kepler's equation is solved
numerically per call rather than by a series expansion — correct, since no closed
form exists — and it is `double` throughout, which matters because eccentricity
near 1 makes the iteration ill-conditioned and a `float` solver would visibly
jitter a Mun transfer.

**[API]** The inverse direction — going from a *state* back to an orbit — is one
method, and it is the bridge between the two universes:

```csharp
void UpdateFromStateVectors(Vector3d pos, Vector3d vel, CelestialBody refBody, double UT)
```

**[inferred]** This is called every time a vessel comes off rails and, while
off rails, **every frame** — which is why the map view's orbit line twitches
while you are burning: it is being re-derived from a position and a velocity that
PhysX is perturbing. §6.6 is the hysteresis that stops that twitch becoming a
visible drift.

**[API]** One delightful detail in the reference-frame documentation: KSP's
`Orbit` methods return vectors in what the docs call **"AliceWorld"** — world
coordinates with the y and z axes swapped, hence right-handed, against Unity's
left-handed "World". Every orbital vector has to be flipped on the way out.
*Through the looking glass* is exactly the right name for a handedness swap, and
it is the sort of thing a codebase only names once it has cost somebody a week.

### 2.3 Patched conics: triage, then bisect

**[HARVESTER]** In March 2012 HarvesteR posted on Physics Forums asking how to
find where an orbit crosses a *moving* sphere of influence. His own account of
what he built, three months later:

> "Basically what it does is first **rule out orbits that won't intersect**,
> first by simple criteria, like periapsis and apoapsis intersection, then by
> comparing the orbital ellipses (as per Hoots' paper). If the orbits pass those
> initial conditions, it runs a **root-finding algorithm to converge on the point
> of SOI transition**, and calculates the new conic patch by taking your state
> vectors at that point, transforming them to the coordinate system of the new
> body, and using those vectors to compute the parameters of the new patch."

And the choice of solver, which is the sentence to keep:

> "**Hoots proposed using a Newton-Raphson solver** to find the solution, but
> that was too unreliable if the initial guess wasn't good enough. **So I wrote a
> simpler and more robust algorithm that is probably less efficient, but much
> more dependable. In my case here, it's more important to be reliable than to be
> efficient.**"

**[API]** The shipped code matches his description line for line, and the names
give away the algorithm:

```csharp
static bool PeApIntersects(Orbit primary, Orbit secondary, double threshold)
static bool SolveSOI_BSP(Orbit p, Orbit s, ref double UT, double dT, double Rsoi,
                         double MinUT, double MaxUT, double epsilon,
                         int maxIterations, ref int iterationCount)
static double SolveClosestApproach(...)
static void FindClosestPoints(...)
```

`BSP` is **binary space partition** — a bisection. That is the "simpler and more
robust algorithm": halve the interval, never diverge, converge slowly and always.

**[API]** The escalation ladder is an enum, and it is exactly the cheap-filter-
first ordering:

```csharp
enum EncounterSolutionLevel { NONE, ESCAPE, ORBIT_INTERSECT,
                              SOI_INTERSECT_2, SOI_INTERSECT_1 }
```

and the solver's budget is split into two named halves —
`maxGeometrySolverIterations` and `maxTimeSolverIterations`, reported separately
as `GeoSolverIterations`, `TimeSolverIterations1`, `TimeSolverIterations2`.
**Geometry first (do the ellipses come near each other at all?), time second
(will both bodies be there at once?)**, because the geometry test is a static
comparison of two conics and the time test needs a root find.

**[inferred]** This is CLAUDE.md's ranking rule with a different noun: *cheap
filters, then score, then the expensive test.* The apsis comparison rejects most
candidate bodies with two subtractions. Hoots' ellipse-proximity test rejects
most of the rest with no iteration. Only survivors reach the bisection. And the
per-stage iteration counters exist because somebody needed to see **which** stage
was eating the frame.

**[API]** The result is a linked list, not a curve: each `Orbit` has
`nextPatch` / `previousPatch`, `StartUT` / `EndUT`, and a
`patchEndTransition` of `{ INITIAL, FINAL, ENCOUNTER, ESCAPE, MANEUVER }`.
The chain is bounded by `maxTotalPatches`, and `IncreasePatchLimit()` /
`DecreasePatchLimit()` are the game's "conic patch limit" setting — **a user-
facing knob on how many iterations of an O(bodies × iterations) search you are
willing to pay for**, which is an honest way to expose a cost that genuinely
varies with what the player is doing.

**[API]** There is also `bool activePatch`, documented as: *"Often
`Orbit.nextPatch` for the last real will not be null, but will be some sort of
bogus Orbit object."* **[inferred]** — a pooled, reused patch object rather than
an allocation per replan, which is the right call for something recomputed every
frame while a manoeuvre gizmo is being dragged.

### 2.4 What the model costs, honestly

**[inferred]** Three real losses, and KSP shipped all three for fifteen years:

- **No perturbations.** Orbits are eternal. A 71 km Kerbin orbit never decays,
  a Mun orbit is never lumpy, and the Mun's own orbit is a fixed ellipse rather
  than something Kerbin and Kerbol argue about.
- **Discontinuous acceleration at SOI edges.** Visible as a "kick" in the map
  view when you cross, and as the reason a craft parked exactly on a boundary can
  flicker between reference bodies.
- **The plot is more accurate than the flight.** HarvesteR again, and this is
  the funniest true sentence in the sources: *"It's actually more accurate than
  the actual flight simulation."* The conic solver runs in double precision on
  clean elements; the flying vessel runs in single-precision PhysX with joints
  and thrust jitter. **The map is more real than the territory**, and the whole of
  §5 exists to narrow that gap.

---

## 3. The numeric universe

### 3.1 A vessel is a tree of rigidbodies

**[API/CODE]** Off rails, each physically-significant part is a Unity
`Rigidbody`, and each attachment is a `PartJoint` holding one or more Unity
`ConfigurableJoint`s. KJR gets at them by reflection:

```csharp
// KJRJointUtils
public static List<ConfigurableJoint> GetJointListFromAttachJoint(PartJoint partJoint)
```

**[inferred]** Two structural facts fall straight out of that and they decide
almost everything downstream. The connectivity is a **tree** — every part has one
`parent`, and `Part.attachJoint` is singular — so there is exactly one load path
from any part to the root. And the joints are **compliant**, not welds: KJR reads
and rewrites `j.xDrive`, `j.yDrive`, `j.zDrive`, `j.angularXDrive`,
`j.angularYZDrive` and `j.slerpDrive`, each a `JointDrive` carrying
`positionSpring`, `positionDamper` and `maximumForce`. You cannot set a spring
constant on a rigid constraint. §4 is what those two facts cost.

**[CFG]** Break thresholds come from the parts, scaled globally:

```
jointBreakForceFactor = 50
jointBreakTorqueFactor = 50
rigidJointBreakForceFactor = 1
rigidJointBreakTorqueFactor = 1
maxAngularVelocity = 50
```

**[CODE]** and the pair-wise rule is the weaker of the two, which KJR reproduces:
`breakForce = Math.Min(p.breakingForce, connectedPart.breakingForce) * factor`.

**[inferred]** `jointBreakForceFactor = 50` against
`rigidJointBreakForceFactor = 1` is the single most informative line in
`Physics.cfg`. A normal joint is fifty times stronger than the part's authored
figure; a **rigid** joint — the "rigid attachment" toggle from 1.2 — is exactly
one times. **Rigidity was bought at 50× the fragility**, deliberately and in a
config file, and that is the honest form of the trade: you cannot have a
constraint that neither bends nor breaks, so the game lets the player choose
which.

`maxAngularVelocity = 50` (rad/s, ~478 rpm) is a hard clamp on rigidbody spin —
the guard rail that stops a phantom-force spin-up going to infinity in one step,
which is the mechanism behind the Kraken players named "gyro".

### 3.2 Gravity, and the frame it is applied in

**[inferred]** Off rails, gravity is not special: the flight integrator applies
`a = μ/r²` toward the current main body to every part's rigidbody each
`FixedUpdate`. One body, no others (§2.1). That is four lines of code and it is
the entire "gravity on planets" system.

The interesting part is the **frame**.

**[API/COMMUNITY]** `Planetarium.FrameIsRotating()` and
`CelestialBody.inverseRotation` expose a switch that happens at an altitude
threshold — **100 km at Kerbin**, different per body. Below it, the game enters
*inverse rotation*: **the planet is held perfectly still and the entire rest of
the universe is rotated around it instead.**

**[inferred]** This is the floating-origin trick applied to *rotation*, and it
buys three things at once:

- The PQS terrain mesh and all its colliders (§7.6) never move. A rotating
  600 km sphere would be translating its surface at 175 m/s at the equator, and
  every collider vertex would be re-transformed every frame in `float`.
- The launchpad is at a fixed position, so a vessel sitting on it is not fighting
  a moving contact.
- Above the threshold the frame becomes inertial and the planet spins normally,
  which is the frame the `Orbit` elements are defined in — so the switch is also
  the boundary at which "surface velocity" stops being the interesting quantity
  and "orbital velocity" starts. **The navball's mode switch (§10.2) is the same
  boundary made visible.**

The bill is that the rotating frame is non-inertial, so **centrifugal and
Coriolis accelerations must be added by hand**. They are: KJR's easing feature
names precisely *"external forces (gravity, centrifugal, coriolis)"* as the three
things it ramps, which is direct evidence stock applies all three as explicit
terms.

### 3.3 Physicsless parts

**[API]** `Part.physicalSignificance` is an enum of `{ FULL, NONE }`, documented:
*"If `physicalSignificance == NONE`, then this part doesn't actually have any
physics. In particular, it has no mass, regardless of what its 'mass' field is
set to, and no drag."*

**[COMMUNITY]** In practice the mass and drag are **added to the parent part**,
and no `Rigidbody` is created. Science instruments, small antennae, ladders and
lights are all physicsless.

**[inferred]** This is a genuinely good idea and it is the cheapest possible
answer to "your part count is your frame time". A thermometer contributes 0.005 t
to the vessel and would otherwise cost a rigidbody, a joint, six solver
constraints and a collider — **for a part that can never be the interesting one
in any interaction**. Folding it into the parent keeps the mass budget exactly
right and removes it from the solver entirely.

It also produces one of the game's odder emergent facts: a physicsless part's
mass acts at its **parent's** centre of mass, not its own, so hanging a battery
bank off one side of a rocket does not shift the CoM. Players discovered this and
called it a bug. It is the price of the optimisation, and it is a clean instance
of the derived-cache hazard in CLAUDE.md — *the summary is right about the total
and wrong about the distribution.*

---

## 4. Why the rockets wobble

This is the question KSP is most famous for and the one with the least official
explanation, so this section is assembled rather than quoted, and §4.7 marks
the join.

### 4.1 The joint is a spring, and always was

**[HARVESTER]** From the very first prototype, before there was a camera or a
control scheme:

> "There were no controls, no camera work. A few static values had been set to
> make the rocket go up, and **the joints (there were joints already) carefully
> set so that they would shear off while the rocket was still in the frame**."

**[inferred]** The founding demo of Kerbal Space Program was *a rocket breaking
at its joints*. Structural failure is not a consequence of the physics engine
choice; it is the thing the game was about on day one, and the physics engine was
chosen to deliver it.

**[CODE]** Stock joints carry `JointDrive`s (§3.1). A `JointDrive` is a
spring-damper: it produces a restoring force proportional to constraint error
rather than eliminating the error. Even where motion is locked, PhysX resolves
locked degrees of freedom with an **iterative solver over a fixed iteration
budget per step**, so a "locked" axis is satisfied to within whatever residual
the solver reached — which is to say, also a spring, just a stiff one.

**[SQUAD]** And Squad said so, in the 1.1 Unity 5 devnotes, while explaining why
vessels came out of the upgrade looking bent:

> "PhysX 3.3 changed the internal limits of **springs in joints**, which caused
> some vessels to look distorted, but this was fixed by adjusting **the global
> tuning factor for joint rigidity**."

A "global tuning factor for joint rigidity" is a spring constant with a nicer
name.

### 4.2 Compliance accumulates down the chain

**[inferred]** This is the mechanism, and it is the part that surprises people.

A single joint under load deflects by a small angle θ. Two joints in series
deflect by 2θ *at the tip*, but the second joint's deflection is measured against
an already-rotated parent, so tip displacement grows faster than linearly with
depth. A thirty-part stack is thirty compliant hinges in series and the nose is
the sum of thirty small errors, each amplified by its lever arm.

Worse, PhysX's solver is projected Gauss-Seidel: **information propagates one
joint per iteration.** With a fixed iteration count per step, a constraint chain
longer than the iteration count *cannot* converge within one step, by
construction — the engine at the bottom has not yet "told" the payload at the top
that it fired. The residual is not a tuning failure; it is the algorithm's
defined behaviour on deep chains.

**[inferred]** So the observable is: **three parts are rigid, ten parts are
springy, thirty parts are a wet noodle** — and the transition is not a threshold,
it is a smooth function of depth that crosses the perceptual line somewhere around
"an actual rocket".

### 4.3 Mass ratio makes it worse

**[SQUAD]** KSP2 inherited this model unchanged, and its developers named the
aggravating factor while listing mitigations:

> "altering **inertia tensor** values to decrease joint issues that emerge when
> **high-mass and low-mass parts are connected**"

**[inferred]** A constraint between a 40 t tank and a 0.05 t decoupler is
numerically ill-conditioned: the impulse that barely moves the tank flings the
decoupler, and the solver oscillates between over- and under-correcting. Games
that fight this either clamp the mass ratio or lie about the inertia tensor, and
KSP2's team say they do the latter. **[COMMUNITY]** KSP1 does the same — the
inertia tensor of small parts is inflated on load, which is why a probe core
attached to a heavy stack behaves better than its real inertia would allow.

### 4.4 The spring does not scale with what it is holding

**[CODE]** This is KJR's central fix, and it is one line:

```csharp
angDrive.positionSpring = Mathf.Max(momentOfInertia * KJRJointUtils.angularDriveSpring,
                                    angDrive.positionSpring);
```

with a default `angularDriveSpring` of **5×10¹²**, and breaking strength
similarly re-derived from geometry:

```
breakStrengthPerArea = 1500     // joint force strength per unit contact area
breakTorquePerMOI    = 6000     // joint torque strength per unit moment of inertia
```

**[inferred]** The stock model scales joint *strength* by part properties but
does not scale joint *stiffness* by the inertia it has to hold. That matters
because the natural frequency of a spring-mass system is ω = √(k/I). Hold k
fixed and grow I, and **ω falls** — so the bigger the rocket, the *slower* and
*larger* its structural mode. That is precisely the observed behaviour: a small
launcher buzzes imperceptibly, a 300 t stack sways with a visible period of a
second or two. KJR's fix is to make k proportional to I, which holds ω constant
across vessel sizes. **A single fixed stiffness cannot be right for a game whose
whole point is that the player chooses the mass.**

### 4.5 And then SAS closes the loop

**[inferred]** The last ingredient is that KSP's attitude controller senses
vessel rotation and applies correcting torque — and the sensor is on the command
pod, which is on one end of the compliant chain, while the actuator (gimballed
engine) is on the other.

That is a control loop closed around a lightly-damped structural mode with a
transport delay, which is the textbook recipe for **aeroservoelastic coupling**.
Push a bit late and you add energy every cycle. Real launch vehicles have exactly
this problem and solve it with notch filters tuned to the bending modes; KSP has
no such filter, so a sufficiently floppy rocket with SAS on will diverge, and
players' folk remedy — *turn SAS off* — is the correct one.

This is why the failure reads as mysterious. Nothing is broken. The structure is
stable, the controller is stable, and **the two together are not**.

### 4.6 Every fix that worked was topological

**[COMMUNITY/CFG]** In order of arrival:

| fix | version | what it actually does |
|---|---|---|
| **struts** | early | a part that creates an *extra* joint between two others — **makes the connectivity graph not a tree** |
| **KJR** | mod | raises k, scales it by MOI, and connects each part to its *grandparent* as well as its parent |
| **autostrut** | 1.2 | an invisible strut to the heaviest part / root / grandparent, chosen automatically. `autoStrutTechRequired = generalConstruction` in `Physics.cfg` |
| **rigid attachment** | 1.2 | locks the drives on one joint — at `rigidJointBreakForceFactor = 1`, i.e. 1/50th the strength |

**[inferred] Three of those four shorten the load path or add a second one, and
none of them raises the spring constant globally.** That is not a coincidence,
and it is the generalisable result:

> **The stiffness of a chain of compliant constraints is set by its depth. If
> the structure is too soft, the fix is to change the graph, not the numbers.**

Adding a strut from the nose to the base turns a 30-deep chain into two 15-deep
chains sharing load, and the solver's convergence problem is halved for free.
Doubling k does nothing of the sort — it just moves the instability threshold and
makes the explicit integrator less stable at the same timestep (§6.3), which is
why KJR has to *weaken* joints as it stiffens them and has an option named
exactly that.

**[SQUAD]** KSP2's creative director, on why the obvious remedy is not one:

> "As a person who has dive-bombed more than one physics meeting with an
> exasperated '**can't we just make the joints stiffer**' comment, let me assure
> you that in true KSP fashion, **this is not a problem with a simple remedy**."

### 4.7 What is evidence and what is argument

**Evidence:** stock joints are `ConfigurableJoint`s with `JointDrive`s carrying
`positionSpring` (KJR's source cannot compile otherwise); the break factors and
the rigid-joint factor of 1 (`Physics.cfg`); Squad's own words about "springs in
joints" and "a global tuning factor for joint rigidity"; KSP2's statement about
inertia tensors and mass ratio; the four shipped mitigations and what each does.

**Argument:** the Gauss-Seidel depth explanation, the ω = √(k/I) reading of why
large vessels wobble slowly, and the aeroservoelastic account of the SAS
interaction. Those are consistent with every observable and with how PhysX is
documented to work, but **no Squad source states them**, and a decompile would be
needed to promote them.

---

## 5. Krakensbane — keeping single-precision floats in range

### 5.1 HarvesteR hit this in month two, and wrote it up himself

**[HARVESTER]** From *The Story of KSP – KSP 0.2*, on adding spherical gravity:

> "It was around here that we had our first encounter with floating-point
> inaccuracy issues, **a set of problems that would pose a challenge to
> development from then on**. […] At distances like 1,000,000.0 m, the finest
> detail the floating point can tell apart is on the tens of centimetres scale.
> That means it can no longer know what happened in the space between that, and
> the result is **a visible shaking in the game**. […] A million meters is peanuts
> to space, so you can imagine, after a million kilometres, just how much
> precision we would be losing. **Your ship's parts could be anywhere inside a
> 100 m-wide radius.**"

And the compromise it forced immediately:

> "the game's project called for a planet 1/10th the radius of Earth. That was
> our goal at least, and we were forced to compromise. **The very first version of
> Kerbin was only 20 km big.**"

**[inferred]** Kerbin shipped at 600 km — still 1/10th Earth, so the goal was
eventually met — but the 20 km stopgap is the honest record of what the problem
costs when you have not yet solved it. Note also that *Kerbin being small* is
itself partly a precision decision, not just a pacing one.

### 5.2 Two frames, not one

**[API]** The fix has a name and a class, and the doc comment is the clearest
single statement in the whole API:

> "The physics simulation has problems if vessels move **too fast relative to the
> underlying reference frame** used by the simulation, or get **too far from the
> origin** of the coordinate system. **Krakensbane shifts the reference frame
> origin *and velocity*** so that the active vessel is always **near the origin
> of, and moving slowly with respect to**, the underlying coordinate system used
> by the physics simulation."

with the fields

```csharp
Vector3d FrameVel;      Vector3d excessV;      Vector3d RBVel;
Vector3d totalVel;      Vector3d lastCorrection;      float MaxV;
static Vector3d GetFrameVelocity();
static void ResetVelocityFrame();
void setOffset(Vector3d offset);   // "The offset can be very large and the
                                   //  vessels will not break, unlike Vessel.SetPosition"
```

**[inferred]** The *velocity* half is the part people miss, and it is the more
important one. A plain floating origin fixes position error and does nothing for
the other failure: at 3,000 m/s with a 20 ms step, a part moves 60 m per step,
and the *relative* motion between two parts of the same rocket — which is what
the joint solver cares about — is being computed as the difference of two large,
nearly-equal numbers. Catastrophic cancellation, every step, on every joint.

Krakensbane's answer is to keep a running `FrameVel` and subtract it from every
rigidbody, so PhysX only ever sees a vessel doing a few metres per second while
the *frame* does 3,000. When `RBVel` exceeds `MaxV`, the excess (`excessV`) is
moved out of the rigidbodies and into the frame. **The rocket is stationary and
the universe is moving.** True orbital velocity is `FrameVel + RBVel`, and the
`GetFrameVelocity()` accessor exists because every mod that wants a real velocity
has to add the two back together.

`setOffset`'s comment — *"the offset can be very large and the vessels will not
break, unlike `Vessel.SetPosition`"* — is a warning left by somebody who
discovered that teleporting a jointed vessel by writing transforms makes the
solver see an enormous instantaneous constraint violation and detonate it. The
whole point of the method is that it moves **everything** consistently, so no
joint sees any relative change at all.

### 5.3 Rotation gets the same treatment

Already covered in §3.2, but the symmetry is worth stating in one line:
**Krakensbane holds the vessel still in translation; inverse rotation holds the
planet still in rotation.** Both are the same move — *pick the thing whose local
numerical accuracy matters most, nail it to the origin, and let the universe do
the moving.*

### 5.4 Read against Space Engineers

**[inferred]** [`space_engineers.md`](space_engineers.md) solves the identical
problem the opposite way: SE **partitions** the world into physics *clusters*,
each with its own local origin, and simulates several at once. KSP **rebases** a
single origin and simulates one vessel.

Neither is cleverer. The difference is forced:

- SE must simulate multiple player-built grids simultaneously in the same
  session, so it needs N independently-accurate neighbourhoods, and therefore N
  origins and a policy for merging and splitting them.
- KSP simulates **exactly one vessel** (plus anything within 200 m, §6.7) because
  everything else is analytic. One simulated island needs one origin, and the
  policy is "follow the player".

**The number of origins a game needs is the number of things it actually
simulates.** KSP's whole precision story is cheap because §1 made that number
one.

---

## 6. Time warp, and why it eats spacecraft

### 6.1 Two warps, and they are unrelated systems

**[API]** `TimeWarp.Modes` is `{ HIGH, LOW }` — documented as "regular" and
"physics" warp — with two separate rate tables:

```csharp
float[] warpRates;         // 1, 5, 10, 50, 100, 1000, 10000, 100000
float[] physicsWarpRates;  // 1, 2, 3, 4
```

Four orders of magnitude between the ceilings, and the reason is entirely §1.

**[API]** Note also `SetRate(int rate_index, bool instant)`, with *"if false, KSP
will gradually smoothly adjust the warp rate up or down until it reaches the
target"*, and `CurrentRate` explicitly documented as possibly *not* equal to
`warpRates[CurrentRateIndex]`. **[inferred]** Warp rate is a ramped continuous
quantity rather than a discrete setting — because a step change in `dt` is
exactly the disturbance §6.3 is about, and ramping it gives the solver time to
settle.

**[API]** And `GetAltitudeLimit(int i, CelestialBody body)` — the per-body, per-
rate minimum altitude. **[inferred]** This is a **safety interlock, not a game
rule**: warping fast near a body means the on-rails position jumps far enough per
frame that a vessel can pass through terrain between samples. Rather than
detecting that, the game forbids the configuration.

### 6.2 On-rails warp costs nothing because it leaves PhysX entirely

**[API]** `Vessel.GoOnRails()` / `GoOffRails()`. On rails, the vessel is
*packed*: rigidbodies stop being simulated, joints are inert, collisions are off,
and position comes from `Orbit.getPositionAtUT(t)`.

**[inferred]** At 100,000× the game advances `Planetarium.time` by 2,000 s per
frame and asks the conic for a position. That is the same arithmetic it does at
1×. There is no integration and therefore no accumulation, no stability limit and
no cost. **This is the payoff for §1 and it is the single biggest reason KSP
plays the way it does** — an interplanetary transfer is a five-second wait rather
than a design problem.

### 6.3 Physics warp stops at 4× because of the integrator

**[API]** `TimeWarp.fixedDeltaTime` — *"the time between FixedUpdate cycles"* —
is what physics warp scales. **[COMMUNITY]** The base is 0.02 s (50 Hz).

**[inferred]** So 4× physics warp integrates the whole vessel at **80 ms per
step**. An explicit integrator on a spring-damper is stable only for roughly
`dt < 2/ω`. Every joint in §4 has a natural frequency; multiplying `dt` by four
divides the stable frequency ceiling by four, and every structural mode above the
new ceiling starts **gaining** energy each step instead of losing it. The rocket
does not wobble more — it wobbles *divergently*, and comes apart.

The same step size makes contacts tunnel: at 80 ms a landing leg moving at
30 m/s travels 2.4 m between checks, straight through the ground.

**That is the whole answer to "why is physics warp capped so low and why does it
destroy things".** It is not conservatism. 4× is approximately where the
stiffest joints in a normal vessel cross their stability limit, and the cap is
set just below the cliff.

### 6.4 The handover is where the ships actually die

**[COMMUNITY]** The KSP wiki states the mechanism directly:

> "Any joints that were bent when entering non-physical time warp **will stay
> bent during warp, and will spring back, potentially violently and
> destructively, when leaving time warp**."

**[inferred]** This is the seam in §1 failing in the cleanest possible way. Going
on rails freezes the vessel's transforms — *including* whatever elastic
deflection every joint happened to be carrying at that instant. The stored strain
energy is preserved perfectly for however long you warp, and then handed back to
the solver in a single 20 ms step. A rocket under 3 g thrust when you hit warp is
a loaded spring; releasing it is an impulse the joints never saw during normal
flight.

**[inferred]** Everything the community calls a "Kraken" is a variant of this or
of §5:

| folk name | actual failure |
|---|---|
| Deep Space Kraken | float precision at large coordinates — the thing Krakensbane fixed |
| gyro Kraken | phantom forces from overlapping colliders, clamped by `maxAngularVelocity = 50` |
| Cthulhu / Ghost Kraken | stored joint strain released on leaving warp — §6.4 |
| Kore Kraken | tunnelling at high speed — "tick-based physics at high speeds can easily miss collisions", per the wiki |

**They are one bug with four names, and the bug is "the analytic universe and
the numeric universe disagreed at the boundary".**

### 6.5 Physics easing is the mitigation

**[CODE]** KJR's own feature list describes the stock idea it extends:

> "Slowly dials up external forces (**gravity, centrifugal, coriolis**) when on
> the surface of a planet, reducing the initial stress during loading. All parts
> and joints are **strengthened heavily during physics loading** (coming off of
> rails) to prevent Kraken attacks on ships."

**[CFG]** `buildingEasingInvulnerableTime = 2` is the same idea for the space
centre's buildings.

**[inferred]** The pattern is worth naming: **when a simulation resumes, ramp the
inputs rather than the state.** A vessel that materialises on the Mun under full
gravity with cold joints sees a step change in load and rings; the same vessel
with gravity ramped from 0 to 1 over a second or two settles into its sag
quietly. And the joints are made temporarily unbreakable during the ramp,
because the transient is guaranteed to exceed the steady-state load and there is
no point in the game evaluating a failure condition it knows is spurious.

Anything in this project that pauses and resumes a stateful system — a
reactivated agent, a re-enabled physics prop, a streamed-in region — has exactly
this problem, and this is exactly the fix.

### 6.6 Orbit drift, and an incumbency bias in the config file

**[CFG]**

```
orbitDriftFramesToWait   = 5
orbitDriftSqrThreshold   = 1E-10
orbitDriftAltThreshold   = 400000000
```

**[inferred]** While off rails, the vessel's orbit is re-derived from state
vectors every frame (§2.2), and PhysX noise means the derived elements never sit
still. These three constants are the filter: **a change must persist for five
frames and exceed a squared threshold before it is accepted, and above 400,000 km
it is not tracked at all.**

That is CLAUDE.md's incumbency rule almost verbatim — *when a choice is re-made
repeatedly, give the incumbent a discount* — arrived at by a different studio for
a different reason. Here the symptom of not having it is not an oscillating unit
but a periapsis readout that flickers in the last three digits and a map-view
orbit line that crawls. Same disease, same cure: **a scorer (or an estimator)
that ignores its own previous answer thrashes.**

### 6.7 The distance table

**[CFG]** `Physics.cfg` ships `VesselRanges`, keyed by exactly the members of the
`Vessel.Situations` enum:

| situation | load | unload | pack | unpack |
|---|---|---|---|---|
| prelaunch / landed / splashed | 2250 | 2500 | 350 | 200 |
| orbit / escaping | 2250 | 2500 | 350 | 200 |
| subOrbital | 2250 | 15000 | 10000 | 200 |
| flying | 2250 | 22500 | 25000 | 2000 |

**[COMMUNITY]** *Loaded* means the parts exist as objects; *unpacked* means they
are actually being simulated.

**[inferred]** Three readings. **Load and simulate are separate decisions with
separate radii** (2250 m vs 200 m), so a nearby station is drawn and queryable
without any of its parts entering the solver — the cheap tier is a real tier, not
a fallback. **The situation, not the distance, chooses the policy**: a `flying`
vessel gets 2,000 m of unpack radius against 200 m for a landed one, because a
decoupled booster is separating at speed and must stay simulated long enough to
be visibly gone. And **`pack` (25,000) exceeding `unload` (22,500) for `flying`
is not a typo** — it means an aircraft is unloaded *before* it would have been
packed, i.e. that situation skips the intermediate state on the way out.

---

## 7. Planets

### 7.1 The prehistory, which is the best-documented part

**[HARVESTER]** KSP 0.4's terrain was not a quad sphere. It was **one 19,000-vertex
disk from 3ds Max** that followed the player:

> "It was tessellated in several steps, so the centre of the disk had a much
> higher polygon density than the edges. […] the disk would follow the player
> around, and it would get larger if you went higher, and lower if you went closer
> to the ground. Because the disk was always just below you, the 'tiers' of mesh
> detail would work as **a crude LOD system**."

Three failures, all of which the shipped system is a direct response to, and he
names all three:

- **Colliders.** *"Unity goes pretty fast until you need to use collision meshes
  on things that are changing shape, and that's exactly what we had here."* The
  fix was a small separate collision patch under the player.
- **Vertex swimming.** *"If on one frame a vertex was sitting on the top of a
  mountain, on the next one, it might be half-way down its slope […] **the noise
  'field' was much more detailed than the mesh**."* The stopgap was to move the
  disk less often — i.e. hide it rather than fix it.
- **A finite disk.** So a second, simpler representation had to exist beyond its
  edge: *"a simple textured sphere placed 'under' the disk-terrain […] coupled
  with a terrain shader that could fade away as it went farther from the
  camera."*

**[inferred] That last paragraph is the birth of ScaledSpace.** The two-
representation planet with a distance crossfade was invented as a patch for a
terrain system that no longer exists, and it outlived it by fourteen years —
because the underlying reason (you cannot afford real geometry at planetary
distance, and you do not need it) never went away.

He also names the noise library — **LibNoise** — and the atmosphere:

> "This was done with 'atmospheric scattering' shaders, which were made
> accessible to the world by a guy called **Sean O'Neil**, who wrote an article
> about that on a developer publication called **GPU Gems**."

O'Neil's *Accurate Atmospheric Scattering* is GPU Gems 2, chapter 16. Fourteen
years later that lineage is still visible in KSP's ScaledSpace atmosphere shell.

### 7.2 Local space and ScaledSpace

**[API]** `ScaledSpace` is *"a class that handles the transformations between the
scaled-down coordinate system used by the map view and the regular coordinate
system used by the main flight view and the physics"*, with
`LocalToScaledSpace` / `ScaledToLocalSpace` and a `scaleFactor`.

**[COMMUNITY]** The factor is **1:6000** — Jool's scaled mesh has radius 1,000
against a real radius of 6,000,000 m.

**[inferred]** So every celestial body exists twice, simultaneously: as a
1:6000 textured sphere in a scene rendered by its own camera with its own far
plane, and (when you are close enough) as a full-scale PQS. The 6000 is not
arbitrary — it puts the *entire Kerbol system*, orbital radii and all, inside a
coordinate range that a `float` depth buffer and a `float` transform hierarchy
can represent without banding. **The map view and the "distant planets in the
sky" are the same objects**, which is why a planet's appearance is consistent
between them and why a mod that adds clouds to ScaledSpace changes both at once.

### 7.3 PQS: six quadtrees on a cube

**[HARVESTER]** The replacement for the disk, announced in 0.10 and explained by
him at the time:

> "The new terrain is based on a **Quadtree**. […] But for a planet, a single
> quadtree isn't enough. Quadtrees are 2D spatial structures, so they don't apply
> themselves too well for a spherical terrain. **So the solution is to use not
> one, but six quadtrees, arranged as a cube.** What happens is that each vertex
> on a quadtree terrain tile is pushed outward from the centre of the planet,
> creating a **spherified cube**. This […] has no poles, which means no points
> where the quads would become terribly distorted."

and the three properties he claims for it, each one answering a §7.1 failure:

> "**it's fast. Really fast.** Since you only need to create 4 new nodes at a
> time, and those are quite small to begin with, the game can do it without
> blinking. That means we no longer have to use yielding and coroutines […]
> **you will never be able to outrun the terrain.**"
>
> "A tile-based terrain also plays a lot more nicely with the physics. **Each
> tile has its own collision mesh** (well, the more detailed ones at least), so
> there are no issues with exploding when landing on the far side."
>
> "the terrain shader used to texture the terrain can now work in **tile-space**,
> which means it's a simpler shader, and **a Mac version is now possible**."

**[inferred]** Tile-space is doing quiet, heavy lifting there. Shading in a local
frame means the shader's inputs are small numbers regardless of where on a 600 km
sphere the tile is — **the same precision argument as Krakensbane, applied to the
fragment shader**, and it is what made the terrain shader portable enough to run
on OpenGL/Mac in 2011.

PQS stands for **Procedural Quad Sphere**, and the name survives into every
modding tool.

### 7.4 The terrain is a modifier stack, not a heightmap

**[CODE]** From Kopernicus's faithful reimplementation of the stock pipeline, the
whole interface is five virtuals:

```csharp
public abstract class PQSMod {
    public Int32 order = 100;
    public virtual void OnSetup() { }
    public virtual void OnVertexBuildHeight(VertexBuildData data) { }
    public virtual void OnVertexBuild(VertexBuildData data) { }
    public virtual Double GetVertexMaxHeight() { return 0; }
    public virtual Double GetVertexMinHeight() { return 0; }
}
```

and the sphere runs them in `order`, twice, over every vertex:

```csharp
public void OnVertexBuildHeight(VertexBuildData data) {
    data.latitude  = Math.Asin(data.directionFromCenter.Y);
    data.directionXZ = Vector3.Normalize(new Vector3(d.X, 0, d.Z));
    data.longitude = ...;
    data.v = data.latitude  / Math.PI + 0.5;
    data.u = data.longitude / Math.PI * 0.5;
    foreach (PQSMod mod in mods) mod.OnVertexBuildHeight(data);
}
```

A representative mod is four lines of actual work:

```csharp
public override void OnVertexBuildHeight(VertexBuildData data) {
    data.vertHeight += heightMapOffset + heightDeformity * heightMap.GetPixelFloat(data.u, data.v);
}
```

**[inferred]** Four things fall out, and they are why this design lasted:

1. **A vertex's height is a pure function of its direction from the planet
   centre.** No neighbour access, no state, no ordering dependency beyond
   `order`. So a quad can be built at any subdivision level, in any order,
   independently — which is what makes the quadtree cheap and what would make it
   trivially parallel.
2. **Height and colour are separate passes** (`OnVertexBuildHeight` then
   `OnVertexBuild`), which lets a colour mod read the final height that every
   height mod contributed. The alternative — one pass — would make "colour by
   altitude" depend on mod ordering in a way nobody could reason about.
3. **It is `Double` throughout.** On a 600 km sphere a `float` height field
   quantises to metres; `double` is what lets a 2 m boulder exist on a 6,000 km
   Jool.
4. **It is a stack of small named things, and that is the whole modding story.**
   Kopernicus adds planets by appending `PQSMod`s. There is no planet format —
   the planet *is* its mod list. `VertexHeightMap`, `VertexSimplexHeight`,
   `VoronoiCraters`, `FlattenArea`, `HeightColorMap`, `LandControl` are all peers
   in one array, and "flatten the launch site" is the same kind of object as
   "generate the whole continent".

### 7.5 Bounds without evaluation

**[CODE]** The most quietly clever line in the PQS:

```csharp
radiusMin = 0; radiusMax = 0;
foreach (PQSMod mod in mods) {
    radiusMin += mod.GetVertexMinHeight();
    radiusMax += mod.GetVertexMaxHeight();
}
```

**[inferred]** Every mod declares its own extreme contribution — the heightmap
mod returns `heightMapOffset` and `heightMapOffset + heightDeformity`, which it
knows without sampling a single texel — and the sphere sums them.

So **the planet knows its own minimum and maximum radius before generating any
terrain at all**. That is what lets the quadtree bound a quad with a conservative
shell and decide whether to subdivide, cull or collide it *without evaluating the
noise inside it*. It is the cheapest possible conservative bound and it costs one
extra method per mod.

This is the pattern CLAUDE.md calls out under derived caches, in its healthiest
form: **the fast path only skips work that provably does nothing.** The bound can
be loose and the system is still correct; it can never be *wrong*, because every
mod is required to declare an honest envelope.

### 7.6 Colliders, and why this stayed on the CPU

**[COMMUNITY/CODE]** PQS quads carry colliders only at the deeper subdivision
levels — HarvesteR's *"well, the more detailed ones at least"* — and the Kopernicus
docs expose the LOD knobs directly: `minLevel`, `maxLevel` (typically 2 and 10),
`minDetailDistance`, `maxQuadLengthsPerFrame`, plus a `PhysicsMaterial` subnode
with `staticFriction = 0.8`, `dynamicFriction = 0.6`, `bounciness = 0.0`.

The handover to ScaledSpace is four numbers: PQS `fadeStart`/`fadeEnd` are
documented as needing to *"line up with ScaledVersion's `fadeEnd`/`fadeStart`"* —
note the **crossed** pairing, i.e. a genuine crossfade where both are drawn — and
`deactivateAltitude` switches the PQS off entirely above it.

**[inferred]** The crossed fade is worth pausing on. If both representations of
the same 600 km sphere are visible simultaneously over an altitude band, they
must agree geometrically to within a pixel or the seam pops. That is only
affordable because the ScaledSpace mesh is *baked from the same PQS* — the
scaled mesh is the PQS evaluated at a low level and divided by 6000. **One
generator, two consumers, and the LOD transition is an alpha blend rather than a
geometry swap.**

### 7.7 Read against Elite Dangerous

**[inferred]** [`elite_dangerous.md`](elite_dangerous.md) §3 describes Frontier's
planet pipeline as **cube → quadtree → spherify → noise**. That is, character
for character, HarvesteR's 2011 blog post. Two teams, three years and an ocean
apart, with no shared literature, reached the identical topology — so the shape
is forced by the problem, exactly as [`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md)
§10 finds for spatial indexing and vegetation.

Where they split is instructive and it is **not** about pixels:

| | **KSP (PQS)** | **Elite Dangerous** |
|---|---|---|
| noise evaluated on | CPU, `double`, per vertex | **GPU compute shaders** |
| far LOD | a 1:6000 mesh **baked from the same generator** | terrain **stops being geometry and becomes a baked texture set** |
| collision | **every detailed quad has a mesh collider** | landable surface, but the terrain is a render target first |
| editable | no | no |

**[inferred]** KSP kept the generator on the CPU because **it needs to collide
with the result, and a height field that only exists in a GPU buffer cannot
cheaply answer "where is the ground under this landing leg".** ED could move to
compute because its terrain's primary consumer is the rasteriser.

That is the third independent arrival in this study directory at the same rule —
with [`dcs_clouds.md`](../flight/dcs/dcs_clouds.md) §11 (a volumetric cloud
system that lost the line-of-sight query the 1999 particle system had) and
[`ruse.md`](../strategy/ruse.md) §5 (three kd-trees split by *purpose*):

> **Render geometry and query geometry are different assets. Moving a
> representation onto the GPU improves the image and can silently delete a query
> the simulation depended on.**

KSP is the case where the query was recognised as load-bearing *first*, so the
move never happened.

---

## 8. Aerodynamics: six numbers per part, baked at build time

### 8.1 The souposphere, and what replaced it

**[COMMUNITY]** Before 1.0 (2015), drag was `mass × maximum_drag` — literally
proportional to a part's mass, with a per-part coefficient. The API doc still
carries the fossil: *"The drag coefficient of this part is equal to (total mass) *
(maximum_drag)"*, and next to it `minimum_drag`, documented simply as
**"Unused."**

**[inferred]** Mass-proportional drag is not a simplification of aerodynamics,
it is a different phenomenon wearing its name — it means a fuel tank gets *less*
draggy as it empties, and that a fairing can never help, because a fairing adds
mass. It is why the pre-1.0 ascent profile was "straight up to 10 km, then pitch
over hard": the atmosphere was thick, drag scaled with the thing you were trying
to keep, and there was no reward for a streamlined shape.

**[SQUAD]** 1.0 replaced it wholesale: lift *"correctly calculated and applied
for all lift-generating parts"*, drag *"pre-calculated automatically based on
part geometry, and applied based on part orientation in flight"*, plus
stack occlusion, induced drag, stalls and body lift.

### 8.2 Drag cubes

**[COMMUNITY]** Every part carries six faces (`XP XN YP YN ZP ZN`), each with an
**area**, a **drag coefficient** and a **depth**, baked offline from the mesh and
shipped in `PartDatabase.cfg`. A liquid fuel fuselage, verbatim:

```
cube = Default, 2.432,0.7714,0.7222,  2.432,0.7714,0.7222,
                1.213,0.9716,0.1341,  1.213,0.9716,0.1341,
                2.432,0.7688,0.7222,  2.432,0.7688,0.7222, ...
```

At runtime, the airflow direction in part space picks which faces are exposed and
by how much, and each exposed face contributes its own type of drag — **tip** for
the face into the flow, **tail** for the face away from it, **surface** (skin
friction) for the sides.

**[inferred]** This is exactly the derived-cache pattern from CLAUDE.md, applied
to geometry: **the authoritative data is the mesh; the summary is 18 floats; the
summary is built offline and the runtime never touches the mesh.** Six faces is
enough to be directionally correct — a flat plate and a cone genuinely differ in
their face areas — and it makes the per-part per-frame cost three dot products
and a table lookup rather than anything resembling a surface integral.

It is also why fairings are special-cased: a fairing's shape is procedural, so its
drag cube has to be **generated at runtime**, and the community reconstruction
notes flatly that *"the process KSP uses to do this can not be re-created"*.

### 8.3 Everything else is a curve in a config file

**[CFG]** `Physics.cfg` ships the whole model as Hermite spline curves, and they
are applied in a fixed order:

| curve | what it does |
|---|---|
| `DRAG_CD` | *"the final Cd of a given facing is the drag cube Cd evaluated on this curve"* — remaps the baked 0.77 to a realistic 0.54 |
| `DRAG_CD_POWER` | the result is then **raised to a power indexed by Mach**: 1.0 subsonic → 2.5 at Mach 1.1 → 3.0 at Mach 5 |
| `DRAG_TIP` | Mach multiplier for the windward face: 1.0 → **2.83 at Mach 1.1** → 4.0 at Mach 5 |
| `DRAG_TAIL` | Mach multiplier for the leeward face: 1.0 → **0.25 at Mach 1.1** (base drag collapses supersonically) |
| `DRAG_SURFACE` | skin friction: 0.02 subsonic → 0.0025 supersonic |
| `DRAG_MULTIPLIER` | the overall transonic hump: 0.5 → **1.3 at Mach 1.1** → 0.7 at Mach 2 |
| `DRAG_PSEUDOREYNOLDS` | a density-indexed correction, 4.0 in near-vacuum → 1.0 at sea level |
| `LIFTING_SURFACE_CURVES` | four named sets — `Default`, `BodyLift`, `CapsuleBottom`, `SpeedBrake` — each with `lift`, `liftMach`, `drag`, `dragMach` |

with the global scalars

```
dragMultiplier = 8      dragCubeMultiplier = 0.1     angularDragMultiplier = 2
liftMultiplier = 0.036  liftDragMultiplier = 0.015   bodyLiftMultiplier = 18
```

**[inferred]** Three observations. **The transonic drag rise is authored, not
emergent** — `DRAG_MULTIPLIER` peaking at 1.3 at Mach 1.1 and the tip curve
tripling at the same point *are* the sound barrier, and nothing in the model
derives them. That is the right call: the shape of the transonic hump is well
known, cheap to author and ruinously expensive to compute.

**The lift curve is indexed by sin(AoA), not AoA** — the keys are 0, 0.2588
(15°), 0.5 (30°), 0.7071 (45°), 1.0 (90°) — which makes the dot product the
lookup key directly and removes an `asin` from the per-part inner loop. Small,
and exactly the kind of thing a per-part-per-frame path should do.

**And the whole aerodynamic model is data.** No aero code needs to change to
retune the atmosphere, which is why FAR (the realistic-aerodynamics mod) can
replace it and why `Realism Overhaul` ships a `Physics.cfg` patch rather than a
plugin.

### 8.4 Occlusion is where it stops being cheap

**[COMMUNITY]** Stack-mounted parts occlude each other, and there is a per-node
correction to `Cd × A` for occupied attachment nodes.

**[inferred]** This is the necessary companion to drag cubes and the part that
cannot be baked, because it depends on the *assembly*, not the part. Without it a
ten-tank stack would have ten nose cones' worth of tip drag. With it, only the
exposed faces count — which is also why cargo bays and fairings work by
**excluding their contents from the drag pass entirely** rather than by shielding
them, since exclusion is a set membership test and shielding would be a
visibility query.

---

## 9. Thermal, buoyancy, and what else is in that file

**[CFG]** `Physics.cfg` is worth reading end to end; it is the closest thing KSP
has to a design document. Selected, with the reading:

**Thermal** (added 1.0, and a full three-channel model):
```
spaceTemperature = 4                  // cosmic microwave background, in kelvin
solarLuminosityAtHome = 1360          // the real solar constant, W/m²
standardSpecificHeatCapacity = 800
conductionFactor = 120                skinInternalConductionFactor = 0.005
radiationFactor = 1                   machTemperatureScalar = 21
thermalIntegrationMinStep = 0.014     thermalIntegrationAlwaysRK2 = False
thermalIntegrationHighMaxPasses = 10  thermalIntegrationHighMinPasses = 1
thermalConvergenceFactor = 0.63       thermalMaxIntegrationWarp = 100
analyticLerpRateSkin = 0.003          analyticLerpRateInternal = 0.001
```

**[inferred]** Four things worth stealing from those twelve lines. The
**integrator is adaptive** — `AlwaysRK2 = False` with a min step and a pass count
between 1 and 10 means it runs Euler when the gradient is gentle and escalates to
RK2 with sub-stepping when it is not, which is the correct shape for a system
that is boring in orbit and violent on re-entry. **`thermalMaxIntegrationWarp =
100`** says the full integration is abandoned above 100× warp and replaced by the
`analyticLerp*` path — *a second, cheaper model that takes over when the
expensive one cannot keep up*, which is the same two-tier structure as §1 in
miniature. And `solarLuminosityAtHome = 1360` is the real value, so Kerbin's
insolation is Earth's despite Kerbin being a tenth the size — **the star was
tuned to the gameplay and the physics constant was left alone.**

**Kerbal G-force blackout**, which is a whole subsystem in nine lines:
```
kerbalGOffset = 900   kerbalGPower = 4   kerbalGDecayPower = 2   kerbalGClamp = 20
kerbalGThresholdWarn = 30000   kerbalGThresholdLOC = 60000
kerbalGLOCBaseTime = 3   kerbalGLOCTimeMult = 0.0001   kerbalGBraveMult = 1.5
```
**[inferred]** G exposure is integrated as `g^4` against two thresholds — so it
is *dose*, not instantaneous load, and the fourth power means 10 g for one second
is far worse than 5 g for two. `kerbalGBraveMult` scales it by the kerbal's
courage stat, which is how a personality trait becomes a physical constant.

**Re-entry blackout**, which correctly costs you something:
```
commNetQTimesVelForBlackoutMin = 500    commNetTempForBlackout = 1100
commNetDensityForBlackout = 5E-05       commNetDotForBlackoutMin = -0.866
```
**[inferred]** The `Dot` terms (-0.866 = cos 150°, -0.5 = cos 120°) mean the
blackout is **directional** — the plasma sheath is in front of you, so an antenna
pointing back up your flight path is shadowed and one pointing sideways may not
be. That is one dot product buying a real physical behaviour, and it is the same
trick as Nuclear Option's exhaust-loudness dot in
[`nuclear_option_audio.md`](../flight/nuclear_option/nuclear_option_audio.md) §3.

**Buoyancy** gets ~35 constants, more than the thermal model. **[inferred]** That
is out of proportion to how much of KSP happens in water, and reads as the
signature of a system that was retrofitted and then repeatedly patched against
specific reported failures rather than derived once.

---

## 10. The ball, and the node

### 10.1 The navball is not a picture, it is a basis viewer

**[inferred]** The navball is a textured sphere showing the vessel's attitude:
blue up, brown down, a pitch ladder and a heading scale, with the vessel's nose
fixed at the centre of the screen and the ball rotating underneath it. Overlaid
are markers — prograde, retrograde, normal, anti-normal, radial in/out, target,
manoeuvre — each of which is a **unit vector rendered at its own direction on the
ball's surface**, and clipped or dimmed when it goes round the back.

The reason this instrument works, and the reason it is the one thing every KSP
successor copies, is that **spaceflight is entirely a problem of expressing one
direction in a different basis than the one you are looking at.** You want to
burn along the velocity vector; you are pointing along the vessel's nose; those
live in different frames, and the pilot's whole job is to bring them together.
The navball is a widget whose only content is *the transform between those
frames*, and every marker on it is a single `Quaternion.Inverse(vesselRotation) *
someWorldVector`.

Real spacecraft carry an FDAI — an eight-ball — for exactly this reason. KSP's
contribution is not inventing it; it is **making the frame selectable**.

### 10.2 Three modes are three reference frames

**[COMMUNITY]** The ball has **Orbit**, **Surface** and **Target** modes, and
what changes is which velocity the prograde marker points along:

- **Orbit** — velocity in the inertial body-centred frame. What the `Orbit` class
  means by velocity, and the only one that makes a manoeuvre node meaningful.
- **Surface** — velocity relative to the *rotating* surface. Different from
  orbital by 175 m/s eastward at Kerbin's equator. This is the frame from §3.2's
  inverse rotation, and it is the one that matters for landing and for aircraft.
- **Target** — velocity **relative to another vessel**, i.e. `v_self − v_target`.

**[inferred]** The mode switch is not a display preference, it is a **change of
reference frame applied to the whole instrument**, and the three modes correspond
exactly to the three frames the simulation already maintains — inertial,
body-rotating, and relative. There is no fourth mode because there is no fourth
frame. And crucially, in Target mode "prograde" means *closing along the relative
velocity*, so **the docking problem becomes the same problem as the burn problem**
and the pilot's learned skill transfers with no new UI. A single instrument, three
bases, and rendezvous is taught for free.

### 10.3 The manoeuvre node lives in an orbital basis

**[API]** This is the detail that makes the planner work, and it is one comment:

> "**Maneuver nodes use a special coordinate system for delta-V.** The
> x-component of `DeltaV` represents the delta-V in the **radial-plus**
> direction. The y-component represents the delta-V in the **normal-minus**
> direction. The z-component represents the delta-V in the **prograde**
> direction."

**[inferred]** The burn is stored in the *orbit's* basis, not in world space and
not in the vessel's frame. Three consequences, and each one is a bug that does not
happen:

1. **The node is invariant under everything that is not the burn.** Rotate the
   ship, warp forward, move the camera, cross a SOI — the node still means
   "1,024 m/s prograde", because prograde is defined by the orbit and the orbit
   is analytic. A world-space delta-v would need re-deriving every frame and would
   drift with the vessel's own noise.
2. **The gizmo is the basis.** The six drag handles on a node are literally
   ±prograde, ±normal, ±radial. There is nothing to design: the UI affordance is
   the storage format made grabbable, which is why it is discoverable without a
   tutorial.
3. **The planner is the flight code.** `OnGizmoUpdated(Vector3d dV, double ut)`
   feeds `Orbit.UpdateFromStateVectors` at the node's UT, producing the next
   patch through the identical function the vessel will use when it actually
   burns. §1 again: the plan and the flight are one computation.

**[API]** And `GetBurnVector(Orbit currentOrbit)` converts that orbital-basis
delta-v into a world direction *on demand*, given the current orbit — which is
the marker the navball then shows. **[inferred]** Storage in the invariant frame,
conversion at the point of display. That is the correct split and it is the
opposite of what a naive implementation does.

---

## 11. What transfers

Ranked by how directly it applies to this project.

1. **Separate the analytic model from the stepped one, and make the boundary
   explicit.** KSP's on-rails/off-rails split is the reason a solar system costs
   nothing and the reason time warp exists. Anything with a cheap closed-form
   answer far from the camera and an expensive incremental one near it wants this
   shape — and wants to name the two states, the transition functions and the
   distance table, as KSP does (`GoOnRails`, `GoOffRails`, `VesselRanges`).
2. **When a simulation resumes, ramp the inputs and suspend failure checks.**
   Physics easing (§6.5). The transient on resume is guaranteed to exceed the
   steady state, so evaluating a break condition during it is evaluating noise.
3. **Depth, not stiffness, sets the compliance of a constraint chain** (§4.6).
   If a jointed structure is too soft, change the graph. This is the note's
   sharpest structural lesson and it generalises to anything solved iteratively
   in series — including, in this codebase, any future ragdoll or debris chain
   on Jolt.
4. **Let each contributor declare its own bound so the aggregate bound is free**
   (§7.5). `GetVertexMinHeight`/`GetVertexMaxHeight` costs one method per mod and
   buys a conservative bounding shell without evaluating anything. Directly
   applicable to any generator stack, layered cost function or effect chain that
   a culler needs to reason about.
5. **Store in the invariant basis, convert at the point of display** (§10.3).
   Manoeuvre nodes in (radial, normal, prograde). The general rule: pick the frame
   in which the quantity is *constant*, not the frame in which it is drawn.
6. **Bake per-object summaries offline; compute assembly effects at runtime**
   (§8.2–8.4). Drag cubes are 18 floats per part baked from the mesh; occlusion
   is the part that depends on the assembly and therefore cannot be baked. The
   split — *what is a property of the thing* versus *what is a property of the
   arrangement* — is the reusable idea.
7. **Give the estimator an incumbency bias** (§6.6). `orbitDriftFramesToWait = 5`.
   Already in CLAUDE.md for scorers; KSP is the reminder that it applies to
   anything re-derived every frame from noisy inputs, not just to choices.
8. **Two representations with a crossfade beats one representation with a
   compromise** (§7.6). And they must share a generator, or the seam pops.
9. **Escalate cost by stage and count each stage separately** (§2.3). Apsis test,
   then ellipse proximity, then bisection — with `GeoSolverIterations` and
   `TimeSolverIterations` reported apart so the expensive stage is identifiable.
10. **Expose the cost knob when the cost genuinely varies with player intent**
    (§2.3's conic patch limit). Better than silently truncating.
11. **Nail the numerically-sensitive thing to the origin and move the universe**
    (§5.2–5.3). Twice: translation via Krakensbane, rotation via inverse
    rotation. And note the velocity half — a floating origin that does not also
    rebase velocity leaves the cancellation error untouched.
12. **A directional term is often one dot product** (§9's re-entry blackout).

And two anti-patterns:

- **Mass-proportional drag** (§8.1). Not a simplification of the real thing but a
  different phenomenon, and it silently removed the reward for every shape
  decision the player could make. When a cheap model inverts an intended
  incentive, it is not cheap.
- **A fixed stiffness in a game whose subject is variable mass** (§4.4). Any
  constant that must be right across three orders of magnitude of a
  player-controlled quantity is not a constant.

---

## 12. What I could not establish

**This is the largest section relative to the note's length of anything in this
directory, and it should be.** KSP is closed-source, undocumented by its
authors, and not installed on this machine.

- **No decompile.** Everything about `PartJoint`, `FlightIntegrator`,
  `OrbitDriver` and the PQS quadtree's actual subdivision criterion is inferred
  from names, from mod source that touches them, and from behaviour. A single
  afternoon with dnSpy on `Assembly-CSharp.dll` would promote §4 and §7.6 from
  argument to source, and **KSP ships Mono, so this is possible** — the game is
  simply not on this machine. That is the highest-value next step by a wide
  margin.
- **No performance numbers, at all.** Not one frame time, part-count curve,
  solver-iteration figure or PQS build cost appears anywhere in the sources.
  Every cost claim in this note is structural ("this is O(1)"), never measured.
- **The joint solver's actual configuration is unverified.** Whether stock locks
  the linear motions and drives only the angular ones, how many `ConfigurableJoint`s
  a `PartJoint` creates in each attachment case, and what `Physics.defaultSolverIterations`
  KSP sets — all unknown. §4.7 marks the boundary but the boundary should not
  need to exist.
- **The navball's implementation is entirely inferred** (§10.1). No source
  describes how it is rendered. The *behaviour* (three modes, three frames) is
  documented; the mechanism is not.
- **The PQS quadtree itself was not read.** Kopernicus's `pqsmods-standalone` is
  an honest reimplementation of the *modifier* pipeline and says so in its own
  header — *"a fake implementation of the PQS"* — so §7.4 and §7.5 are strong,
  but the subdivision rule, the collider LOD threshold, the quad build budget
  (`maxQuadLengthsPerFrame`) and whether any of it is threaded are not
  established.
- **Resource flow is not covered here at all.** KSP's fuel routing (the crossfeed
  graph, flow priority, the 1.0 rewrite) is a real system with real design
  content and I found no source good enough to write about it honestly.
- **The KSP2 "Wobbly Rockets" dev chat is a video and its transcript could not be
  retrieved.** It is the single most likely place for an authoritative statement
  on §4, and this note quotes it only at second hand.
- **`Physics.cfg` is from a community dump**, not from an install, and its version
  is unknown. The values are consistent with 1.4–1.12 but no claim here should be
  treated as version-precise.
- **Argument from a naming convention is weak evidence.** `SolveSOI_BSP` almost
  certainly means bisection and matches HarvesteR's own description of what he
  wrote, but "almost certainly" is what it is.
