# DCS World — air combat as a set of separately-solved problems

The counterweight to [`nuclear_option.md`](../nuclear_option/nuclear_option.md) and its five companion
notes, and the reason to write it is that **DCS and Nuclear Option disagree about
almost every decision, and both shipped.** Where Nuclear Option makes the
vehicle's *geometry* its physics model and lets everything emerge, DCS builds a
**table of measured coefficients for the whole airframe** and interpolates it.
Where Nuclear Option has one `ControlInputs` struct that the player, the AI and
the autopilot all write, DCS runs **four different flight models at four fidelity
tiers** and the AI does not fly the player's one. Where Nuclear Option models
damage physically because parts really detach, DCS ray-traces the projectile
through a **second, invisible internal geometry** and looks up what it hit.

Neither is the better answer in the abstract. The whole note is an argument that
each choice follows from one number: **how many aircraft must be in the air at
once, and how much does the player's own aircraft have to reward study.**

## 0. How this was sourced, and how far to trust it

This is **not** a decompilation note. Nuclear Option ships Mono and
`Assembly-CSharp.dll` reads back as near-original C#; R.U.S.E. ships its content
pipeline as data. DCS ships **native C++ DLLs with no equivalent**, and the
install is ~300 GB, which is not on this machine. So this note is assembled from
four kinds of evidence and every claim is tagged:

| Tag | What it means |
|---|---|
| **[ED]** | Eagle Dynamics' own published material — the *DCS: Flight Modelling principles* PDF, the *Volumetric Weather* white paper, newsletters, product pages, module manuals. Primary and quotable. |
| **[LUA]** | Read directly out of DCS's shipped Lua database. See below. Primary — this is the game's actual data, not a description of it. |
| **[API]** | The External Flight Model C header, which ED publish inside the install (`API/`) and which is mirrored verbatim in several open-source EFM projects. |
| **[COMMUNITY]** | Forum and third-party reporting. Weakest tier; used only where nothing better exists, and flagged in place. |
| **[inferred]** | My reading of the above. Where I have checked an inference against an independent number, I say so and show the check. |

**On the [LUA] source.** Until DCS 2.7, the weapon, aircraft, sensor and damage
definitions sat in readable `.lua` files under `Scripts/Database`. ED removed
them. A community project (`Quaggles/dcs-lua-datamine`) now dumps the same tables
**out of the running process's Lua state** at launch and commits them per patch;
the tree read for this note is tagged **2.9.28.26283, July 2026**, so it is
current rather than archival. Two consequences matter:

- The dump is of `_G` at runtime, so **comments and functions are gone.** The
  values are real; the *reasons* are not recoverable. Where a column's meaning is
  not self-evident I have had to derive it, and §2.2 shows one such derivation
  being checked against wing geometry.
- The dumped sensor definitions are, in the project's own words, for
  **"AI controlled units, repeat, not player aircraft"** — the player's radar is
  compiled into each module's C++. That split is not an artefact of the dump. It
  is the architecture, and §7 argues it is the single most consequential decision
  in the whole design.

**§13 lists what this note could not establish**, which is a great deal more than
in the Nuclear Option notes. In particular: no PFM internals, no renderer
internals beyond ED's white paper, and no AI decision logic at all.

---

## 1. The architecture is a DLL boundary, and everything follows from it

### 1.1 One C ABI, four fidelity tiers

DCS is not one simulator. It is a host process that owns the world, the terrain,
the network, the rendering and the rigid-body integrator, and a set of aircraft
that plug into it at one of four levels of detail **[ED]**:

| Tier | What it is | Who uses it |
|---|---|---|
| **SFM** — Standard | "A data-driven means of achieving flight dynamics, in conjunction with some scripting." Inherited from *Lock On*. | Originally everything; now legacy AI |
| **AFM / AFM+** — Advanced | "Multiple force application points on flight surfaces", edge-of-envelope behaviour without scripting. AFM+ adds "limited modelling of the hydraulic and fuel systems". | Older player modules |
| **PFM** — Professional | Wind-tunnel and CFD-derived coefficients, landing-gear kinematics with real lengths and arms, servo-piston forces, "realistic simulation of hydraulics, fuel, electrical, engine and other systems". | ED's current player modules |
| **EFM** — External | "Rigid body physics and contact modelling from PFM, but developers independently determine aerodynamic forces and moments." | Third-party modules (Heatblur, Razbam, Polychop…) |
| **GFM** — General | New AI model; PFM's short-period behaviour at SFM's price. §8. | AI, replacing SFM |

The critical word is **External**. DCS's third-party ecosystem exists because ED
drew a C function boundary and published it, and that boundary is the cleanest
single artefact in this note.

### 1.2 The EFM API, which is the whole design in thirty functions

**[API]** The host loads a DLL and calls into it by ordinal name. The complete
shape:

```c
/* --- host pushes state IN, once per step --- */
void ed_fm_set_atmosphere(double h, double t, double a, double ro, double p,
                          double wind_vx, double wind_vy, double wind_vz);
void ed_fm_set_current_mass_state(double mass,
                                  double center_of_mass_x, double y, double z,
                                  double moment_of_inertia_x, double y, double z);
void ed_fm_set_current_state(double ax,double ay,double az,   /* world accel   */
                             double vx,double vy,double vz,   /* world vel     */
                             double px,double py,double pz,   /* world pos     */
                             double omegadotx, ..., double omegax, ...,
                             double quaternion_x, ..., double quaternion_w);
void ed_fm_set_current_state_body_axis(double ax, ...,        /* body frame    */
                                       double wind_vx, ...,
                                       double yaw, double pitch, double roll,
                                       double common_angle_of_attack,
                                       double common_angle_of_slide);
void ed_fm_set_command(int command, float value);             /* pilot input   */

/* --- host asks the DLL to advance --- */
void ed_fm_simulate(double dt);

/* --- host pulls results OUT --- */
void ed_fm_add_local_force (double &x,double &y,double &z,
                            double &pos_x,double &pos_y,double &pos_z);
void ed_fm_add_local_moment(double &x,double &y,double &z);
void ed_fm_add_global_force(...);   void ed_fm_add_global_moment(...);
bool ed_fm_add_local_force_component(...);   /* iterate: return true = more   */
bool ed_fm_add_local_moment_component(...);

/* --- mass bookkeeping, called cyclically after simulate --- */
bool ed_fm_change_mass(double &delta_mass,
                       double &delta_mass_pos_x, double &y, double &z,
                       double &delta_mass_moment_of_inertia_x, ...);
void   ed_fm_set_internal_fuel(double fuel);  double ed_fm_get_internal_fuel();
void   ed_fm_set_external_fuel(int station, double fuel, double x,double y,double z);

/* --- presentation and lifecycle --- */
void   ed_fm_set_draw_args(EdDrawArgument *drawargs, size_t size);
double ed_fm_get_param(unsigned index);
void   ed_fm_configure(const char *cfg_path);
void   ed_fm_cold_start();  void ed_fm_hot_start();  void ed_fm_hot_start_in_air();
```

Five things about this are worth stealing, and one is a warning.

**The DLL does not integrate.** It is handed the current state and returns
*forces and moments*; the host owns the integrator, the quaternion, the contact
solver and the world. This is what makes "rigid body physics and contact
modelling from PFM" true for a third-party module that shares no code with ED's
— **the parts that must agree between aircraft are on the host's side of the
line, and the parts that make an aircraft distinctive are on the module's.** A
module author cannot get the integrator wrong, cannot desynchronise the network,
and cannot invent their own ground contact. That is the correct place to cut.

**Forces come back as (vector, application point), not as a resolved
force-and-torque.** `ed_fm_add_local_force` returns a position alongside the
force. So a module reports "this much thrust, applied here" and the host
computes the pitching moment from the offset. This is exactly the idiom Nuclear
Option uses per part — and it is worth noticing that DCS, which does *not* build
its aerodynamics per part, still exposes the per-part force interface, because
that is how you make thrust-line effects fall out. §2.4's MiG-29 pitch-up is
this, in ED's own words.

**The `_component` variants are an iterator.** `bool ed_fm_add_local_force_
component(...)` returns `true` while there are more forces to collect. So a
module with fifty force contributions does not have to sum them itself, and the
host can attribute each one. Cheap, and it means the host can debug-draw a
module's force breakdown without the module cooperating.

**Body axes are X-forward, Y-up, Z-right** **[API]** — which is *not* the
aerodynamics convention (Y-right, Z-down), and the open-source EFM projects all
carry a comment warning you to "switch the Y and the Z and reverse the Y prior to
output" when transcribing from a textbook or a wind-tunnel report. A published
API that silently disagrees with its own field's convention taxes every single
consumer forever. **If you publish a physics ABI, match the discipline's
convention or state the transform in the header, in a comment, at the top.**

**The warning:** the public EFM API historically had **no ground-reaction or
weight-on-wheels access**, so community EFM aircraft were advised to start
airborne **[COMMUNITY]**. A boundary drawn to protect the host's contact solver
also denied modules the information they needed to *use* it. Boundaries leak in
whichever direction you did not design for.

### 1.3 The consequence: the same aircraft exists twice

Because the tiers are real, an F-16 in DCS is **two different objects depending
on who is flying it**. As a player module it is a PFM plus a compiled systems
model; as an AI it is the `SFM_Data` table in §2.2 and a sensor definition from
§7. Its RCS, its damage cells and its loadout drag are shared; its aerodynamics,
its radar and its flight control laws are not.

This is the fork in the road. Nuclear Option's [§7 of the combat
note](../nuclear_option/nuclear_option_combat.md) makes a virtue of the opposite — *"the AI is not
privileged; it writes the same `ControlInputs` struct the player's stick does"* —
and gets, for free, an AI that must solve turn radius and energy honestly, plus
a flight-model fix that improves the AI in the same commit. DCS gets, for free,
**a hundred aircraft in a mission**. §11 is the argument that this is the only
real decision either game made and everything else is downstream of it.

---

## 2. Fixed-wing: the aircraft is a table of coefficients

### 2.1 ED's stated method, which is a virtual wind tunnel

This is the passage that defines the whole approach **[ED]**, from *DCS: Flight
Modelling (FM) principles*:

> The forces are obtained in different ways: a) using physical wind tunnel data
> b) experimental flight test data and c) computational flow dynamics (CFD)
> methods. The forces are measured or calculated and then CL and CD are
> calculated.
>
> CL and CD are functions of angle of attack (AoA), but it is convenient to have
> them in the form of CL = f (AoA) and CD = g (CL). The last one is known as
> Lift/Drag polar plot.
>
> **When designing our FM it is necessary to place our aircraft mathematical
> model into a virtual wind tunnel, obtain the force coefficients and match them
> to the documented reference coefficients.** At this stage, no airspeed is used
> at all in the process except dimensionless Mach number.

