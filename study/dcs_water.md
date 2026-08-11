# DCS World's water — the system nobody wrote about, and why that is the finding

**Short answer: no, and the absence is the interesting part.**

[`dcs_clouds.md`](dcs_clouds.md) exists because Eagle Dynamics published an
eleven-page white paper on volumetric weather, rewrote the system twice, and
documented the failure of the version they withdrew. **There is no equivalent
document for water. There is no newsletter, no white paper, no dev blog, no
roadmap entry.** The top search results for DCS water rendering are *wish-list
threads*, several of them a decade old.

That makes this a deliberately short note, and it would be dishonest to pad it
into a "deep dive" matching the clouds one. What it does instead:

- **§1** states the evidence for the absence, because "I could not find much" is a
  weak claim and "here is the specific shape of the nothing" is a strong one.
- **§2–§3** give what *is* establishable from shipped data — and there are two
  hard structural facts there, both verifiable: **sea state is a pure function of
  surface wind**, and **ships carry no wake parameters and are kinematic bodies,
  not floating ones.**
- **§4** reconstructs how the wake and surface must work, marked as
  reconstruction throughout.
- **§5 is the payoff and the reason to write the note at all.** Read against
  [`sea_of_thieves_water.md`](sea_of_thieves_water.md) — which covers Sea of
  Thieves' FFT ocean, CS2's water, and UNIGINE's Gerstner ocean read from real
  shader source — and against **Nuclear Option's ships, which genuinely float on
  a sea that is a flat plane.** DCS is the exact mirror image: **a sea with waves,
  carrying ships that do not respond to them.** Two games, opposite halves of the
  same system left unbuilt, and both defensible for the same stated reason.

§7 is unusually large relative to the note, and that is the honest ratio.

## 0. Sourcing

| Tag | Meaning |
|---|---|
| **[LUA]** | Read from DCS's shipped Lua database (build 2.9.28.26283, July 2026) via the datamine described in [`dcs_world.md`](dcs_world.md) §0. Primary. |
| **[MIZ]** | DCS's mission-file weather schema, read from `pydcs`. Primary. |
| **[COMMUNITY]** | Forums, mods, settings guides, reporting. **Most of this note's rendering claims are this tier**, which is exactly the problem. |
| **[reconstructed]** | My inference. §4 is entirely this. |

**There is no [ED] tier in this note**, and that is the single most important
sentence in it. Every other note in this directory about a rendering system —
[`unigine_clouds.md`](unigine_clouds.md), [`rdr2_atmospherics.md`](rdr2_atmospherics.md),
[`sea_of_thieves_water.md`](sea_of_thieves_water.md),
[`source2_rendering.md`](source2_rendering.md), [`dcs_clouds.md`](dcs_clouds.md) —
rests on shader source, a conference talk or a vendor white paper. This one has
none of those.

---

## 1. The evidence for the absence

Four independent signals, none conclusive alone, consistent together:

**No technical publication.** ED published *Volumetric Weather: DCS History
Excursion and Techniques* for clouds and fog — history, failure post-mortem,
technique, remaining work. Searching their newsletters, file server and site for
an equivalent on water returns nothing. Water is not mentioned in the volumetric
weather paper either, despite fog, rain, snow and dust all being in scope.

**Not on the 2026 roadmap.** ED's 2026 roadmap covers the Vulkan transition,
weather, explosions and visual effects, a new navmesh for AI ground pathfinding,
an ATC overhaul, and a substantial naval **content** push — Supercarrier Ready
Room, improved landing aids, dynamic deck crew, a large set of US Navy and
Imperial Japanese ships "to an exceptionally high level of detail"
**[COMMUNITY]**. **Ships, yes. The water they sit in, no.**

**The community's own framing.** The highest-ranked non-tutorial results for DCS
water are threads titled *"Better water rendering"*, *"Better ocean and general
water representation"*, *"Improved Ship's Wakes"*, *"Sea State / Wave height ME"*,
*"Ability to change sea state independently from wind speed"*, and *"Supercarrier
and all ship water wake/wave adjustments"* — all in **wish-list** forums, several
long-running **[COMMUNITY]**. For clouds, the equivalent searches return release
coverage and technical explainers. A community asks for what it does not have.