And the moments, same document:

```
m_pitch = M_pitch / q / S / MAC        (vs AoA or vs CL)
m_yaw   = M_yaw   / q / S / Wingspan   (usually vs angle of sideslip)
m_roll  = M_roll  / q / S / Wingspan   (usually vs angle of sideslip)
```

Read that against Nuclear Option §2, where **the vehicle's geometry *is* its
physics model** — parts carry an area and an airfoil, flow is resolved in each
part's own transform, and *"sideslip needs no term because a fin is a part rolled
90° and the same `atan2` is its slip angle"*.

DCS does the reverse. **Sideslip is a term**, because `m_yaw` and `m_roll` are
tabulated *against* sideslip angle. There is no fin whose orientation produces
directional stability; there is a measured curve that says what the aircraft
does at 5° of slip, and the model interpolates it. The same holds for every
coupling: it exists if and only if somebody measured it and put it in a table.

The trade is stark and worth stating plainly:

| | Nuclear Option (geometry) | DCS (coefficients) |
|---|---|---|
| Novel configuration (VTOL, canard, asymmetric) | works automatically | needs new data |
| Damage that changes the shape | **automatic** — the part is gone | needs a modelled correction (§5.6) |
| Matching a real aircraft's charts | approximate, tuned by hand | **exact, by construction** |
| Cost per aircraft to author | a parts list | wind tunnel / CFD / flight test |
| Cost per aircraft per tick | N parts × resolve | one table lookup chain |
| Behaviour outside the tabulated envelope | still physical | undefined; extrapolated |

ED are explicit that the authoring cost is the binding constraint. The GFM
newsletter says AFM/PFM "require substantial work to create each individual
aircraft flight data model" **[ED]** — which is precisely why they could not put
PFM on the AI, and why §8 exists.

**The P-47 story in the same document is the honest version of this cost** **[ED]**.
Wind-tunnel data for the aircraft "is not very detailed, so we had to add CFD
measurements". They ran a half-model for the symmetric cases to save time and the
full model for deflected ailerons, and note that the 3D visual model — built from
factory drawings with ordinate tables "up to 0.001 inch" — "became a good base
for various CFD tests". Then: *after* the FM was finished, they obtained real
5-foot wind-tunnel data, "surprisingly, a Soviet report based on a translation of
an American report", and compared. **The art asset became the aerodynamic mesh,
and the validation arrived after the fact.** That is a real pipeline, not a
tidy one.

### 2.2 What the AI's table actually contains, read out of the game

**[LUA]** The F-16C's `SFM_Data`, in full:

```lua
SFM_Data = {
  aerodynamics = {
    Cy0 = 0, Czbe = -0.016, Mzalfa = 4.355, Mzalfadt = 0.8,
    cx_brk = 0.06,    -- speedbrake drag increment
    cx_flap = 0.05,   -- flap drag increment
    cx_gear = 0.0268, -- undercarriage drag increment
    cy_flap = 0.52,   -- flap lift increment
    kjx = 2.75, kjz = 0.00125,
    table_data = {
      -- { M,    Cx0,    ?,     ?,      ?,     ?,    ?,    ?   }
      { 0,    0.0165, 0.07,  0.132,  0.025, 0.5,  30,   1.45 },
      { 0.9,  0.0542, 0.083, 0.1327, 0.042, 3.5,  26,   1.3  },
      { 1,    0.0707, 0.085, 0.1634, 0.1,   3.5,  24.7, 1.12 },
      { 2,    0.0471, 0.039, 0.222,  2.5,   0.78, 10.2, 0.52 },
      { 3.9,  0.035,  0.033, 0.35,   6,     0.7,  9,    0.4  },
      -- 20 rows total
    }
  },
  engine = {
    MinRUD = 0, MaksRUD = 0.85, ForsRUD = 0.91, MaxRUD = 1,   -- throttle detents
    Nmg = 67.5, Nominal_RPM = 14710, Nominal_Fan_RPM = 8215,
    Startup_Duration = 35, Shutdown_Duration = 19,
    dcx_eng = 0.0144, hMaxEng = 19, cefor = 2.56, cemax = 1.24,
    table_data = {           -- { Mach, mil thrust N, AB thrust N }
      { 0,   77000, 108313.6 }, { 1,   109000, 218000 },
      { 1.4, 55000, 230000  }, { 3.9,  25000, 120636 },
    },
    type = "TurboFan"
  }
}
```

The dump has no comments, so the eight columns had to be identified. Six are
unambiguous from their behaviour across Mach; **column 4 can be checked against
the aircraft's own geometry, which is worth showing because it converts a guess
into a fact:**

- **Column 4** behaves like the induced-drag factor *B* in `Cx = Cx0 + B·Cy²`. At
  M = 0 it is **0.132**. The F-16C's table gives `wing_span = 9.45` and
  `wing_area = 28`, so aspect ratio **AR = 9.45² / 28 = 3.19**, and the classical
  induced-drag factor is `1/(π·AR·e)` = `1/(π × 3.19 × e)`. With a
  span efficiency `e = 0.76` that is **0.131**. Matching to three decimal places
  on a number derived from two unrelated fields in the same table is not a
  coincidence — **column 4 is the induced-drag factor.** [inferred, verified]
- **Column 2** is `Cx0`: 0.0165 subsonic, peaking **0.0707 at M 1.0**, falling
  after. That is the transonic drag rise, in the right place.
- **Column 3** is the lift-curve slope `Cya`: 0.07 per degree = 4.0 per radian,
  reasonable for a low-AR wing with LERX; it peaks near M 1 and collapses to 0.033.
- **Column 6** rises 0.5 → 3.5 and falls again: **maximum roll rate in rad/s**
  (3.5 rad/s ≈ 200°/s). It is speed-limited at the bottom and
  authority/structurally-limited at the top, which is the correct shape.
- **Column 7** falls 30° → 9°: **maximum permissible AoA**, tightening with Mach.
- **Column 8** falls 1.45 → 0.4: **Cy_max**.
- **Column 5** rises 0.025 → 6 and is not identified. [see §13]

**This is the entire aerodynamic model of an AI F-16: 20 rows of 8 numbers, plus
nine scalars.** 169 numbers. Compare Nuclear Option, where an aircraft is a parts
hierarchy with an airfoil per part and the moments emerge from the transforms.

Two design points fall out of the scalars:

- `cx_brk`, `cx_flap`, `cy_flap`, `cx_gear` are **additive increments**, not
  separate tables. Configuration change is a delta on the polar. Cheap, and it
  means you can author a new configuration without a new sweep.
- **Stores drag is per-store data on the pylon.** Each launcher entry carries
  `Cx_gain = 0.796` **[LUA]**, so a loadout's drag penalty is a multiplier
  attached to the thing being carried, not a property of the aircraft. Adding a
  weapon to the game gives every carrier the correct drag automatically.

### 2.3 The engine: two models for two tiers

The SFM engine is a **thrust table indexed by Mach**, with an altitude
correction (`dpdh_m`, `dpdh_f` — the pressure-altitude gradients for military and
full AB — and `hMaxEng = 19` km). ED say directly that where a thrust chart
exists, "there is no problem to convert TAS to Mach number for each altitude and
then use it directly for SFM, **or to use it as a reference for our
thermodynamic model for PFM**" **[ED]**.

So: **SFM interpolates a measured thrust surface; PFM runs a thermodynamic cycle
and the measured surface is its validation target.** That is the tier system in
one sentence, and it recurs everywhere — the high tier simulates the mechanism,
the low tier interpolates the mechanism's published output, and the published
output is the contract between them.

The MiG-29 verification section makes the depth of the PFM engine model concrete
**[ED]**: the RD-33 is modelled with three operating modes (Low Power, Normal
Power, **Increased Temperature Mode**), DCS permanently selects ITM "as DCS is a
combat simulator where engine lifetime does not matter", and the reason ITM
exists at all is that at high Mach "rammed air in front of the compressor has
high temperature", so turbine inlet temperature forces a **fuel-flow limit** to
protect the blades. A thrust table cannot produce that. A cycle model produces it
without being asked.

### 2.4 Trim, thrust line, and the thing tables cannot fake

ED's own MiG-29 verification chart is captioned: *"MiG-29 has thrust vector
applied below CoG causing very pronounced pitch-up effect especially at low IAS
and low altitudes"* **[ED]**.

That is not in any aerodynamic table. It exists because the PFM applies thrust at
a point — the same `(force, position)` idiom the EFM API exposes in §1.2 — and
the moment arm does the rest. **The coefficient tables describe the air; the
force-application points describe the machine.** DCS is a table-driven model
*wrapped in* a per-point force model, and the interesting behaviours live in the
wrapper.

---

## 3. Player control: there is no generic aircraft

### 3.1 Inputs are abstract commands; the aircraft decides what they mean

**[API]** The host gives the module `ed_fm_set_command(int command, float value)`
and nothing else. Commands are integers from DCS's global `iCommand` enumeration
(open-source EFM templates handle e.g. 2001 pitch, 2002 roll, 2004 throttle).

This is structurally the same idea as Nuclear Option's `ControlInputs` — **one
narrow interface between "what the human asked for" and "what the aircraft does
about it"** — and it buys the same things: a stick, a keyboard, an autopilot and
a network-replicated remote player all arrive through one door.

The difference is what sits behind the door. In Nuclear Option there is *one*
fly-by-wire implementation and every aircraft goes through it. In DCS there is
**no shared flight control system at all** — each module implements its aircraft's
real one.

### 3.2 The FCS is the module, and that is the product

For the F-16C, DCS models the FLCS as a **G-command system**: the pilot's stick
does not command elevator, it commands load factor, and the aircraft delivers it
"within the limits allowed and at the rate permitted", between −3 G and +9 G
**[COMMUNITY]**. The limiter's real subtlety is reproduced too — *the g/AoA
limiter does not limit g or AoA, it limits the g **command***, and past 25° AoA
the FLCS commands full nose-down.

This is why DCS aircraft feel categorically unlike each other in a way that a
game with one FBW layer cannot reproduce: the F-16's stick means "G", the Su-27's
means something else, and a Bf-109's means "cable tension, and good luck above
400 km/h". **The control law is the character of the aircraft**, and DCS spends
its per-module budget there.

The cost is the obvious one. Every module reimplements this, there is no shared
correctness, and a bug in one aircraft's control laws is invisible to every
other. Nuclear Option's single FBW is worse per aircraft and better per bug.

### 3.3 What DCS does not do, and Nuclear Option does

Nuclear Option's control note has a detail worth naming here as an absence: the
throttle **detects absolute versus relative hardware from the axis delta**
(`delta < 0.5` → snap to a real lever, else treat as a direction and ramp at 1/s),
so a HOTAS and a keyboard are both correct with no setting. DCS instead exposes
axis tuning to the user — curvature, saturation, deadzone, per-axis, per-module —
and asks them to configure it.

**That is the same problem solved at opposite ends: infer the hardware, or expose
the knobs.** For a simulator whose users own hardware and enjoy configuring it,
exposing the knobs is defensible. For anything else, it is a settings screen that
exists because a heuristic was not written.

---

## 4. Helicopters: the one place DCS simulates the mechanism

Rotary wing is where DCS stops interpolating and starts integrating, and ED's own
description of the Ka-50 is unusually specific **[ED]**:

> The rotor model is based on a **joint model of each blade** with its own complex
> motion relative to rotor axis and **flapping (horizontal) and hunting (vertical)
> hinges**. Each blade is **separated into multiple segments**, each having its own
> air velocity vector based on its orientation, twist, and **induced velocity at
> current rotor section**. Induced velocity is calculated by solving the equations
> based on **simultaneous application of motion quantity theorem and blade element
> method**.

"Motion quantity theorem" is a translation of the momentum theorem, so that last
sentence is **blade element momentum theory (BEMT)** stated exactly: solve the
induced velocity at each annulus by equating the momentum-theory and
blade-element expressions for thrust, then use it to get each segment's local
inflow. This is the textbook method, implemented rather than approximated.

ED then list what falls out rather than being coded **[ED]**:

> conical rotor inclination in forward flight (oscillations in hover with fixed
> stick, cyclic stick input increasing accordingly to the airspeed), power excess
> after transition from hover to forward flight, ground effect (over inclined
> surface or close to ground objects), «vortex ring» phenomena, airflow stall
> from the blades, **blades intersection (collision)**.

### 4.1 Read against Nuclear Option's rotor

Nuclear Option §7 is also a genuine blade-element rotor, and the two are close
enough that the differences are informative:

| | Nuclear Option | DCS Ka-50 |
|---|---|---|
| Blade discretisation | segments along span | segments along span |
| Flapping hinge | yes — β̈ = M/I against an asymmetric droop-stop spring | yes |
| **Lead-lag (hunting) hinge** | **no** | **yes** |
| Induced velocity | modelled | **solved by BEMT each annulus** |
| Azimuthal integration | **4× sub-stepping** (280 rpm turns 34°/tick) | not stated |
| Cyclic | projection of blade azimuth onto a tilted swashplate transform | swashplate tilt |
| Autorotation | emergent from `ω += (Q_eng + Q_aero − friction)·dt / I` | emergent |
| Rotor–fuselage/blade collision | not modelled | **modelled** |
| Coaxial | n/a | modelled, including the two rotors' mutual inflow |

The **hunting hinge** is the substantive addition, and it is not decoration:
lead-lag is what makes ground resonance and blade-tracking behaviour possible,
and on a coaxial it is part of why blade intersection can happen at all. The
manual is explicit that this is a *modelled failure mode*, describing how a
too-abrupt recovery from a dive can produce "the collision of the tail boom and
rotor blades… as a result of their conflicting movements" **[ED]** — on a
single-rotor helicopter. On the Ka-50 the same physics puts the upper and lower
discs into each other.

**Both games arrived at blade-element rotors independently**, which is the same
convergence [`broken_arrow.md`](../broken_arrow/broken_arrow.md) §10 observes for spatial
structures: when the problem is forced, the shape is forced. You cannot get
retreating blade stall, translational lift, VRS and autorotation out of a thrust
model, and every one of them is a thing the player must feel. So everyone who
takes helicopters seriously writes the same thing.

### 4.2 Vortex ring state, and the useful observation about emergent physics

ED's manual describes VRS physically **[ED]** — "the descent is so rapid that
induced flow at the inner portion of the blades is upward rather than downward…
the upward flow caused by the descent can overcome the downward flow produced by
blade rotation" — and then gives the *numbers* that fall out of their own model
as flight limits: below 50 km/h IAS, keep sink rate under 5 m/s above 200 m and
3 m/s below it; recovery is "rapidly decrease collective about 1/3 of total
range while pushing the cyclic to −20 to −25°".

That is worth pausing on. **The manual's emergency procedure is a description of
the simulation's behaviour, not a script the simulation runs.** Nuclear Option's
command note makes the identical point from the other side: its helicopter
autopilot reads `GetVRSFactor()` and rotor RPM droop *back out of* the
blade-element model and applies the real recovery technique — the AI as a
consumer of emergent physics rather than a parallel model of it.

**When the physics is real, the documentation, the AI and the player's technique
are all readers of one system.** When it is scripted, each is a separate
implementation that can disagree. This is the strongest argument in the note for
paying for a real rotor.

### 4.3 The AI helicopter is still a table

**[LUA]** The Ka-50's shared unit definition carries the *AI's* helicopter model,
and it is as coarse as the fixed-wing one: `blade_area`, `blade_chord`,
`blades_number`, `rotor_MOI`, `rotor_RPM`, `rotor_diameter`, `rotor_pos`,
`tail_fin_area`, `tail_stab_area`, `H_stat_max` / `H_stat_max_L` (hover ceiling
out of and in ground effect), `H_din_one_eng` / `H_din_two_eng`, and —

```lua
fuselage_Cxa0  = ...   -- drag coefficient at zero incidence
fuselage_Cxa90 = ...   -- drag coefficient at 90°
```

— **anisotropic fuselage drag from two numbers**, which is the same trick Nuclear
Option uses for ship hulls (lateral drag ≫ longitudinal; *that is the keel*).
Two coefficients and a blend is enough to make a body that does not want to fly
sideways, in any medium, at any fidelity. It is the cheapest genuinely useful
aerodynamic term there is.

---

## 5. The damage model, which is two damage models

This is the part the question was really about, and DCS has **two of them shipped
side by side**: the legacy cell model on most aircraft, and the new
component-and-ray model on the WWII set. Both are visible in the data, and
comparing them is the clearest possible illustration of what the new one buys.

### 5.1 The legacy model: cells, hit points, and a dependency graph

**[LUA]** The F-16C's entire damage model:

```lua
Damage = {
  [0]  = { args = { 146 }, critical_damage = 3 },
  [23] = { args = { 223 }, critical_damage = 2, deps_cells = { 33 } },
  [29] = { args = { 224 }, critical_damage = 4, deps_cells = { 23, 33, 25 } },
  [35] = { args = { 225 }, critical_damage = 5, deps_cells = { 29, 23, 33, 25 } },
  [36] = { args = { 215 }, critical_damage = 5, deps_cells = { 30, 24, 34, 26 } },
  -- 38 cells
},
DamageParts = { "f-16c_bl50_oblomok_wing_R", "f-16c_bl50_oblomok_wing_L" },
```

Three fields carry the whole thing:

- **`critical_damage`** is hit points for that cell. Values run 1–6.
- **`args`** are model draw-argument indices — the visual damage state is driven
  by the same animation-argument channel as flaps and gear. Damage is *animation*.
- **`deps_cells`** is the structural cascade, and it is the good idea here. Cells
  23 → 29 → 35 with each depending on all the previous ones is **an outboard wing
  chain**: destroying an inboard cell destroys everything outboard of it, because
  the list of dependencies grows as you go out the span. The engineer's
  "everything distal to the break comes off" is expressed as a static list, with
  no geometry query at runtime.
- **`DamageParts`** names the detached-debris meshes — here, exactly two: a left
  and a right wing stub.

Note what is *absent*: no materials, no thickness, no internal components, no
projectile path. A hit deposits damage into whichever cell it landed on; the cell
either survives or fails; failure cascades along an authored list. **It is a hit
box with hit points and a parent pointer.**

### 5.2 The new model: an X-ray mesh, materials and wall thicknesses

**[LUA]** The Bf-109K-4, same field name, entirely different content:

```lua
XRayShape = "Bf-109K-4_X-Ray",     -- a SECOND, invisible geometry

Damage = {
  {   -- forward fuselage, left
    args = { 150 }, children = { 59 },
    construction = { durability = 1.137, refractory = false, skin = "Aluminum" },
    critical_damage = 3, damage_boundary = 0.01, droppable = false,
    innards = {
      { id = "XEng0Supercharger", skin = "Aluminum", wall = 0.004,
        failures = { { "ENG0_WASTEGATE_OIL_FEED_CLOGGED", 0.25 } } },
      { id = "XEng0WaterTank0",  skin = "Aluminum", wall = 0.004 },
      { id = "XEng0WaterHose0",  skin = "Rubber",   wall = 0.01, plenum = 0.005 },
      { id = "XEng0Magneto0",    skin = "Plastic",  wall = 0.01 },
      { id = "XEng0OilPump",     skin = "Steel",    wall = 0.005 },
    }
  },
  {   -- canopy
    args = { -1 },
    construction = { durability = 0.485, skin = "Glass", spar = "Truss" },
    critical_damage = 1,
    detachable = { shape = "Bf-109K-4_fonar" },
    innards = { { id = "XArmor03", skin = "ReinforcedGlass", wall = 0.06 } }
  },
  -- 45 cells
}
```

The vocabulary, counted across the file:

| Field | What it is |
|---|---|
| `XRayShape` | **a separate mesh the projectile is traced through**, distinct from the visual and collision meshes |
| `construction.skin` | material of the cell's outer surface — 9 in use: `Steel` (60), `Aluminum` (47), `Rubber` (8), `Fabric` (5), `Plastic` (5), `Glass` (4), `CastIron` (4), `ReinforcedGlass` (2), `Plywood` (1) |
| `construction.spar` | load path type — `FlangeBeam` (12), `Truss` (6), `Rod` (4), `HeavySprocket` (2) |
| `construction.durability` | structural strength, ~0.49–10 |
| `construction.refractory` | whether the cell resists fire |
| `innards[].wall` | **wall thickness in metres** — 0.0002 for a brake line, 0.06 for armour glass |
| `innards[].plenum` | **contained fluid volume** — 0.0002 (brake line) to 0.75 (oxygen hose) |
| `innards[].failures` | `{ FAILURE_ID, probability }` |
| `children` | structural cascade (the new `deps_cells`) |
| `detachable` | the part separates, with its own mesh |
| `damage_boundary` | damage threshold before the cell registers anything |