**The last visible step change was 2017.** The most-cited "new water" material is
a *New wave and water effects in DCS World 2.1* video from the 2.1 era
**[COMMUNITY]**. Since then, water improvements appear as **per-map** work —
Marianas' 2026 update lists "refined coastlines" and "upgraded shoreline
visuals", and offshore depth shading is described as newly convincing
**[COMMUNITY]** — rather than as engine work.

That last point is the structurally interesting one, and §3.3 returns to it:
**DCS's water improvements arrive through the terrain pipeline, per map, not
through the renderer.**

---

## 2. Sea state is one number, and that number is the wind

**[MIZ]** The mission weather block ([`dcs_clouds.md`](dcs_clouds.md) §9 has it in
full) contains **no sea-state, wave-height, swell-direction, swell-period or
fetch field**. The complete set of water-relevant inputs is:

```lua
wind = { atGround = { speed, dir }, at2000 = {...}, at8000 = {...} }
```

and community reporting is unanimous that **the only way to raise the waves is to
raise the wind speed at sea level** **[COMMUNITY]**. There are multiple
long-standing wish-list threads asking for the two to be separated, and one of
them states the objection correctly: *"currently sea state is linked to the wind
speed, but in real life the sea state would vary depending on the fetch of the
wave."*

**This is a real and defensible simplification, and it is worth being precise
about what it costs.** Wind speed alone does determine a *fully developed* sea, so
`waveHeight = f(windSpeed)` is the right first-order model. What it cannot
represent:

- **Swell** — waves that have outrun the weather that made them. A long,
  low swell under light winds is the single most common real sea state a naval
  aviator meets, and it is unrepresentable here: light wind means flat water.
- **Fetch and duration** — a 20 kt wind that started ten minutes ago over 5 km of
  water produces nothing like the same sea as the same wind blowing 500 km for two
  days.
- **Sea and swell from different directions**, which is what makes a real deck
  move in more than one axis.

And it has a gameplay consequence that is not obvious from the graphics: **you
cannot author a rough sea for a carrier recovery without also authoring the strong
surface wind that makes the recovery easier.** The two knobs a mission designer
would want to oppose are welded together, and welded the wrong way round — more
wind means both a worse deck and more wind over the deck. Every wish-list thread
on this is really about that.

The coupling is also the reason the shared-parameter complaint appears in the
*wind* threads too: raising sea-level wind raises it aloft as well, because the
three wind altitudes interpolate **[COMMUNITY]**.

---

## 3. What the shipped data says about ships

### 3.1 There are no wake parameters. Anywhere.

**[LUA]** I read the ship unit definitions looking for wake, trail, foam, spray,
splash or wave fields. **There are none.** The complete set of hull-and-motion
data a DCS ship carries:

| Field | ALBATROS | PERRY | TICONDEROG | CVN_75 | HarborTug |
|---|---|---|---|---|---|
| `shipLength` (m) | 65 | 124.3 | 160.7 | 332.9 | 20 |
| `Length` (m) | 71 | 137.5 | 172.34 | 332.9 | 20 |
| `Width` (m) | 10.2 | 14 | 18.4 | 96 | 7.7 |
| `Tail_Width` (m) | 7.5 | 13.5 | 15 | 22 | 8 |
| `draft` (m) | 4 | 7.5 | 12 | 13 | — |
| `mass` (kg) | 1 120 000 | 4 100 000 | 9 590 000 | 72 916 000 | **1 427** |
| `max_velocity` (m/s) | 15.43 | 14.92 | 15.43 | 15.43 | 13.375 |
| `economy_velocity` | 7.20 | 10.29 | 10.29 | 15.43 | 5.14 |
| `speedup` (m/s²) | 0.567 | 0.270 | 0.230 | **0.119** | 0.366 |
| `R_min` (m) | 130 | 275 | 345.6 | **665.8** | 247 |
| `Om` | 0.02 | 0.02 | 0.02 | 0.05 | 0.05 |
| `Gamma_max` | 0.35 | 1 | 1 | 1 | 0.35 |
| `X_nose` / `X_tail` (m) | 31.75 / −34.84 | 59.19 / −64.93 | 75.74 / −85.98 | 141 / −140 | 10.5 / −10.5 |

Two columns identify themselves cleanly by scaling:

- **`speedup` is acceleration in m/s²** — it falls monotonically with mass, from
  0.567 for a 1,120 t corvette to 0.119 for a 72,916 t carrier. A Nimitz taking
  ~130 s to reach 30 kt from rest is about right.