There are **~150 named internal components** in one aircraft, and the naming is
systematic enough to read as a parts list: `XSparWLHIn` / `XSparWLHMid` /
`XSparWLHOut` (left wing spar, inboard/mid/outboard), `XCtrlLineAileronLH`,
`XHydroHoseWBrakeRH`, `XEng0Magneto0`/`1`, `XOxygenHose`, `XCrew0`, `XCrew0H`
(the pilot, and the pilot's head).

**The wing spar is three separate severable objects.** That is the entire answer
to "does losing a wing spar make the wing fold at high G" — the spar is a thing
with a position, a material and a wall thickness, a bullet either cuts it or does
not, and the structural model reads its state.

### 5.3 How a hit is resolved

**[ED]**, from the Damage Model Development Report, plus the data above:

> Accurate damage and destruction of aircraft systems is based on **where bullets
> hit the target and where the bullet went next**… the system precisely calculates
> hits to these internal aircraft systems **as the projectile passes through the
> aircraft**. The predicted damage depends on the type of munition, munition
> velocity dependent on distance, and location of impact.

So the sequence is [ED for the mechanism, inferred for the ordering]:

1. The round strikes the outer surface; the **cell's `skin` material and the
   `construction`** decide whether it penetrates and what it costs to do so.
2. The round **continues along its path through `XRayShape`**, which is a
   geometry containing the `innards` as solids.
3. Each intersected component's own `skin` + `wall` decides penetration again,
   and the round loses energy each time.
4. A penetrated component rolls its `failures` list; `{"ENG0_WASTEGATE_OIL_FEED_
   CLOGGED", 0.25}` fires that failure 25% of the time.
5. A component with a `plenum` **leaks that volume** — which is why hydraulic,
   oil, coolant, fuel and oxygen lines all carry one, and why the observable
   effects ED list are colour-coded vapour trails: reddish/white for hydraulic,
   brown haze for oil, white steam for coolant, white vapour for fuel **[ED]**.

**The critical structural idea is step 2.** DCS does not ask "which hitbox did
this hit"; it asks "what is the ordered list of things this round passed
through". That converts armour, component shadowing and spall-path behaviour from
special cases into consequences of geometry — a plate protects whatever is behind
it because it is *in front of it*, not because a rule says so.

Nuclear Option's damage note reaches the same structural conclusion from a much
cheaper direction: **§3.3's blast shadowing "in fifteen lines" — cast at each
candidate, damage whatever you hit *first*.** Same principle (occlusion is
resolved by a trace, not a table), one dimension down. **If you take one idea
from this section, take that one: resolve protection by tracing, not by
enumerating.**

### 5.4 The failure ID is a shared channel, and this is the best idea here

**[LUA]** The Bf-109K-4 also has a top-level `Failures` table, 34 KB of entries
like:

```lua
{ id = "CTRL_AILERON_ROD_DESTROYED", label = "Aileron control failure",
  enable = false, prob = 100, mint = 1, hh = 0, mm = 0, hidden = false },
{ id = "FUEL_TANK_00_MAJOR_LEAK",  label = "...", ... },
{ id = "ELEC_GENERATOR_FAILURE",   label = "...", ... },
```

`enable`/`prob`/`hh`/`mm`/`mint` are the **mission editor's random-failure
scheduler**: enable a failure, give it a probability and a mean time, and it
fires on a timer.

And these are **the same IDs the `innards[].failures` lists emit when a bullet
cuts something.** `CTRL_AILERON_ROD_DESTROYED` can arrive because a cannon shell
severed `XCtrlLineAileronLH`, or because the mission designer scheduled it at
T+12 minutes, and the systems model cannot tell the difference and does not need
to.

**That is one integration channel with two producers and one consumer.** The
damage model does not know how flight controls work; the systems model does not
know about ballistics; the mission editor knows about neither. They agree on a
string. Adding a new failure mode is a new ID and a consumer, and it is
immediately available to both producers.

This is the direct counterpart to [`broken_arrow_damage.md`](../broken_arrow/broken_arrow_damage.md)
§5's **five-effect critical table** (mobility / optics / targeting / fuel /
loading, probabilistic, slotted, networkable as an enum) — and the comparison is
exactly the count-versus-depth trade that note names. Broken Arrow has five
effects because it has a thousand units. DCS has several hundred IDs per aircraft
because it has one.

### 5.5 The crew are components

`XCrew0` and `XCrew0H` are `innards` like any other, with a material and a wall
thickness, sitting behind `XArmor00`–`XArmor03`. A pilot kill is not a special
case; it is a component with a low durability that happens to be behind armour
plate whose `wall` is 0.06 m of `ReinforcedGlass`.

**Armour in this model is not a damage-reduction statistic. It is an object in
the way.** Its value is entirely a function of what is behind it and what angle
you shot from, and neither of those is authored.

### 5.6 What damage does to the flight model

**[ED]** — the report is explicit and this is the hard part:

> The new system manages to calculate **loss of lift due to damage to the wing
> skin** and **strength reduction from a damaged spar that may lead to a wing snap
> at higher G loads**.

And for the Ka-50 **[ED]**:

> The damage model is based on aerodynamic and rigid contact forces where
> applicable… Any damage affects the helicopter's physical and functional
> properties **and reposition the CG**.

Here the coefficient-table architecture is paying its bill. Nuclear Option gets
this free — a wing part is deleted, so it stops producing force, and the CG moves
because the mass is gone. DCS has **one table for the whole airframe**, so
"damaged wing skin" has to be a modelled *correction* applied to that table:
reduce lift by some function of the skin damage, reduce the G limit by a function
of spar state, shift the CG by the mass lost.

ED are also candid about the ceiling **[ED]**: *"current computers can't
dynamically bend construction and tear fuselage and wings"*, so visual damage is
**four progressive texture levels (0–3)** plus detachable parts. The damage is
simulated in detail and *rendered* as a state machine.

**This is the price of §2's choice, paid here.** A per-part aerodynamic model
makes damage-to-aerodynamics free and matching a real aircraft's charts hard. A
coefficient model makes matching the charts exact and damage-to-aerodynamics a
correction term you have to write, per effect, by hand. There is no third option
that gets both cheaply.

### 5.7 The one implementation detail worth stealing outright

**[ED]** *"The damage system doesn't register any damage for the first 30 seconds
from mission start."*

A blanket invulnerability window to absorb spawn-time overlaps, physics settling,
carrier deck collisions and mission-start jitter. It is inelegant and it is
obviously correct, and every game with a spawn has needed it.

---

## 6. Weapons: a 6-DOF missile with a PID autopilot, and real exterior ballistics

### 6.1 The missile is a small aircraft with its own coefficient tables

**[LUA]** The AIM-120C's aerodynamic block, complete:

```lua
fm = {
  Cx0 = { 0.468, 0.468, 0.468, 0.468, 0.479, 0.751, 0.88, 0.857, ... 0.364 },  -- 26
  CxB = { 0.021, ... 0.0286 },      -- base drag, 26
  Cya = { 0.318, ... 0.421 },       -- side/normal force slope, 26
  Cza = { 0.318, ... 0.421 },       -- 26
  Mya = { -0.712, ... -0.149 },     -- yaw moment vs incidence, 26
  Myw = { -8.808, ... -7.968 },     -- yaw damping, 26
  Mza = { ... }, Mzw = { ... },     -- pitch equivalents
  Mxd = 5.73,   Mxw = -15.8,        -- roll control power / roll damping
  K1  = { ... }, K2 = { ... },      -- 26 each
  A1trim = { 28, ... 36 }, A2trim = { ... },
  Ix = 1.04, Iy = 125.32, Iz = 125.32,   -- inertia tensor
  S = 0.0248, L = 0.178, caliber = 0.178, mass = 161.48,
  delta_max = 0.34906585,           -- 20° fin deflection
  table_scale = 0.2,                -- Mach step
  table_degree_values = 1,          -- angles in degrees
  model_roll = 0.78539816,          -- 45° — X fin configuration
  fins_stall = 1,
}
```

**26 entries × `table_scale = 0.2` = Mach 0 → 5.0**, and the missile's
`Mach_max = 4`. The `Cx0` peak of **0.88 sits at index 6 = Mach 1.2**, immediately
after the transonic rise from 0.479 at Mach 0.8 — which is exactly where a
slender supersonic body's wave drag peaks. The interpretation checks out.

So a DCS missile is **a 6-DOF rigid body with a full inertia tensor, static and
dynamic stability derivatives in pitch, yaw and roll, base drag separated from
total drag, and fin stall.** `Myw = −8.8` is aerodynamic damping in yaw; `Mxw =
−15.8` is roll damping; `Mxd = 5.73` is roll control power. This is a missile
that can be dynamically unstable, can be over-damped, and can have its fins
stall.

Nuclear Option's missile, by comparison, is *"total-incidence lift/drag curves on
a single body"* with a predictive rate limiter — one incidence angle, no
separate axes, no inertia tensor. It is a good model that costs almost nothing.
DCS's is a defensible engineering model that costs a great deal more.

### 6.2 The motor is phased and burns real mass

```lua
boost = { work_time = 0.1,  fuel_mass = 0,     impulse = 0,   nozzle_exit_area = 0.0132 },
march = { work_time = 6.5,  fuel_mass = 51.26, impulse = 234, nozzle_exit_area = 0.0132 },
controller = { boost_start = 0, march_start = 0.4 },
```

`impulse = 234` is specific impulse in seconds; 51.26 kg of propellant over 6.5 s
at Isp 234 gives roughly `51.26/6.5 × 234 × 9.81 ≈ 18.1 kN` of thrust, on a
161 kg missile — about 11 G of pure acceleration at launch, decaying as mass
burns off. **The mass is really removed** (the missile's `mass = 161.48` includes
it), so the missile gets lighter, its inertia changes, and its achievable G rises
through the burn. All of that is a consequence rather than a curve.

`controller.march_start = 0.4` — the sustainer lights 0.4 s after launch, which
is the separation delay.

### 6.3 The autopilot is a PID, and the gains say which one

This is the direct answer to "is it a PID or something else": **the missile is a
PID; the AI aircraft is a cascaded autopilot (§8); nothing in DCS is a
behaviour tree.**

**[LUA]** AIM-120C:

```lua
autopilot = {
  Knav = 4,             -- proportional navigation constant N'
  Ka = 16, Kd = 180, Krx = 2, Kx = 0.1,
  T1 = 309, Tc = 0.06, Tf = 0.1,
  accel_coeffs = { 0, 11.5, -1.2, -0.25, 24, 0.00016926 },
  gload_limit = 30,     -- matches Nr_max = 30
  fins_limit   = 0.31416,   -- 18°  (note: less than the actuator's 20°)
  fins_limit_x = 0.08727,   --  5°  roll channel
  loft_active = 1, loft_factor = 4.5, loft_off_range = 15000,
  cmd_delay = 0.8, delay = 0.2, null_roll = 0.785, op_time = 100,
}
actuator = {
  max_delta = 0.34907,   -- 20°
  max_omega = 6.98132,   -- 400°/s fin rate
  T1 = 0.002, T2 = 0.006, Tf = 0.005,
  fin_stall = 1,
  sim_count = 4,         -- 4 actuator sub-steps per physics step
}
```

Reading this:

- **`Knav = 4` is proportional navigation with N′ = 4**, the classic value.
  `PN_gain = 4` appears redundantly at the top level.
- **The autopilot commands less than the actuator can deliver** — `fins_limit`
  18° against `max_delta` 20°. Deliberate headroom so the control loop never
  saturates its own actuator.
- **`sim_count = 4`** is the actuator sub-stepped four times per physics tick.
  This is precisely Nuclear Option's rotor trick (4× azimuthal sub-stepping
  because a 280 rpm rotor turns 34° in one tick). **Two unrelated codebases
  independently sub-stepped the fastest sub-system rather than shortening the
  global timestep**, which is the correct answer and worth taking as a rule.
- **`cmd_delay = 0.8` and `delay = 0.2`** are transport lags — the guidance
  command does not take effect instantly. Autopilot lag is what produces realistic
  miss distance against a manoeuvring target; a lag-free autopilot hits everything.
- **`loft_off_range = 15000`** — stop lofting inside 15 km and go for the target.

The guidance law is **per-missile-class data, and the gain names change with the
law**, which is how you can tell what each one is doing:

| Weapon | Class / seeker | Distinctive autopilot fields | What it says |
|---|---|---|---|
| AIM-120C | `wAmmunitionSelfHoming`, ARH | `Knav=4`, `loft_active=1`, `loft_factor=4.5` | pure PN + loft |
| AIM-7 | `wAmmunitionSelfHoming`, SARH | `Kd`, `Ki`, `Kconv=4`, `Knv`, **`PN_dist_data={15000,1,9000,1}`**, `max_side_N=10`, `bang_bang=0`, `alg=0` | PN **gain-scheduled by range**, PID inner loop, 10 G lateral limit |
| AGM-88 HARM | ARM | `K_heading_hor=0.5`, `K_heading_ver=0.3`, **`K_loft=3.3`, `loft_active_by_default=1`** | lofts by default — range extension is the point |
| AGM-65D | `wAmmunitionSelfHoming`, IIR | **`K_GBias=0.26`, `Kg=2.5`** | **gravity bias** — an unpowered glide weapon must lead the drop |
| Vikhr | **`wAmmunitionVikhr`** | **`ray_limit_data = {2, 0.0873, 12, 0.0087}`** | **its own class** — laser beam rider |
| Igla | MANPADS IR | `gbias_Kp=4.8`, `gbias_time=1.2`, `lim_amp_Kp=1.5`, `lim_amp_time=1.6` | gravity bias with a time constant + **amplitude limiting** |

Three of those deserve a sentence:

**`bang_bang = 0`** exists, so DCS supports **on-off actuators** as a mode. Early
missiles did not have proportional fins, and the data has a slot for it.

**`PN_dist_data = {15000, 1, 9000, 1}`** is a range-scheduled navigation gain —
two `(range, gain)` breakpoints. A missile that uses the same PN constant at
15 km and at 500 m either wanders early or over-corrects late; scheduling the
gain against range is the standard fix, and it is data rather than code.

**Vikhr has its own class** because beam riding is a different *topology*, not
different gains: the missile does not know where the target is, it knows where
the beam is. `ray_limit_data = {2 s → 0.0873 rad, 12 s → 0.0087 rad}` is the beam
narrowing from **5° to 0.5° over ten seconds of flight**, which is the guidance
basket tightening as the missile gets further out. That is a *whole different
control problem* and DCS correctly refused to express it as a parameter of the
homing class.

This is the same lesson as Nuclear Option's `GetEvasionPoint()` being virtual —
**find the axis along which weapon types genuinely differ, and make that the
polymorphic one.** Nuclear Option found it at the *seeker* (eight seekers, one
autopilot, because loft and jink and datalink are all aimpoint edits). DCS found
it at the *guidance topology* (homing versus beam-riding). Both are right, and
both beat a `switch` on a weapon-type enum.

### 6.4 The seeker is a separate block with its own limits

```lua
sensor = {                       -- AIM-120C, active radar
  FOV = 0.2618,                  -- 15° half-angle
  max_w_LOS = 0.5236,            -- 30°/s max LOS rate it can track
  sens_far_dist = 30000,         -- self-screening range
  sens_near_dist = 100,
  delay = 1.5,                   -- seeker activation delay
  aim_sigma = 3.5,               -- aim error, sigma
  height_error_k = 20, height_error_max_h = 300, height_error_max_vel = 50,
  hoj = 1,                       -- home-on-jam capable
  ccm_k0 = 0.1,                  -- counter-countermeasures coefficient
}
gimbal = {
  pitch_max = 1.0472, yaw_max = 1.0472,   -- ±60° gimbal limits
  max_tracking_rate = 0.5236,             -- 30°/s
  tracking_gain = 50,
}
proximity_fuze = { radius = 15, arm_delay = 1.6 }
```

`height_error_k`, `height_error_max_h`, `height_error_max_vel` are a **low-altitude
degradation model**: the seeker gets worse near the ground, scaled by height and
by the target's speed. That is ground clutter, expressed at the seeker rather
than at the radar — and it means flying low genuinely degrades the missile, which
is the mechanic the AI in Nuclear Option exploits when it *"descends to 10 m for
clutter"*.

**`gimbal` being a separate block from `sensor` is the right decomposition**: the
seeker's detection performance and the physical limits of the thing it is bolted
to are independent properties, and a missile can lose a target either because it
faded or because it ran out of gimbal. Those failures feel completely different
to a player and should not share a parameter.

### 6.5 The `client` / `server` split, which is the network model in one flag

This is the finding I would not have predicted, and it is verifiable in seconds.

**[LUA]** Every missile definition contains **two near-identical sub-tables**,
`client` and `server`, each ~120–240 lines. Diffing them across five weapons of
four different guidance types (AIM-120C, AIM-7, R-27T, Vikhr-M, AGM-65D):

```
== AIM_120C   client 208 lines   server 208 lines   DIFFS: 4
     -   fantom = 1,      (client, warhead)
     +   fantom = 0,      (server, warhead)
     -   fantom = 1,      (client, warhead_air)
     +   fantom = 0,      (server, warhead_air)
```

**Every single field is identical except `warhead.fantom`, which is 1 on the
client and 0 on the server. Five for five.**

So: the client flies a **fully modelled missile** — same aerodynamic tables, same
autopilot gains, same seeker, same motor — whose warhead is a *phantom*. The
server flies the same missile with the real warhead. Damage is decided by the
server; the client's copy exists so the missile *looks and moves* right locally
with no waiting for the network.

That is **client-side prediction with server-authoritative damage, expressed as
one boolean in shared data instead of as two code paths.** The prediction is
exact rather than approximate, because both sides run identical dynamics from
identical tables. It is the cheapest correct answer to "the missile must look
smooth and must not be trusted", and it is worth putting straight into the
transferable list.

Compare Nuclear Option's damage note §4, which solves the same trust problem the
other way: the server keeps a **rolling five-second log of firing rays** and
validates claimed hits with an angular tolerance that opens up at close range —
plausibility rather than reproduction. **Two valid answers: reproduce the
projectile authoritatively (DCS), or let the client claim and check the claim is
plausible (Nuclear Option).** The first costs server CPU per projectile; the
second costs a validation heuristic and accepts some slop. DCS can afford the
first because a mission has tens of missiles, not thousands of bullets.

### 6.6 Guns: real exterior ballistics, dispersion, and ricochet

**[LUA]** A 20 mm API round, complete:

```lua
{ caliber = 20, mass = 0.1, round_mass = 0.26, cartridge_mass = 0.12,
  v0 = 1050,                    -- muzzle velocity m/s
  cx = { 0.5, 1.27, 0.7, 0.2, 2.3 },     -- drag law, 5 parameters
  k1 = 2e-08,
  Da0 = 0.0022, Da1 = 0, Dv0 = 0.006,    -- DISPERSION
  piercing_mass = 0.1, explosive = 0, damage_factor = 639,
  life_time = 30, tracer_on = 0, tracer_off = -100,
  rotation_freq = 7,
  silent_self_destruction = false,
  rebound_ground   = { angle0 = 55, angle100 = 73, deviation_angle = 30,
                       velocity_loss_factor = 0.5, cx_factor = 5 },
  rebound_water    = { angle0 = 65, angle100 = 83, ... },
  rebound_concrete = { angle0 = 50, angle100 = 75, ... },
  rebound_object   = <same table as concrete>,
}
```

Six things here are worth having:

**`cx` is a five-parameter drag law, not a constant.** A projectile's drag
coefficient varies enormously through the transonic region and DCS carries the
curve per round type. An HE-T round from the same era has `cx = {1, 0.85, 0.65,
0.2, 1.6}` — a different shape, because it is a different shape.

**`Da0` / `Dv0` are dispersion at the round level**: angular dispersion
(0.0022 rad ≈ 2.2 mrad ≈ 7.5 MOA) and muzzle-velocity dispersion (0.6%). Weapon
accuracy is a property of the ammunition, so the same gun firing different belts
disperses differently, for free.

**The ricochet model is per-surface and has four surfaces.** `angle0` and
`angle100` bracket the critical ricochet angle across the velocity range;
**water's angles are the highest (65°/83°)**, meaning rounds skip off water far
more readily than off ground (55°/73°) or concrete (50°/75°). That is correct
physics and it is three numbers.

**`cx_factor = 5` after a ricochet.** A bullet that has bounced is deformed and
tumbling, so its drag goes up fivefold and it decelerates out of relevance
quickly. One number that prevents ricochets being a long-range hazard, which is
both realistic and a quiet performance saving.

**`tracer_on = 0`, `tracer_off = -100`** is the tracer burn window in seconds, so
`-100` means "never stops". The HE-T round has `tracer_off = 3` and
`silent_self_destruction = true` with `life_time = 3` — **it self-destructs
exactly when its tracer burns out**, which is how the real fuze worked.

**`life_time`** is 30 s for the API and 3 s for the self-destructing HE. That is
also the projectile's budget in the simulation, so ammunition design and
projectile lifetime management are the same field.

Against Nuclear Option's kinetic model — `pierce · v²/v₀²`, so range, dive angle
and platform speed all matter with no falloff curve — DCS is doing a great deal
more work for a genuinely different product: **NO wants gunnery to be
*understandable*; DCS wants it to be *reproducible* against a firing table.**

---

## 7. Sensors: the split that defines the game

### 7.1 AI sensors are data; player sensors are code

The datamine's own README states it plainly: `db/Sensors/Sensor` holds
definitions for **"AI controlled units, repeat, not player aircraft! (Those
values are hidden in the source code for each module on the C++ side)"**.