- **`R_min` is the turning radius in metres** — 130 m for the corvette, 666 m for
  the carrier, scaling with length as it must.

Two do not, and I am flagging rather than guessing (§7): **`Om`** takes only the
values 0.02 and 0.05 across every ship, and the *tug* and the *carrier* share
0.05 while the corvette, frigate and cruiser share 0.02 — so it is not a
size-scaled rate. **`Gamma_max`** takes only 0.35 and 1, splitting small craft
from large, which is consistent with a maximum heel angle but not established.

One data oddity worth recording in passing, because this directory has a habit of
catching them: **`HarborTug.mass = 1427`** against a 20 m hull, where every other
ship's mass is in kilograms and correct. A 20 m harbour tug is roughly 300,000 kg.
This is the same class of shipped unit inconsistency as the millimetre/metre mix
that [`ruse.md`](ruse.md) §7.1 catches in R.U.S.E.'s tunable constants.

### 3.2 The ships are kinematic, not floating

The consequence of that table is the important part. **`R_min`, `speedup`,
`max_velocity` and a heel limit are the parameter set of a body being *driven
along a path*, not of a body being *simulated in a fluid*.** There is no
displacement, no buoyancy volume, no metacentric height, no hull drag
coefficient, no righting moment — and no per-compartment anything.

Compare what Nuclear Option's ships carry
([`nuclear_option.md`](nuclear_option.md) §11), in a game with a fraction of the
budget:

| | Nuclear Option | DCS |
|---|---|---|
| Body | **one rigidbody + N compartments as buoyancy probes** | kinematic, path-following |
| Flotation | `lerp(ρ_air, ρ_water, submerged)·g·V` per probe — a smooth waterline | **draft is a number** |
| Drag | **anisotropic quadratic** (lateral ≫ longitudinal — *that is the keel*) | `speedup` and `max_velocity` |
| Turning | emergent from thrust and hull drag | **authored `R_min`** |
| Heel / list | **off-centre buoyancy from flooded compartments** | **`Gamma_max`, a limit** |
| Damage | flooding against an exhaustible damage-control pool, propagating to neighbours | hit points (`life`) + `DM` |
| Death | **by posture** — below the surface, heeled past 60°, bow up past 14° | `life` reaches zero |
| Sea surface | **a flat plane** | waves driven by wind |

**Both games left half of the ship–sea problem unbuilt, and they left opposite
halves.** Nuclear Option simulates the ship properly and does not give it a sea to
float on. DCS renders a sea and does not give the ships anything to float with.

### 3.3 Water quality is a map asset, not an engine feature

**[COMMUNITY]** DCS's per-map water differences — Marianas' shallow lagoon
gradients, South Atlantic's cold grey ocean, the newer maps' "convincing" offshore
depth shading — plus the 2026 Marianas update's "refined coastlines" and "upgraded
shoreline visuals", all point the same way: **the water's appearance is largely
authored per terrain, through the terrain toolchain, rather than being a global
renderer feature that improves everywhere at once.**

That fits ED's own framing elsewhere: they describe the **terrain creation tool
technology as "equally important as the Scene Renderer itself"**
([`dcs_world.md`](dcs_world.md) §10.4). It also explains the observed pattern in
§1 — no engine-level water milestone since ~2017, but water that keeps looking
better on new maps.

**It is also why sea state cannot easily become a mission parameter.** If depth
maps, shore blending and water colour are baked terrain assets, then the parts of
"how the sea looks" that a mission designer would most want to vary are on the
wrong side of the build.

### 3.4 One more shipped detail: shallow water stops ships

**[COMMUNITY]** There is a long-running bug report that the sea contains "many
patches of very shallow water, stopping ships in their tracks", making shipping
routes awkward to author. Combined with `draft` being a real per-ship number and
`distFindObstacles` appearing in the ship data **[LUA]**, this says the naval
navigation layer **does** read a bathymetry field and **does** compare it against
draft.

So the water has a real depth field that the simulation queries — it is just not
one that the *surface* simulation uses for anything interesting like wave
shoaling. **The data exists; the coupling does not.**

---

## 4. How the surface and the wake must work [reconstructed]

**No evidence about DCS's code follows.** This is what the constraints permit,
and it is here only so §5's comparison has something to compare.