**[LUA]** The AI's AN/APG-68 in its entirety:

```lua
{ DisplayName = "AN/APG-68", SensorType = 1, type = 2, category = 1,
  max_measuring_distance = 265000,
  scan_period = 5,
  scan_volume = { azimuth = { -60, 60 }, elevation = { -30, 30 } },
  air_search = {
    TWS_max_targets = 4,
    centered_scan_volume = { azimuth_sector = 30, elevation_sector = 30 },
    detection_distance = { { 32000, [0] = 68400 }, [0] = { 54000, [0] = 68400 } },
    lock_on_distance_coeff = 0.85,
    velocity_limits = { radial_velocity_min      = 27.7778,
                        relative_radial_velocity_min = 27.7778 },
  },
  surface_search = { RBM_detection_distance = 150000, GMTI_detection_distance = 180000,
                     HRM_detection_distance = 20000, RCS = 100, vehicles_detection = true },
}
```

Twenty-odd numbers. And an EWR:

```lua
{ DisplayName = "1L13 EWR", detection_distance = 300000, scan_period = 1,
  scan_volume = { azimuth = { -180, 180 }, elevation = { -15, 60 } },
  velocity_limits = { radial_velocity_min = 50 }, ... }
```

The pieces that matter:

- **`detection_distance` is a nested table**, not a scalar — the `[0]`/`[1]`
  indexing distinguishes look-up from look-down (68.4 km against 32/54 km), so
  **look-down range penalty is data.**
- **`lock_on_distance_coeff = 0.85`** — you detect at range R and can lock at
  0.85 R. One number for the whole detect/track distinction.
- **`radial_velocity_min = 27.78 m/s` is the notch, as data.** 27.78 m/s is
  exactly 100 km/h. A target whose closure rate falls below it disappears from a
  pulse-Doppler radar. The EWR's threshold is 50 m/s.

That last one is worth putting beside Nuclear Option's, because they solve the
same tactic at different fidelities:

| | Nuclear Option | DCS (AI) |
|---|---|---|
| Notching | `min(\|dot(losDir, targetVel)\|, 150) · dopplerFactor`, applied as a **multiplier** on signal | `radial_velocity_min` — a **hard threshold** on closure |
| Effect of flying abeam | removes your processing gain; you must *also* be in clutter | you drop out of the scan |
| Clutter | `targetRadius²·2 / radarAlt²`, inverse-square in height, subtracted before the floor | `height_error_*` on the missile seeker; ground clutter on the radar not exposed in data |
| Character | graded, stackable, tunable | binary |

Nuclear Option's is the better *game* mechanic — graded, and it forces you to
stack two techniques rather than find one magic heading. DCS's AI version is a
threshold because the AI radar is a detection oracle, not a signal model; the
*player's* radar (in C++) is where the real modelling lives, and this note cannot
see it.

### 7.2 Signatures are two coefficients

**[LUA]** From the F-16C: `RCS = 4` (m²), `IR_emission_coeff = 0.6`,
`IR_emission_coeff_ab = 3`.

**Afterburner is a 5× step in IR signature**, from one pair of numbers. That is
the entire "don't light the burner with a MANPADS around" mechanic. Nuclear
Option's radar equation uses `RCS^0.25` so that *"the fourth root makes stealth
valuable but never decisive"* — a deliberate design choice about how much a
signature should matter. DCS gives the raw m² and lets the sensor model decide,
which is the correct division when the sensor model is trying to be right rather
than trying to be fun.

Countermeasures likewise:

```lua
passivCounterm = { SingleChargeTotal = 120, CMDS_Edit = true,
                   chaff = { chargeSz = 1, default = 60, increment = 30 },
                   flare = { chargeSz = 1, default = 60, increment = 30 },
                   preferred_flare_kind = 2 },
chaff_flare_dispenser = { { pos = { -3.65, -0.5, -0.93 }, dir = { 0, -1, 0 } },
                          ... 4 dispensers with real positions and directions },
```

**The dispensers have positions and ejection directions**, so countermeasures
leave the aircraft from the right place pointing the right way — which matters
because a flare's separation geometry is what the seeker's discrimination logic
tests. Nuclear Option makes the same commitment from the other end: its IR
decision includes **angular separation from flare drag**, and flares are affected
by the wind field. Both games agree that *where the flare goes* is the mechanic,
not *how many you have*.

---

## 8. The AI: a general autopilot, not a behaviour model

### 8.1 What the GFM actually is

**[ED]**, the General Flight Model newsletter, is the most useful AI source that
exists, and its framing is a cost argument:

> **SFM** produces accurate trajectory parameters but "suffers from a lack of
> natural short-period movement" and switches between ground and flight modes.
>
> **AFM/PFM** require "substantial work to create each individual aircraft flight
> data model" and impose excessive processor demands when multiple aircraft
> operate simultaneously.
>
> The GFM borrows the short-period motion characteristics from existing PFM player
> models while maintaining computational efficiency. **"The GFM 'pilot' will use
> the same control surface deflection as PFM and can naturally stall and spin as
> well; the GFM also experiences air turbulence."**

And the part that answers the steering question directly **[ED]**:

> The greatest challenge in developing GFM was creating a **multi-level autopilot
> system capable of controlling any aircraft**, using an approach that permits
> **automatic use of aircraft flight parameters rather than requiring individual
> aircraft adjustments**.

Read carefully, that is: **a cascaded (multi-loop) autopilot whose gains are
derived from the aircraft's own tabulated parameters rather than hand-tuned per
airframe.** Outer loops for trajectory, inner loops for attitude and rate,
gains scheduled from the aircraft's own coefficients so that adding an aircraft
does not mean re-tuning a controller. ED note this is "particularly appreciated
for trans-sonic and supersonic regime changes, which no longer require
substantial tuning".

**So: yes, it is fundamentally PID-shaped, but the interesting engineering is the
gain derivation, not the controller.** Anyone can write a cascade; the hard part
is making it stable for 140 different aircraft without 140 tuning passes.

Two more details **[ED]**: the GFM adds deliberate **"micro-delays, errors and
limitations"** to the virtual pilot — the fix for AI that flies like a UFO is to
degrade the *pilot*, not the *aircraft*. And the project cost **about two years
of programming, with tight formation flying alone taking a further five months**,
which is a startling number until you consider that formation flying is a
tracking problem with the reference signal being another instance of the same
unstable controller.

### 8.2 The comparison that matters

Nuclear Option's solution to the same problem is one line
([`nuclear_option_combat.md`](../nuclear_option/nuclear_option_combat.md) §7):

```
RotateTowards(heading, toTarget, 0.9 · (V / Vcorner)²)
```

**Rate-limit the demand, not the response** — with the square matching the way
turn radius scales with speed. The AI writes the same `ControlInputs` the player
does and goes through the same fly-by-wire, so it must solve turn radius and
energy honestly, and it does so by never asking for more than it can have.

Set the two side by side:

| | Nuclear Option | DCS GFM |
|---|---|---|
| Controller | demand rate-limiter into the shared FBW | multi-level autopilot |
| Flight model used | **the player's, exactly** | **its own tier** |
| Tuning per aircraft | none — `Vcorner` is a property | none — gains derive from parameters |
| Development cost | days | **~2 years** |
| Aircraft supported | ~15 | ~140, including 80 years of aircraft types |
| Can the AI cheat? | structurally impossible | structurally possible, and players notice |
| Does a FM fix improve the AI? | **automatically** | no — separate model |

**Both teams identified "no per-aircraft tuning" as the requirement**, and both
met it. They met it at wildly different costs because they were solving different
sized problems. Nuclear Option's approach does not scale to a Bf-109 and an
F-16 and a B-17 and a Ka-50 in one autopilot; DCS's two-year cascade does.

The price DCS pays is visible to its own players: because the AI does not use the
player's flight model, the AI's performance is not the player's performance, and
the community complaint that *"the AI doesn't follow the same PFM flight model
that players use"* **[COMMUNITY]** is structurally true rather than a
misperception. **A tier system is a correctness gap by construction, and no amount
of tuning closes it — it can only be narrowed.** GFM is ED narrowing it.

### 8.3 What this note cannot tell you

Everything above is *flight control*. The layer above — target selection, threat
evaluation, when to commit, when to abort, formation tactics, SAM battery
coordination — is **entirely inside compiled C++ with no data exposure and no
published description.** Skill levels (Rookie → Ace) exist and demonstrably change
behaviour, but *what* they scale is not documented.

This is the largest single gap in the note, and it is exactly the area where
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) is richest: `TrackingInfo`
as a 90-line world model with fog-of-war as one timestamp and a four-second
grace; deconfliction as two `sbyte`s in the denominator of every scoring
function; `AnalyzeTarget` shared by fighters, SAMs and ships; `InterceptViability`
projecting escape rate onto weapon top speed. **For the decision layer, read the
Nuclear Option notes — this one has nothing.** See §13.

---

## 9. Sound: four radii, a cone, a filter, and the speed of sound

DCS uses an **in-house audio engine** (no FMOD, no Wwise), configured by plain-text
`.sdef` files under `Sounds/sdef/`, one per sound, referencing a wave and a set of
parameters. The complete parameter vocabulary **[COMMUNITY]**:

```
gain, attack, release
inner_radius, outer_radius, silent_radius, peak_radius
cone_inner_angle, cone_outer_angle, cone_outer_gain
direction = { x, y, z },  position = { x, y, z }
distance_filter_offset
nodoppler = true/false
pitch_random
```

Units and weapons reference these by a **sounder name** — the AIM-120C's table
carries `sounderName = "Weapons/Missile"` **[LUA]** — so the sound assignment is
a string in the same shared database as the aerodynamics.

### 9.1 The four radii

This is the part worth stealing. Most engines give you a min and max distance and
a rolloff curve. DCS gives four:

- **`peak_radius`** — inside this, the sound is at full gain (a plateau, not a
  singularity as distance → 0).
- **`inner_radius`** — documented in the community as the range at which the
  sound is audible **in a closed cockpit** **[COMMUNITY]**.
- **`outer_radius`** — the range at which it is audible **outside** the cockpit
  (external view, or canopy open).
- **`silent_radius`** — beyond this, nothing.

**`inner_radius` and `outer_radius` are the same sound heard through two different
listener contexts, expressed as two numbers on the source rather than as an
occlusion system.** A closed canopy is not modelled as a filter or an occluder; it
is a second attenuation curve, selected by camera state. That is enormously
cheaper than real occlusion and, for the one occluder that matters in a flight
sim, indistinguishable.