**The surface.** A wind-driven, tiling, animated height field — most plausibly a
small sum of directional waves (Gerstner or a simple sinusoidal stack) whose
amplitude, wavelength and choppiness are driven by the single wind-speed
parameter, tiled over the visible sea with distance-based LOD, plus normal-map
detail at close range and a switch to normal-only far out. The strongest
supporting evidence is negative: **a full FFT ocean would almost certainly have
been announced**, the way ED announced the clouds, and a spectrum-based ocean
naturally exposes fetch and swell parameters — which DCS conspicuously does not
have (§2).

Shading is deferred + PBR like the rest of the renderer
([`dcs_world.md`](dcs_world.md) §10.4), with a **Water** quality setting and a
separate **SSLR** (screen-space reflection) toggle that community guides describe
as producing wet reflective surfaces such as rain-slicked carrier decks, and as
expensive enough to disable in VR **[COMMUNITY]**. A known artefact — *"SSLR on
Supercarriers produces block effect at bow"* **[COMMUNITY]** — is the classic
screen-space failure at a silhouette edge where the reflected sample leaves the
screen.

**The wake.** Given §3.1 — no wake data on any ship — a wake must be generated
from what *is* there: `shipLength`, `Width`, `Tail_Width`, `X_nose`, `X_tail`,
`draft` and current speed. The plausible construction is the standard one:
a bow-wave and stern-wake **decal or projected texture** written into the water's
normal/foam channels, scaled by hull dimensions and modulated by speed, trailing
behind the hull along its recent path history, with foam fading over a fixed
lifetime. Community threads titled *"Ship wake still broken"* and *"Improved
Ship's Wakes"* **[COMMUNITY]** are consistent with a decal-based system whose
alignment and persistence are the usual failure points.

**What it is almost certainly not**: a wake that displaces the actual water
surface, that interacts with other ships' wakes, or that persists at Kelvin-wake
angles over the kilometres real wakes do. The absence of any per-ship tuning data
is itself the argument — a wake system with real fidelity would have per-hull
parameters, because a carrier and a patrol boat do not make the same wake, and
those parameters would live next to `draft`.

---

## 5. The comparison, which is the point

Against the three oceans already in this directory
([`sea_of_thieves_water.md`](sea_of_thieves_water.md) covers SoT, CS2 and — in
§11, from real SDK shader source — UNIGINE), plus Nuclear Option's:

| | **Sea of Thieves** | **UNIGINE** | **Nuclear Option** | **DCS World** |
|---|---|---|---|---|
| Surface model | **FFT spectrum** | **Gerstner sum** (read from source) | **flat plane** | wind-driven height field **[reconstructed]** |
| Sea state control | full spectrum params | wave params | n/a | **wind speed only** |
| Swell separate from wind sea | yes | partly | n/a | **no** |
| Foam | dedicated system | yes | particles | decals **[reconstructed]** |
| Fake SSS / translucency | **yes, a signature feature** | yes | n/a | not established |
| Shore interaction | yes | yes | n/a | authored per map |
| Buoyancy of vessels | **yes — the ship is the game** | sample the height field | **yes — probes, flooding, list** | **no — kinematic** |
| Wake | yes | yes | **particles** | decals, no per-ship data |
| Published source | GDC talk | **SDK shader source** | decompiled C# | **nothing** |

Three readings.

**One: fidelity follows the camera, and both "bad" oceans are correct.** Nuclear
Option's note states the rule explicitly — *"a medium's fidelity should be set by
what interacts with it"* — and then admits its own sea is a flat plane with
particle wakes. That is right for a game where you are in an aircraft and the
ships are things you attack. DCS is in the same position with the *opposite*
omission, and for the same reason: **you fly over the sea, and you land on one
ship that has a stabilised deck.** Sea of Thieves spends everything on the ocean
because the ocean *is* the game.

So DCS's water is not a failure of engineering. **It is the one major system whose
consumer never got demanding enough to justify the work** — and the moment that
changed (Supercarrier, 2020) the fix was to make the *carrier* good, not the sea.

**Two: the two games' omissions are exactly complementary, which is the useful
observation.** Put them together and you would have a complete system: DCS's
wind-driven surface and per-map shore work, plus Nuclear Option's buoyancy probes,
anisotropic hull drag, compartment flooding and death-by-posture. Neither team
needed both halves. **If cromwell ever wants ships that matter, this pair is the
specification** — and the cheap half is Nuclear Option's, because probes against a
height field is a small amount of code and needs no new rendering.