Nuclear Option does the same class of trick and the parallel is exact: **the
camera state toggles Doppler and spatial blend** — the aircraft you are flying
gets `dopplerLevel = 0` and `spatialBlend = 0`. Both games discovered that *the
listener's situation is a property you can bake into the source's parameters*,
and neither built a propagation system to get it.

### 9.2 Distance as a filter, and this is the "miles away" question

**`distance_filter_offset`** is the interesting one: a **distance-driven low-pass
cutoff**. High frequencies are absorbed by air far faster than low ones, so a
distant sound is not merely quieter — it is *duller*, and the dullness is what
your ear reads as "far away" rather than "quiet".

Nuclear Option implements the same physics as one divide: the low-pass cutoff for
a delayed explosion is **`22000 / travelTime`** — *"one divide that models
atmospheric absorption and turns a delayed sound from a late one into a distant
one"*. DCS parameterises it per sound instead of deriving it, which is more work
to author and more control for the sound designer.

**If you build one thing from this section, build the distance low-pass.** It is
the single highest ratio of perceived realism to code in game audio, and both of
these games independently rate it as essential.

### 9.3 Propagation delay and the sonic boom

DCS **does simulate the speed of sound**, so a distant explosion arrives late
**[COMMUNITY]**; the community's frame of reference is that DCS is the sim other
sims are compared against for this. Sonic booms are modelled, and community mods
exist specifically to add the **double bang** (nose shock and tail shock) at close
range while leaving the stock single report for distant aircraft **[COMMUNITY]**.

This is the one area where **the note is thinner than its Nuclear Option
counterpart, and the counterpart is better documented and probably better
designed.** Nuclear Option's audio note has the whole mechanism in the open:

- explosions register a **pending sound on an expanding wavefront**
  (`propagation += 340f * dt`, started at the cube-root fireball radius) and play
  when the sphere reaches the listener;
- **camera shake fires on arrival, not detonation**, so desynchronisation is
  impossible;
- the aircraft is **muted while you are outside its Mach cone** (`μ = asin(1/M)`),
  the boom fires once on cone entry with its source placed at the **emission
  point walked back along the flight path**, and the engine noise switching on is
  *the same state transition*;
- a guard prevents booming yourself in chase view.

I could not establish DCS's equivalent mechanism from published material. What I
can say is that DCS's *observable behaviour* — delayed distant sounds, a boom that
arrives with the shock rather than with the aircraft — is consistent with the
same design, and the same design is what the Nuclear Option note derives from
first principles. **[inferred]**

### 9.4 Doppler and the exhaust cone

**`nodoppler`** is a per-sound opt-out, which is the same admission Nuclear Option
makes explicitly: full-strength Doppler at supersonic closure *"sounds like a
broken tape"*, so NO runs everything at **0.6, not 1.0**, and zeroes it entirely
for the aircraft you are riding. DCS's boolean is the coarser version of the same
knowledge. **Physically correct Doppler is wrong at these speeds, and everyone who
ships a jet discovers it.**

The **cone** parameters (`cone_inner_angle`, `cone_outer_angle`,
`cone_outer_gain`, plus a `direction`) are how a jet exhaust gets to be far louder
astern than ahead — the standard directional-source model, driven by an authored
direction vector on the source. Nuclear Option gets the same effect from **one dot
product** (`max(1 − 2·camFacing, 0.01)`, near-zero within 60° of astern) and
extracts a **4:1 rear-to-front loudness ratio** from it.

**Same phenomenon, authored versus derived** — which is the whole DCS/Nuclear
Option axis showing up again, this time in audio. And the same trade applies: the
authored version lets a sound designer shape it per aircraft without a programmer;
the derived version cannot be got wrong.

Engine sound itself is driven from **RPM and power** — Nuclear Option states this
directly (pitch from RPM, volume mostly from power) and DCS's `.sdef` set plus its
per-aircraft engine parameters imply the same **[inferred]**. There is no
evidence DCS does anything more exotic, such as synthesis.

---

## 10. Rendering: raymarched weather over a 400 km sphere

> **This section is a summary. The full treatment is
> [`dcs_clouds.md`](dcs_clouds.md)** — the withdrawn 2014 system, the spherical
> earth and 400 km radius, the fog/voxel-resolution conflict, the thirty shipped
> presets read out of the mission format, the network-synchronisation constraint,
> the line-of-sight regression, and the three-way comparison against
> [`unigine_clouds.md`](../../rendering/unigine_clouds.md) and
> [`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md).

ED's *Volumetric Weather* white paper is the one genuinely technical rendering
document they have published, and it is unusually candid **[ED]**.

### 10.1 The three generations of clouds

**Particles (original).** Each cloud a separate object. ED's own assessment: easy
to scatter and easy to query for line-of-sight blocking, but *"a rather
inefficient rendering technique"* with sorting problems and rotating/intersecting
particles under camera motion, and *"almost impossible to effectively describe and
render huge multi-layered cloud formations that cast shadows on the terrain,
objects, and themselves"*.

**Raymarch, first attempt (2014).** Single-layer, flat-earth, limited quality and
draw distance, self-shadowing problems. ED shipped it and it did not fit the frame
budget: *"Those of you who have been with us for a while may remember how an
earlier update included clouds being rendered at the edge of the render field and
how they would slide by, simulating movement."* They pulled it. **A published
post-mortem on a shipped rendering feature that was not good enough is rare and
worth the citation on its own.**

**Raymarch, rewrite (2021).** Written almost from scratch:

- designed for a **spherical earth**;
- **up to 16 independent layers**;
- **400 km draw radius**, "can be increased even more without much loss of
  quality";
- self-shadowing no longer radius-limited — *"clouds on the horizon could now cast
  a shadow across the entire map"*;
- all transparent objects and effects blend with the clouds, which **eliminated
  the sorting problem** that killed the particle version;
- rain and snow are part of the same system;
- optical effects on the volume: rainbow, moonbow, halo, glory, fogbow.

The governing idea, in ED's words: *"clouds no longer exist as separate objects,
but are rather represented as continuous volumes in which the cloud density is
known at each point in space."*

### 10.2 The fog rewrite, and the honest engineering admission

The old fog was flat-earth, uniform, unshadowed, and could not blend with the new
spherical-earth clouds. So fog had to become **part of the cloud system** — and ED
explain exactly why that is hard **[ED]**:

> Drawing the fog in one pass along with the clouds while storing information
> about the density of the fog in the same volume as the cloud data was highly
> complex… **To describe low and dense fog, a very small voxel size is required,
> but a larger voxel is sufficient to describe a small cloud.**
>
> Moreover, even today, the technique of raymarching clouds is quite
> computation-heavy, and the entire frame of clouds is not drawn completely in one
> pass. Instead, the technique of **temporal reprojection** is used that allows
> cloud samples drawn in previous frames to be reused. This technique isn't
> perfect either as it introduces a whole class of problems and artifacts that
> only get worse when trying to paint thin, dense fog against the ground.
>
> **Today, we do not know of a single successful implementation of a single
> volumetric cloud system that can draw fog in one pass without a significant
> performance hit.**

That is a resolution conflict inside a shared data structure, stated cleanly:
**two phenomena want the same volume at two different frequencies, and temporal
reuse — the thing that makes the volume affordable — is precisely what breaks on
the high-frequency one.**

This is directly relevant to
[`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md), which describes RDR2's answer to
the same problem: **froxel volume near, raymarch far** — two representations at
two frequencies rather than one shared volume. ED chose to unify and paid for it
in engineering; Rockstar chose to split and paid for it in having two systems to
blend. Worth reading the two notes together before this project builds either.

The resulting fog can be **up to 5 km thick "to simulate suspended matter in the
air"**, is patchy rather than uniform, animates, self-shadows, receives cloud and
ground shadows, and can be any colour. Dust is now the same system.

### 10.3 The part that matters for gameplay, not looks

Two sentences carry more design weight than the rest of the paper **[ED]**:

> Just like fog, the dust effect **can affect the AI and sensor visibility**,
> which was not the case in previous versions of DCS.

and, in the remaining-work list:

> **AI and visual and sensor line of sight blocking** *(still to do)*

So for several years DCS's clouds were **purely visual** — the AI could see
through them and so could sensors. The particle system could do LOS queries (ED
say so explicitly); the raymarched volume, which is strictly better to look at,
**lost that capability**, and getting it back is still open work.

**That is the trap, and it is the reason this section is in a study note about
gameplay rather than in one about rendering.** A representation change that
improves the image can silently remove a query the simulation depended on. The
particle cloud was a set of objects you could test against. The volumetric cloud
is a density field evaluated on the GPU during raymarching — and there is no
cheap CPU-side way to ask it "is this line blocked". The simulation needs an
answer at a completely different rate and place from the renderer.

Both [`rainbow_six_siege.md`](../../shooters/rainbow_six_siege.md) §4.9 (*"poking holes degrades
occlusion efficiency"*) and [`ruse.md`](../../strategy/ruse.md) §5 (three separate kd-trees split
**by purpose, not by contents**) point the same way. **Rendering geometry and
query geometry are different assets with different budgets, and unifying them is
usually a mistake.** If cromwell ever grows volumetric weather that gameplay must
respect, the query structure should be designed first and separately — a coarse
occupancy volume the simulation owns, which the renderer's field is authored to
match — not extracted from the render representation afterwards.

### 10.4 The rest of the renderer

Much thinner sourcing here **[COMMUNITY]**, mostly ED roadmap posts and interviews:

- **DX11, deferred shading, PBR**, with the stated design constraint of looking
  and performing well "from 1 meter to 50,000 meters" — which is the same
  extent problem [`map_scale.md`](../../../topics/scale/map_scale.md) and
  [`elite_dangerous.md`](../../space/elite_dangerous.md) treat, at an intermediate scale.
- **Volumetric lights** — clouds and fog are lit by discrete light sources and
  by earthshine, so city lights illuminate cloud bases.
- **A Vulkan port in late-stage development**, motivated explicitly by wanting
  manual VRAM management, granular resource streaming, frame generation, broader
  DLSS/FSR support and cockpit ray tracing. The render code currently supports
  both APIs on separate branches.

I have no primary material on DCS's terrain LOD, its object LOD, its shadow
cascades or its VR-specific path, and I am not going to reconstruct them from
screenshots. §13.

---

## 11. DCS against Nuclear Option: the comparison, and what it means

### 11.1 The table