**Three: the coupled-parameter problem is the transferable design bug.** Sea state
welded to wind (§2) is a specific instance of a general failure: **two things a
designer needs to vary independently were derived from one another because they
are physically related.** They *are* physically related — that is what makes the
mistake attractive. The fix is not to break the physics but to make the derived
value an *override*: default `seaState = f(wind)`, and let the mission set it
directly when it wants to. Every wish-list thread in §1 is asking for exactly
that, and has been for ten years.

---

## 6. What transfers

Short, because the note is.

1. **Set a medium's fidelity by what interacts with it** — Nuclear Option's rule,
   and DCS is a second confirmation from the opposite direction. Neither game's
   sea is under-built by accident.
2. **Derived parameters need an override, not just a formula** (§5, reading
   three). `seaState = f(wind)` as the *default*, overridable per mission. This is
   the cheapest possible fix to the most-requested missing feature in a
   twenty-year-old simulator, and it generalises to every "these two are
   physically linked so I'll compute one from the other" decision.
3. **Buoyancy probes are the cheap half of ship–sea coupling.** N sample points,
   `lerp(ρ_air, ρ_water, submerged)·g·V` per probe, anisotropic quadratic drag for
   the keel. Nuclear Option gets list, trim, wallowing and death-by-posture out of
   it, on a *flat* sea. Add a height field and it works on waves for free.
4. **Watch what lives in the terrain build versus the renderer** (§3.3). DCS's
   water looks better on new maps and not on old ones because the good parts are
   baked assets. Anything you want a designer to vary at mission time must not be
   on the wrong side of the content build.
5. **A depth field the simulation queries is not the same as a depth field the
   surface uses** (§3.4). DCS has bathymetry — ships run aground on it — and the
   waves do not shoal over it. Having the data is not having the coupling.

---

## 7. What this note does not establish

Proportionally the largest section in this directory, and deliberately so.

**Essentially the entire water implementation.** No wave model (FFT? Gerstner?
sinusoidal stack? — §4 is reconstruction and says so), no mesh or LOD scheme, no
shading model, no foam system, no subsurface approximation, no reflection method
beyond the existence of an SSLR toggle, no formats, no resolutions, no costs. I
could not establish whether the surface is a projected grid, a tiled patch set or
something else.

**The wake, completely.** Whether it is a decal, a mesh, a particle system or a
write into the water's own textures; how long it persists; whether wakes
interact; whether the Supercarrier's is special-cased. §4 is inference from the
*absence* of per-ship data, which is suggestive and not evidence.

**Whether waves affect anything.** I could not establish whether the Supercarrier
deck actually pitches and heaves with sea state, whether small boats respond to
waves at all, or whether the water height field is even readable by the
simulation. §3.2's "ships are kinematic" is a strong inference from the parameter
set, **not a demonstration** — a kinematic path could still be modulated
vertically by a wave sampler, and I have no evidence either way.

**`Om` and `Gamma_max`** (§3.1). Two discrete values each, not scaling with size.
Flagged, not identified.

**Whether the water shaders are readable.** As with the clouds
([`dcs_clouds.md`](dcs_clouds.md) §15), community mods edit plain-text files under
`Bazar/shaders/` — `enlight/watercompose.hlsl` is named directly in a shader-edit
thread **[COMMUNITY]**, and one mod advertises "clear water" edits — so **at least
part of the water shading ships as source.** DCS is not installed on this machine
(~300 GB; see [`dcs_world.md`](dcs_world.md) §0), and no mirror exists on GitHub.
**If the install ever exists locally, `Bazar/shaders/` would convert most of §4
from reconstruction into fact, and this note is the one in the set that would
benefit most.**

**The negative claims themselves are the weakest kind of evidence.** "ED never
published anything about water" is an argument from absence: I searched their
newsletters, file server and site and found nothing, and the community's wish-list
framing corroborates it. But absence of a document is not proof of absence of
engineering, and a talk or article I did not find would change §1 substantially.

**Version currency.** Ship data is build 2.9.28.26283 (July 2026). The roadmap
reading is early-2026 coverage. Per-map water work is continuous, so §3.3's
picture is a moving target.