| | **Nuclear Option** | **DCS World** |
|---|---|---|
| **Aero model** | geometry *is* the model — per-part area, airfoil, transform | whole-airframe coefficient tables from wind tunnel / CFD / flight test |
| **Sideslip** | needs no term (a fin is a rolled part) | a tabulated axis (`m_yaw` vs AoS) |
| **Flight model tiers** | **one**, everyone uses it | **five** (SFM / AFM / PFM / EFM / GFM) |
| **Control interface** | `ControlInputs`, six floats, universal | `ed_fm_set_command(int, float)`, per-module FCS behind it |
| **Fly-by-wire** | one shared implementation | reimplemented per aircraft, faithfully |
| **AI flight** | shared FBW, `RotateTowards` demand limiter | 2-year multi-level autopilot, own model tier |
| **Can AI cheat?** | structurally impossible | structurally possible |
| **Rotor** | blade element, flapping hinge, 4× azimuth sub-step | blade element **+ momentum theory**, flapping **and lead-lag** hinges, blade collision |
| **Damage** | **physical** — parts detach, wings really stop lifting | **ray through an X-ray mesh**, 150 components, materials, wall thickness, fluid plenums, failure IDs |
| **Damage → aero** | free | a modelled correction on the table |
| **Target/weapon matching** | `TypeIdentity · RoleIdentity` — 5-vector dotted with 4-vector | per-weapon tables and flags |
| **Missile** | one-body total-incidence curves, one PID autopilot, 8 seekers | 6-DOF, full inertia tensor, per-axis stability derivatives, PID with per-class gains, 2 guidance topologies |
| **Missile networking** | client claims hits; server validates a 5 s ray log for plausibility | **client flies a `fantom` warhead; server flies the real one** |
| **Guns** | `pierce · v²/v₀²` | 5-parameter drag law, per-round dispersion, 4-surface ricochet, tracer burn windows |
| **Notching** | graded multiplier; must be stacked with clutter | hard `radial_velocity_min` threshold (AI radars) |
| **Audio middleware** | none — ~250 lines of stock Unity | none — in-house engine, `.sdef` files |
| **Distance model** | derived (`22000/travelTime` low-pass, 340 m/s wavefront) | authored (4 radii + `distance_filter_offset` per sound) |
| **Doppler** | 0.6 global, 0 for your own aircraft | per-sound `nodoppler` boolean |
| **Clouds** | not a factor | 16-layer raymarched volume, 400 km, temporal reprojection |
| **Clouds block LOS?** | n/a | **no — still open work** |
| **Aircraft in the air** | tens | **hundreds** |

### 11.2 Four readings

**One: every difference reduces to aircraft count × per-aircraft depth.** This is
the same conservation law [`battle_scale.md`](../../../topics/scale/battle_scale.md) states for ground
combat (*count × depth ≈ constant*) and
[`broken_arrow_damage.md`](../broken_arrow/broken_arrow_damage.md) §5 states for damage (*a
thousand units with a crit table, or forty real parts per unit*). DCS needs a
hundred aircraft, so the AI gets a table and the player gets a cycle model.
Nuclear Option needs tens, so everyone gets the same model and the AI is honest
for free. **Neither team was cleverer; they were solving for different N.**

**Two: they converged wherever the problem forced it.** Both wrote blade-element
rotors. Both sub-step the fastest sub-system rather than the whole simulation
(NO's rotor azimuth, DCS's `sim_count = 4` on missile actuators). Both refused
audio middleware. Both put the distance low-pass in as essential. Both made the
countermeasure's *geometry* the mechanic rather than its count. Both identified
"no per-aircraft AI tuning" as a hard requirement. **Where two independent teams
with opposite philosophies build the same thing, that thing is not a style
choice.**

**Three: the split is authored-versus-derived, and it runs through every
system.** DCS authors: aerodynamic coefficients, sound cones and radii, damage
component materials and wall thicknesses, per-missile autopilot gains. Nuclear
Option derives: aerodynamics from part transforms, the exhaust cone from a dot
product, the audio low-pass from travel time, control authority from dynamic
pressure. **Authored scales with content budget and gives designers control
without programmers; derived scales with engineering quality and cannot be got
inconsistent.** This is the same finding as
[`broken_arrow_audio.md`](../broken_arrow/broken_arrow_audio.md)'s *"hand-rolled fails by being
wrong; bought fails by being disconnected"*, one level down: **derived fails by
being wrong everywhere at once; authored fails by being wrong in one file nobody
opens.**

**Four: DCS's tier system is a permanent, visible tax.** It bought the aircraft
count, and it costs a real correctness gap between what the player flies and what
the AI flies — one the players can feel, that ED have spent two years narrowing
and cannot close. **A fidelity tier is not a temporary optimisation. It is a
second implementation of the same physics that must be maintained forever and can
always disagree.** If this project ever contemplates "a cheap version for distant
units", that is the sentence to re-read.

---

## 12. What transfers, ranked

Ordered by value to this project, not by how interesting they are.

1. **Resolve protection by tracing, not by enumerating.** DCS traces the round
   through an X-ray mesh and damages what it passes through, in order. Nuclear
   Option casts at each blast candidate and damages whatever it hits first,
   in fifteen lines. Armour, shadowing and cover all stop being special cases.
   **The one-dimension-down version is cheap enough to use anywhere.**
2. **One failure-ID channel with several producers and one consumer** (§5.4).
   Combat damage and the scheduled random-failure system emit the same strings;
   the systems model cannot tell them apart. A new failure is an ID and a
   consumer. This is the best structural idea in DCS's data.
3. **Client-side prediction as one flag on shared data** (§6.5). Identical
   dynamics both sides, `fantom = 1` on the client's warhead, `0` on the
   server's. Exact prediction, authoritative damage, no second code path.
4. **Sub-step the fastest sub-system, not the whole simulation.** `sim_count = 4`
   on the missile actuator; 4× azimuthal sub-stepping on Nuclear Option's rotor.
   Two independent arrivals at the same rule.
5. **Design the query structure before the render structure** (§10.3). DCS's
   volumetric clouds are better in every way except that the simulation can no
   longer ask them anything, and that has been open work for years. Applies
   directly to any volumetric system cromwell grows.
6. **Command less than your actuator can deliver.** `fins_limit` 18° against
   `max_delta` 20°. Deliberate headroom so the controller never saturates the
   thing it is controlling — applies to any servo, turret, camera or steering
   system.
7. **The distance low-pass.** `distance_filter_offset` authored, or
   `22000/travelTime` derived. Highest realism-per-line in game audio, and both
   games rate it essential.
8. **Two attenuation curves selected by listener context** beats an occlusion
   system, when there is one occluder that matters (§9.1).
9. **Anisotropic drag from two coefficients** (`fuselage_Cxa0` / `Cxa90`).
   Cheapest useful aerodynamic term that exists; works for hulls, fuselages and
   anything that should not want to travel sideways.
10. **Per-store drag as a multiplier on the store** (`Cx_gain`), not a property of
    the carrier. Add content, get correct behaviour on every carrier for free.
11. **Make the *topology* polymorphic, not the parameters.** Vikhr gets its own
    class because beam-riding is a different control problem; homing missiles
    share one class and differ by gains. Find the axis along which things
    genuinely differ.
12. **Range-schedule your gains** (`PN_dist_data`). A controller tuned for one
    engagement distance is wrong at the others, and the fix is two breakpoints.
13. **Degrade the pilot, not the vehicle** (§8.1). ED's fix for UFO-like AI was
    micro-delays and errors in the virtual pilot. Nuclear Option does the same
    with `randomError / skill` on the evasion geometry. **Difficulty belongs in
    the decision layer, never in the physics.**
14. **A blanket damage-immunity window at spawn** (§5.7). Thirty seconds. Ugly,
    correct, universally needed.
15. **`cx_factor = 5` after a ricochet.** One number that makes a deflected
    projectile decelerate out of relevance — realistic *and* a performance
    saving. The general form: make degraded states cheap to stop simulating.

### Two anti-patterns

- **Publishing a physics ABI in a non-standard axis convention** (§1.2). Every
  consumer pays forever, and the open-source implementations all carry the same
  warning comment.
- **A fidelity tier for the same physics** (§11.2, reading four). It buys scale
  and it never stops costing correctness. Pay it deliberately, with the number
  that justifies it written down.

---

## 13. What this note does not establish

Longer than usual, and deliberately so.

**Nothing about PFM's internals.** The player flight models are compiled C++.
§2 describes ED's stated *method* from their own document; it does not describe
their implementation. How the coefficient tables are interpolated, how many
dimensions they have, how control-surface deflection enters, how the
thermodynamic engine model is structured — all unknown.

**Nothing about the AI's decision layer.** §8 covers flight *control* only.
Target selection, threat evaluation, commit/abort logic, formation tactics, SAM
coordination, and what the Rookie→Ace skill levels actually scale are entirely
unexposed. This is the biggest gap and the place where the Nuclear Option notes
are strongest by comparison.

**Player sensors.** The datamine's sensors are AI-only by its own statement.
Everything DCS does for the player's radar — scan patterns, RWR logic, ECM,
track-while-scan implementation, real clutter modelling — is in module C++.

**The sound engine's implementation.** §9 lists the `.sdef` vocabulary and infers
behaviour from parameter names, community reporting and Nuclear Option's parallel
solutions. **I could not source DCS's actual propagation-delay or sonic-boom
mechanism**, only that both exist and behave consistently with the design
Nuclear Option's note derives. §9.3 is the weakest section here.

**Column 5 of `SFM_Data.table_data`** (§2.2). Rises 0.025 → 6 across the Mach
range. The other seven columns are identified, one of them verified against wing
geometry. This one is not.

**Whether the legacy `Damage` cell model has been superseded.** The F-16C uses
cells and `deps_cells`; the Bf-109K-4 uses `construction`/`innards`/`children`.
Whether jets are scheduled to migrate, or whether the new model is
WWII-specific by design, is not established.

**The two weapon APIs.** The datamine distinguishes a "NEW Weapon API"
(`weapons_table/weapons`) from an "OLD Weapon API" (`rockets/`, `bombs/`), and
both are populated in the current build — the AIM-120C is new, the AIM-54 and
R-73 are old. Whether the old path is a compatibility shim over the new one or a
genuinely separate simulation is unknown, and it matters, because it would mean
two missiles in the same engagement are being flown by different code.

**Rendering beyond the weather white paper.** No primary material on terrain LOD,
object LOD, shadow cascades, the VR path, or how the 1 m → 50 km depth range is
actually handled. §10.4 is roadmap-post sourcing and should be treated as such.

**Version currency.** The Lua tables are from build 2.9.28.26283 (July 2026).
ED's FM principles PDF and the volumetric weather paper are undated in their
text. The damage report is November 2020 and the GFM newsletter December 2021, so
both describe systems that have had years of development since.

**No performance numbers at all.** Nothing in this note is measured. There is no
public frame budget, no cost-per-AI-aircraft figure, no profile. Every claim
about what is "cheap" or "expensive" here is structural reasoning, not
measurement — which is exactly the thing CLAUDE.md says to distrust, so distrust it.
