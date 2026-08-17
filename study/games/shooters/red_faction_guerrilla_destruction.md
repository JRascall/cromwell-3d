# Red Faction: Guerrilla — GeoMod 2.0, read from the Re-MARS-tered install

How Volition made every man-made structure on Mars fall down for real in 2009,
what the shipped data actually contains, and what each of the renderer, the
physics, the network and the budget pay for it.

The one-line answer, up front, because it reframes everything below:

> **A GeoMod 2.0 building is not a mesh with damage states and it is not a
> volume that gets cut. It is a *graph* — a few thousand rigid subpieces joined
> by links that each carry a cross-sectional area — and destruction is the act
> of deleting edges and re-running connectivity.** Everything else in the
> system exists to make that graph affordable: a rendering trick that draws one
> mesh until the graph is disturbed, a spatial tree so a blast touches only the
> pieces it reaches, an amortised background pass that walks a few hundred
> objects a frame, and a *deletion* budget with a dust puff over it so pieces
> that cannot be afforded appear to crumble rather than vanish.

That makes it the third distinct answer in this directory to the same question.
[`bad_company_2_destruction.md`](bad_company_2_destruction.md) says *nothing is
ever cut — every destructible steps between authored states*.
[`rainbow_six_siege.md`](rainbow_six_siege.md) says *the hole is genuinely cut
at runtime by arbitrary polygons, and nothing else in the world is
destructible*. RFG says **neither: pre-fracture everything offline, then
simulate the connectivity**. §12 reads the three against each other, and the
short version is that RFG bought *scale* (the whole city, all at once) by
giving up *arbitrariness* (you can never make a hole the artist did not
pre-cut), which is exactly the trade Siege made in the opposite direction.

> **Read alongside:** [`bad_company_2_destruction.md`](bad_company_2_destruction.md)
> and [`rainbow_six_siege.md`](rainbow_six_siege.md) — same problem, other two
> corners. [`voxel_terrain.md`](../../topics/world/voxel_terrain.md) /
> [`space_engineers.md`](../space/space_engineers.md) for the fourth corner,
> the world as real editable volume. [`lod_systems.md`](../../topics/world/lod_systems.md)
> for the DLOD idea in §4, which is an LOD system whose switch condition is
> *damage* rather than distance.

---

## 0. Sourcing

The published record on GeoMod 2.0 is **thin and almost entirely non-technical**
— Volition gave two GDC talks in 2009 and neither is a destruction-tech talk
that survives on the Vault, and no paper, course chapter or white paper exists.
What redeems the note is that the retail install is unusually legible: the
executable ships **full source paths and RTTI type names**, the game's tuning
data ships as **self-documenting XML tables where Volition wrote the
description of every field**, and the destructible-object format has been
reverse-engineered by the modding community well enough to parse from scratch.

So this note is mostly **read out of the build**, in the manner of
[`ruse.md`](../strategy/ruse.md) and [`simcity3000.md`](../strategy/simcity3000.md),
and every claim carries a tag.

| tag | source | strength |
|---|---|---|
| **[CHUNK]** | The shipped `.cchk_pc` destructible-object files, parsed with a reader written for this note. **Census: 18 archives (single-player world, DLC and all shipped MP maps), 532 unique chunk files, 462 of them carrying destruction data, 888 destroyables, 279,262 subpieces, 960,343 links, 26,281 DLODs.** Numbers in §1–§4 are counted, not estimated. | **Very strong** — this is the shipped data. |
| **[XTBL]** | `table.vpp_pc`: 423 XML tables, each carrying its own `TableDescription` block in which **Volition documented every field for their own designers**. Quotes tagged `[XTBL]` are Volition's words, in the shipped build. Primary source for §3, §6, §7, §8. | **Very strong.** Better than a talk: it is the authored contract, not a retrospective. |
| **[EXE]** | `rfg.exe` (25 MB, build `cs:4931`): 109,882 extracted strings, including **250 absolute source paths** under `C:/unit4projects/rfg/root/code/...`, 1,220 RTTI class names, assertion text, console-command usage strings and log format strings. | **Strong for structure**, weak for behaviour — a name proves a thing exists, not what it does. |
| **[RFGM]** | [`rfg-modding/RFGM.Formats`](https://github.com/rfg-modding/RFGM.Formats) — the community's chunk-format reader. Its field comments **preserve Volition's own struct names** (`rfg_rbb_node`, `rfg_subpiece_base`, `rfg_links_base`, `rfg_dlod_base`, `rfg_destroyable_base_instance_data`), which is why §1 can name types rather than offsets. Checked before writing a reader, per the standing rule. | **Strong** for layout; its own TODOs mark what is still unread. |
| **[RINGER]** | The Ringer, *"The Destruction (and Reconstruction) of Destructible Environments in Video Games"*, 2 Feb 2024 — carries on-record quotes from **Alan Lawrance** (Volition) and **Matt Gawalek** (gameplay programmer on RFG). The only published source with a programmer describing the mechanism. | Strong for intent and for one hard admission about memory. |
| **[CBS09]** | Eric Arnold (Senior Developer, Volition), Q&A with CBS News GameCore, 30 June 2009. | Strong for intent; no implementation detail. |
| **[GDC10]** | Luke Schneider (lead technical / lead multiplayer designer, Volition), *"Multiplayer Level Design in Red Faction Guerrilla"*, GDC 2010. Design, not tech, but it is the only Volition GDC session about RFG on the Vault. | Medium. |
| **[inferred]** | Our reading. Marked inline wherever it appears. | — |

**Version note.** This is the 2018 *Re-MARS-tered* build (Kaiko/THQ Nordic), not
the 2009 original. The destruction data is the same content re-shipped — the
chunk format is still `version 56 / source version 20` — but the *engine around
it* was updated, and one of the updates is visible in the strings: the Havok
path is `libmodules/havok/hk2014_2_5_r1/Source/Physics2012/...`, i.e. **Havok
2014.2.5 running the Physics2012 API**, where the 2009 game shipped a
2008-vintage Havok. Where a claim could plausibly differ between the two builds
it is flagged.

---

## 1. What a destructible object actually is

Buildings ship as a pair: `name.cchk_pc` (CPU) and `name.gchk_pc` (GPU
vertex/index data), packed inside per-zone `.str2_pc` containers. The CPU file
is a 656-byte header, a render-mesh config, textures and materials, a list of
"general objects", and then a **destruction block** whose offset the header
gives directly. Inside that block sits one or more *destroyables*. **[CHUNK]**

A destroyable is six arrays and nothing else **[RFGM]/[CHUNK]**:

| Volition's type | count in one large building | what it is |
|---|---|---|
| `rfg_subpiece_base` | 4,326 | the rigid pieces |
| `rfg_subpiece_base_extra_data` | 4,326 | each piece's Havok shape + physical material |
| `rfg_links_base` | 15,985 | the joints between pieces |
| `rfg_dlod_base` | 536 | render groups (§4) |
| `rfg_rbb_node` | ~2,580 | a recursive-bounding-box tree over the pieces |
| `rfg_destroyable_base_instance_data` | 1 | the per-instance mutable state buffer |

*(the row is `0102oasis_mun_bldg_a` — a mid-sized municipal building, not the
biggest thing in the game)*

### 1.1 The subpiece — 64 bytes

```
Vector3 bmin, bmax;          // local AABB
Vector3 position;            // rest position
Vector3 center_of_mass;
float   mass;                // see §2 — it is a volume
uint    dlod_key;            // (dlod_index << 16)
uint    links_offset;        // into a shared ushort array
byte    physical_material_index;
byte    shape_type;
byte    num_links;
byte    flags;
```

Two things are worth noticing immediately. **There is no orientation** — a
subpiece has a position and an AABB and that is all, because until it breaks
free it never moves, and once it breaks free Havok owns its transform. And
**`num_links` is a byte**, which caps a piece at 255 neighbours; the measured
maximum across the whole game is **244**, so the cap was chosen after looking at
real content and is very nearly hit. **[CHUNK]**

#### The anchor bit

`flags` is undocumented anywhere, but one bit is identifiable from the content
alone. Profiling every set bit against the piece's height within its own
structure, over 349 destroyables **[CHUNK]**:

| bit | pieces | mean height in structure | median |
|---|---:|---:|---:|
| 0x02 | 126 | 0.449 | 0.414 |
| 0x04 | 4,377 | 0.383 | 0.414 |
| 0x08 | 1,550 | 0.456 | 0.462 |
| **0x20** | **2,195** | **0.174** | **0.095** |
| 0x80 | 6,152 | 0.407 | 0.406 |

Every other bit sits at the statistical middle of the building. **0x20 sits on
the floor.** Characterised further against everything else in one map's worth of
buildings:

| | flag 0x20 | all others |
|---|---:|---:|
| count | 550 | 18,546 |
| **median volume** | **0.616 m³** | **0.084 m³** |
| mean volume | **8.81 m³** | 0.14 m³ |
| mean link degree | 9.50 | 7.26 |
| fraction in the bottom 2% of the structure | **49.1%** | 4.0% |
| top materials | Concrete-**Strong** 67%, Steel-**Strong** 16%, Steel-**SuperStrong** 5% | Concrete-Strong 30%, Concrete-Normal 24% |

> **A tiny set of unusually large, unusually strong, unusually well-connected
> pieces at the bottom of the structure — a median 1.1% of a large building's
> subpieces.** `[inferred]`, but it is hard to read as anything but *the
> foundation: the pieces the connectivity test treats as ground.*

Two honest caveats. **75% of destroyables with ≥200 pieces carry at least one
0x20 piece; 25% do not** — and the exceptions are exactly the shapes where
"anchored at the bottom" is the wrong model: bridges (`0101bridge_refinery_a`,
1,607 pieces), long pipes, perimeter walls, a wrecked aircraft lying on the
ground. So there must be a second route to "this is supported", most likely the
`rfg_mover`'s own **base chunk** — the thing the assertion *"Invalid rfg_mover —
base chunk has 0 mass"* **[EXE]** is about. And **no anchor piece uses an
`Indestructo` material**, which is the design working: the foundation is
supposed to be knockable-out.

### 1.2 The link — 16 bytes, and it is the whole design

```
int   yield_max;   // ← zero in every link in the shipped game
float area;        // cross-sectional area of the joint, m²
short obj[2];      // the two subpieces it joins
byte  flags;
```

That is the entire structural model. A joint between two pieces of a building
is **one float: how much material is in it**. Not a spring, not a constraint,
not a Havok joint — a number that says how big the contact is.

Measured across all 960,343 links in the game: **median joint area 0.102 m²**,
5th percentile 0.012 m², 95th percentile 0.468 m², maximum 78.5 m². The
distribution is exactly what you would expect if the areas were computed
geometrically from the fracture at bake time and never touched by hand. **[CHUNK]**

Link `flags` is essentially binary in practice: **778,359 links have flag 1 and
181,984 have flag 0**, an 81/19 split. `[inferred]` — the most likely reading is
that flag 1 marks a load-bearing/structural link and flag 0 a merely-adjacent
one, which would match `structural` being a per-material flag in
`rfg_materials.xtbl` (§2), but nothing in the strings names the bit.

### 1.3 The graph, measured

| | |
|---|---|
| destroyables in the shipped game | **888** across 462 chunk files |
| subpieces | **279,262** |
| links | **960,343** |
| links per subpiece | **3.44** |
| per-piece link *degree* (each link counted at both ends) | mean **6.88**, median 6, 95th pct 14, max 244 |
| pieces with degree 1 (a single attachment) | **3.0%** |
| median destroyable | **52 subpieces** |
| largest destroyable | `mp_large_bridge` — **5,374 subpieces, 23,496 links** |
| largest single-player building | `0101office_corp_large_a` — **4,381 subpieces, 16,668 links** |

The degree sum is 1.92 M against 960,343 links — exactly 2×, which is the
consistency check that the per-subpiece link lists really are the same edges
seen from both ends, i.e. **the graph is stored twice on purpose: once as an
edge list for the solver, once as an adjacency list for the flood fill.** That
is the classic layout for "I need to iterate edges *and* walk neighbours from a
given node, and I cannot afford to build the adjacency at load". **[CHUNK]/[inferred]**

### 1.4 The RBB tree

`rfg_rbb_node` is 20 bytes: `int num_objects`, a **six-`short` quantised AABB**,
and an offset. Internal nodes have `num_objects == 0` and two children packed
contiguously; leaves carry `num_objects` 4-byte subpiece indices. **[RFGM]**

Measured over the whole game: **68,604 leaves for 279,262 subpieces — 4.07
subpieces per leaf**, maximum depth **11**. **[CHUNK]**

The name is not Havok's. `volition/vlib/code/util/recursive_bbox.cpp` and
`volition/rfg/code/destruction/rfg_rbb_tree.cpp` are both in the string table,
and `rl_fp_rbb_tree` is a *renderer* class alongside `rl_fp_kdtree` **[EXE]** —
so **the same recursive-bounding-box structure serves destruction queries and
render culling**, built once per destroyable at bake time. The AABBs being
quantised to 16-bit is the tell that this tree is expected to be resident in
large numbers: 12 bytes of bounds instead of 24 across ~100k nodes.

A blast at a point therefore costs a descent of ~11 levels to a handful of
4-piece leaves, not a scan of 4,000 subpieces. That is the
[`spatial_queries.md`](../../topics/agents/spatial_queries.md) rule — *do less
work before you do it faster* — applied to destruction.

---

## 2. What is *not* in the file, and why that is the interesting part

Three fields that you would expect to be authored are zero in every shipped
destroyable:

1. **`yield_max` is 0 in all 960,343 links.** Not "mostly zero" — zero, everywhere. **[CHUNK]**
2. **The destroyable's `mass` is 0** in every header. **[CHUNK]**
3. **The subpiece `mass` is not kilograms.** Over 17,849 subpieces the ratio of
   the stored value to the piece's own AABB volume has **median 0.366, mean
   0.359, 10th–90th percentile 0.095–0.594, and exceeds 1.0 for 0.4% of
   pieces** — i.e. the value is a real mesh volume sitting inside its box, in
   m³. **[CHUNK]**

The reason all three are empty is one sentence in Volition's own table
documentation. `rfg_materials.xtbl` defines `LinkStrength` as **"Link strength
per unit area. 100 is the base reference value"**, and `Density` as *"Density of
a material (kg / m³), this directly relates to how the object will react in the
wonderful world of havok. **object weight = volume * density**"*. **[XTBL]**

So:

> **The chunk file ships the *geometry* of the structure — how big each piece is
> and how much material holds each joint together — and the material table
> supplies the *physics*. Yield is `LinkStrength × area` and mass is
> `density × volume`, both resolved at load.**

The consequence is the reason to care. **Every building in the game can be
rebalanced by editing one 17 KB XML file, with no re-bake of any asset.** A
designer who decides that concrete is too weak edits one row and 28,929
subpieces across the city change behaviour. Compare the alternative — baking
absolute yields into 960,343 links — which would make the same change a
content-pipeline run over the entire game.

Nothing about this is exotic; it is *late binding of the tuning value*, and it
is the single most transferable idea in the note.

### 2.1 The material table, in full

There are **27 physical materials in the whole game**, of which **21 appear in
the shipped world**. **[XTBL]/[CHUNK]**

| material | Link/area | density kg/m³ | brittleness | friction | flags | subpieces using it |
|---|---:|---:|---:|---:|---|---:|
| Concrete-Normal | 235 | 2250 | 0.9 | 0.9 | structural, crushable, allow_jitter, allow_regrow | 28,929 |
| Composite-Normal | 250 | 1750 | — | .55 | flammable, structural, crushable, allow_regrow | 28,330 |
| Steel-Normal | 300 | 2500 | 0.5 | 0.5 | structural, electrifiable, allow_regrow | 22,605 |
| Carbon-EDF | 300 | 2250 | 2.0 | .55 | structural, flammable | 13,229 |
| Steel-Strong | 700 | 2750 | 0.25 | 0.5 | structural, electrifiable, allow_regrow | 11,285 |
| Steel-SuperStrong | 4000 | 2750 | 0.1 | 0.5 | structural, electrifiable, allow_regrow | 9,309 |
| Steel-StrongLight | 800 | 1750 | 0.5 | 0.5 | structural, electrifiable, allow_regrow | 6,959 |
| Concrete-Strong | 290 | 2500 | 0.75 | 0.9 | structural, crushable, allow_jitter, allow_regrow | 6,340 |
| Composite-Strong | 280 | 2000 | 0.75 | .55 | flammable, structural, allow_regrow | 3,189 |
| Electronics-Weak | 200 | 1500 | 3.0 | .5 | bullet_damage, melee_damage, flammable, electrifiable, crushable | 2,574 |
| Steel-Weak | 220 | 2000 | 0.75 | 0.5 | melee/bullet damage, structural, electrifiable, allow_regrow | 1,840 |
| Composite-Weak | 200 | 1500 | 2.0 | .55 | bullet/melee damage, flammable, structural, crushable | 1,522 |
| EOS-Glass | 250 | 500 | 5.0 | 0.2 | crushable, bullet/melee damage, transparent | 599 |
| Stone-Normal | 250 | 2500 | 0.75 | 0.8 | structural, crushable, allow_regrow | 496 |
| Concrete-Weak | 200 | 2000 | — | 0.9 | bullet_damage, structural, crushable, allow_jitter, allow_regrow | 362 |
| Glass-Weak | 25 | 2500 | 5.0 | 0.2 | **shatter**, bullet/melee damage, transparent, crushable | 237 |
| Stone-StrongLight | 400 | 1750 | 0.5 | 0.8 | structural, crushable, allow_regrow | 96 |
| Stone-Indestructo | 2000 | 500 | — | 0.8 | **indestructible** | 59 |
| Indestructo | 2000 | 500 | — | 0.7 | **indestructible** | 31 |
| Plastic-Normal | 250 | 1750 | 0.25 | 0.6 | flammable, structural, crushable | 6 |
| *(unused in world)* | | | | | Cloth, Flesh, Ice-Normal, Plastic-Weak, Rubber, Carbon, DEFAULT | 0 |

Three readings.

**The strength range is 160:1 and the density range is 6:1.** `Glass-Weak` at
LinkStrength 25 against `Steel-SuperStrong` at 4000. A material system where
the *interesting* axis is how well it holds on rather than how heavy it is.

**`Brittleness` is not what the name suggests.** Volition define it as *"Controls
how much energy is transferred across broken links when hit by non-explosive
damage. **Higher value = less energy transferred**"* **[XTBL]**. It is a damage
*propagation damper*, i.e. the knob that decides whether a sledgehammer swing
stops at the panel you hit or ripples down the wall. Glass at 5.0 (energy dies
instantly, the pane just goes), steel at 0.1–0.5 (the shock carries), concrete
at 0.75–0.9. **This is the parameter that makes different materials feel
different under the same weapon**, and it has nothing to do with strength.

**`Indestructo` exists and is used 90 times.** A shipped world where "everything
is destructible" still needs 90 pieces that are not, which is worth remembering
before promising the same thing.

---

## 3. The stress system

`rfg_stress_controls.xtbl` is 3.4 KB and is the whole gravity-collapse
controller. Four rows, six fields, and Volition's descriptions attached. **[XTBL]**

| | Default (single-player) | Multi | Wrecking Crew Low | Wrecking Crew High |
|---|---:|---:|---:|---:|
| `Objects_Per_Frame` | 400 | 400 | 800 | 800 |
| `Repeat_Delay_Min` (ms) | 100 | 100 | 50 | 50 |
| `Repeat_Delay_Max` (ms) | 5000 | 5000 | 700 | 400 |
| `Force_Scalar` | 0.12 | 0.1 | 0.08 | 0.2 |
| `Parent_Yield_Scalar` | 1.4 | 0.5 | 0.5 | 0.5 |

Volition's own field documentation, verbatim:

- `Objects_Per_Frame` — *"Controls the number of objects the stress system
  processes per frame. Higher numbers will make the overall pass happen faster
  but each frame will take longer to process."*
- `Repeat_Delay_Min` / `Max` — *"Minimum / Maximum millisecond delay before
  running stress again after a pass is finished."*
- `Force_Scalar` — *"This controls how strong the stress is overall. Higher
  numbers will make things fall apart faster."* (default 0.2)
- `Parent_Yield_Scalar` — *"This is used to break links to parents when they are
  heavily damaged. Higher values make small pieces break off easier."* (default 2.0)

Four things fall out of that table, and they are the whole architecture of the
system.

**Stress is not evaluated per frame, and it is not evaluated on demand.** It is
a **background pass with a fixed per-frame quota and a randomised
inter-pass delay of 100–5000 ms**. A building that has just been hit does not
get a stress solve that frame; it gets one whenever the rolling pass reaches
it. That is why RFG's buildings sag, groan and *then* go — the delay is not a
dramatic flourish, it is the scheduler.

**The cost is bounded by construction, not by a budget check.** 400 objects per
frame is a hard number; the pass simply takes as many frames as it takes. This
is the same idea as `broken_arrow_vision.md` §5's "search cadence is membership
in a set, not a timer", reached from the other side — *bound the work per frame
and let latency float, rather than bounding latency and letting work float*.

**Multiplayer runs the stress system 17% weaker and with a `Parent_Yield_Scalar`
of 0.5 against single-player's 1.4** — nearly 3× less willing to shed small
pieces from a damaged parent. `[inferred]`: this is a determinism and bandwidth
decision as much as a feel one. Every piece that breaks off is a networked
event (§10), and small pieces breaking off spontaneously are the events most
likely to differ between clients.

**Wrecking Crew — the score-attack mode — ships its difficulty as a stress
setting.** `wrecking_crew_challenges.xtbl` gives every round a `stress_level` of
`Low` or `High` (26 rounds Low, 4 High **[XTBL]**), and those name the two
Wrecking Crew rows above: `Force_Scalar` 0.08 versus 0.2, a **2.5× swing in how
eagerly the world collapses under its own weight**, with the repeat delay
tightened from 5 s to 0.4–0.7 s so the world reacts almost immediately. A game
mode whose difficulty dial is *a physics constant* rather than a score
multiplier is a rare and good idea.

### 3.1 What the runtime is allowed to remember

Before guessing at the algorithm it is worth pinning down how much state it can
possibly have, because that is recoverable exactly.

Every destroyable carries a `rfg_destroyable_base_instance_data` — a single
contiguous mutable buffer, sized at bake time, that is the entirety of one
building's runtime damage state. Regressing its `DataSize` against the counts
over **277 destroyables gives a perfect fit with zero constant term**, and
checking the buffer's own three internal offsets against that layout matches on
**288 of 288 destroyables, with no mismatches** **[CHUNK]**:

```
[ 36 B × num_subpieces ][ 4 B × num_links ][ 4 B × num_links ][ 12 B × num_dlods ]
  ^ objects_offset = 0    ^ (inside the        ^ links_offset      ^ dlods_offset
                            objects region)
```

For `0102oasis_mun_bldg_a` that is **283 KB of mutable state for one building**;
for the whole shipped set it would be roughly 15 MB if every destructible in the
game were instantiated at once, which is of course exactly why the streaming and
regrow behaviour in §9 exists.

Three readings, and they constrain the solver hard.

**The first 4 bytes per link sit *inside* the objects region, immediately after
the 36-byte-per-subpiece array — which is the size of the per-subpiece
neighbour lists** (2 bytes per half-edge × 2 ends). The static file already
carries that adjacency; the instance buffer carries a **second, writable copy**.
That is only worth paying for if the adjacency is *edited* — i.e. **breaking a
link physically removes it from both endpoints' neighbour lists**, so the flood
fill in step 4 below never has to test whether an edge is still alive. It walks
a list that has already shrunk.

**The link's own mutable state is 4 bytes and no more.** That is not enough for
"accumulated load *and* resolved yield". So `[inferred]`: the four bytes are the
link's **remaining strength** — `LinkStrength × area` resolved at load into the
empty `yield_max` slot (§2) and decremented by damage — and **the load side is
recomputed by each stress pass rather than stored**. Which is consistent with
stress being a *pass* with a repeat delay rather than an accumulator, and it is
why a building that has been sitting damaged for a minute does not slowly drift
into collapse.

**36 bytes per subpiece** is not enough for a full transform (48) and is far more
than a broken/intact bit. `[inferred]` it is the piece's live physics identity —
resolved mass, a Havok body handle, current state — since the moment a piece
detaches it stops being described by the static array.

### 3.2 What the solver appears to do

`[inferred]`, from the layout above, the field names, and the effect vocabulary
in §7 — no source or talk states it:

1. For each destroyable in the frame's quota of 400, walk the subpiece graph and
   accumulate the load each link carries — the weight resting on it, scaled by
   `Force_Scalar`.
2. Compare that load against the link's remaining strength. The
   `stress_container` effect event is sized by *"the load/yield **values**"* as a
   **"Ratio"** **[XTBL]** — direct evidence that the runtime has both numbers in
   hand during the pass and that their quotient is a first-class quantity,
   because the creaking noise is keyed off it.
3. Break links whose ratio exceeds 1 — removing them from both neighbour lists —
   and additionally break links to a parent when the parent is heavily damaged,
   scaled by `Parent_Yield_Scalar`.
4. Flood-fill the (now shorter) adjacency lists from the anchor set — the 0x20
   pieces of §1.1, plus whatever the mover's base chunk contributes. **Any
   component that no longer reaches an anchor becomes free rigid bodies**, minus
   whatever the load balancer (§6) declines to afford.

The one part that is *not* inference is step 2's quantity, and it is the part
worth stealing: **the solver's internal ratio is exported as an authoring
signal.** The sound designer's creak-volume curve and the programmer's failure
test read the same number.

### 3.3 So what happens when you take out the ground floor

Both halves of the model fire at once, which is why it is the effective attack
and why it looks right.

**Connectivity goes first.** The anchor set is a median **1.1% of a large
building's pieces** (§1.1) and it is concentrated in the bottom 2% of the
structure. Destroy those and there is nothing left for the flood fill to reach:
the whole remaining component detaches in one pass. Not "the building is
scripted to fall" — there is simply no longer a path from any piece to ground.

**And the load path was already the tightest thing in the building.** Slicing
shipped buildings horizontally and summing, at each height, the mass above
against `Σ (area × LinkStrength)` of every link crossing that plane **[CHUNK]**:

| building | height | at 15% height | at 50% | at 90% |
|---|---:|---:|---:|---:|
| `0102oasis_mun_bldg_a` | 20.2 m | 244 m² of joint / 2,025 t | 94 m² / 690 t | 12 m² / 44 t |
| `0102ha` | 11.9 m | 85 m² / 757 t | 121 m² / 294 t | 3 m² / 3 t |
| **`0102ga`** | 12.8 m | **7.8 m² / 1,709 t** | 38 m² / 416 t | 54 m² / 62 t |
| `0101bridge_refinery_a` | 7.4 m | 26 m² / 1,066 t | 29 m² / 950 t | 8 m² / 4 t |

The absolute ratios do not reconcile to SI from outside — `LinkStrength` is *"per
unit area, 100 is the base reference value"* with no unit given, and
`Force_Scalar` is the constant that closes the gap — so read the *shape*, not the
numbers. The stress ratio is **not** flat with height: it peaks near the base in
almost every building, and in a building whose ground floor is columns rather
than walls it peaks catastrophically. `0102ga` puts **1,709 tonnes on 7.8 m² of
joint**, a fifth of the cut area its own upper floors get for a tenth of the
load.

That is the mechanism behind the moment everyone remembers. **You are not
"damaging the building" when you sledgehammer the ground-floor columns; you are
attacking the one place in the structure where the designed margin is already
thinnest, and removing the anchors while you do it.** The two-storey building
that comes down on you when you take the supports out — Eric Arnold's own
example of the system making the player *"feel powerful and vulnerable at the
same time"* **[CBS09]** — is those two effects landing in the same stress pass.

It also explains the failure the system does *not* have. Because load is
recomputed per pass rather than accumulated, and because breaking edges shrinks
the adjacency rather than flagging it, **a partly-demolished building is not a
degrading simulation — it is just a smaller graph**, and it costs proportionally
less to solve than it did intact. A structural system whose cost goes *down* as
it gets destroyed is the opposite of what a naive constraint-solver approach
would give you, and it is most of why the whole city can be destructible at
once.

---

## 4. Rendering — the DLOD, and why the insides of walls are the expensive part

The problem a pre-fractured world creates is not physics, it is draw calls. A
city block that is 300,000 loose triangles the instant you look at it is not
shippable on a 2009 console. Volition's answer is `rfg_dlod_base`, 60 bytes:

```
uint      name_hash;
Vector3   pos;
Matrix3x3 orient;
ushort    render_subpiece;   // the single combined mesh
ushort    first_piece;       // start of a contiguous run of subpieces
byte      max_pieces;        // length of that run
```

`first_piece` / `max_pieces` are a **contiguous range**, and the subpiece array
is ordered so that they are. In `0102house_sml` the first six DLODs cover
subpieces 0–50, 50–100, 100–150, 150–174, 174–224, 224–226 — a clean partition
**[CHUNK]**. `dlod_key` on the subpiece is `dlod_index << 16`, so the mapping is
navigable in both directions with a shift.

> **A DLOD is a *damage* LOD: draw one merged mesh while this region of the
> building is intact, and expand it into its `max_pieces` individual subpiece
> meshes only once something in that range has broken.**

Measured across the game: **26,281 DLODs for 279,262 subpieces — 10.6 subpieces
per DLOD**, median `max_pieces` 4, 95th percentile 46, maximum 192. **[CHUNK]**
So an untouched building draws roughly **a tenth** of the geometry its fractured
form would, and — more importantly — a *tenth of the draw calls*, and the number
degrades only where the player has actually hit it.

That is the same structural trick as [`lod_systems.md`](../../topics/world/lod_systems.md)'s
merged-instance tiers, with damage substituted for distance as the switch
condition, and it is genuinely better than a distance LOD here because **the
condition is monotone**: a DLOD only ever splits, never re-merges (except at
regrow, §9), so there is no hysteresis problem and no popping under camera
motion.

### 4.1 The interior faces

Eric Arnold, on what made the renderer hard: programmers had to deal with
*"increased rendering load from having to draw the insides of walls that no
other game would ever show"* **[CBS09]**.

The install shows how that was authored. `composite_materials.xtbl` is a
ten-row table whose entire content is a mapping from a fracture material to its
edge material: `composite_frag → composite_frag_EDGE`, `Gls_Opaque--frag →
Gls_Opaque--frag_EDGE`, `EDF_InsideFoam → EDF_InsideFoam_EDGE`, and so on
**[XTBL]**. `rfg-composite_s.fxo_kg` is a shipped shader **[EXE]**.

So the newly-exposed interior of a broken piece is **not** a generic "inside"
texture and **not** a procedurally generated cap: it is a *second authored
material paired with the first*, selected by a lookup table, so that broken
concrete shows rebar-flecked concrete and broken panelling shows foam. Arnold
mentions the team attending real building demolitions and *"later added details
like rebar to concrete for authenticity"* **[CBS09]** — that detail lives in
these `_EDGE` materials.

There is a cheaper-looking alternative in the format that Volition **did not
ship**: `ObjectSkirt` + `ObjectSkirtEdge` (a render-mesh index plus a list of
"weld edges", each two points and two normals) is a first-class section of the
chunk header. Across the 140 chunk files checked directly, **`num_object_skirts`
and `num_light_clip_objects` are 0 in every one** **[CHUNK]** — matching the
RFGM reader's own note that no vanilla file has them. A whole geometric
seam-welding feature exists in the format and was abandoned in favour of "author
the interior material properly". `[inferred]` — that is a defensible call: a
weld skirt fixes a *silhouette* problem, and what the player actually notices
is the surface.

### 4.2 The renderer side, by name

`rl_destroyable_model_instance` and `rl_destroyable_model_instance_shared_base`
are RTTI types in the shipped binary **[EXE]** — the split is the standard one,
*shared* holding the immutable mesh/DLOD data for a chunk type and *instance*
holding this particular building's damage state. `rl_clone_mesh_instance` and
`rl_clone_mesh_pool` sit beside them, which is `[inferred]` the pool that
individual broken-off pieces are drawn from.

---

## 5. Physics — Havok, but not Havok Destruction

Worth settling because the received wisdom is fuzzy. The binary contains
**1,220 RTTI class names, of which zero begin with `hkd`** — the prefix for
every class in Havok's Destruction product. What it does contain is the full
`hkp*` Physics set plus **27 Volition-written `havok_*` / `hk_custom_*`
classes** **[EXE]**:

```
havok_filter_shape_collection      havok_rigid_body
havok_mesh_shape                   havok_game_filter
havok_navmesh_shape                havok_blast_decal_collector
havok_region_callback_shape        havok_const_accl_action
havok_closest_linear_cast_collector    ... (+ 10 vehicle classes)
```

Together with `rfg_destroyable_shape` / `rfg_base_destroyable_shape` and the
source file `volition/rfg/code/havok/havok_filter_shape_collection.cpp`, the
picture is unambiguous:

> **GeoMod 2.0 is Volition's own system standing on Havok's rigid-body solver.
> The building presents to Havok as a *single filtered shape collection* whose
> child shapes are the intact subpieces; breaking a piece off removes it from
> the collection and creates a free rigid body.**

`SubpieceData` carrying a `havok shape offset` per subpiece **[RFGM]** is the
other half of that: the collision shapes for all 4,000 pieces are baked and
resident, and "destruction" is a change of *membership*, not a change of
geometry. The chunk file has a `mopp_handle` field and the collision block is a
raw Havok serialised blob (signature `1212891981`, 32% of the CPU file — §11).

Two runtime strings sharpen it. `shape_cutter` and *"uh… trying to cut a
rfg_mover that isn't streamed in: %d"* **[EXE]** say there is a genuine cutting
path — `[inferred]` for terrain/road holes and the nano-rifle rather than for
buildings, since `world.cpp` also carries *"True means break road, false means
repair road"*. And *"\*\*\* Warning: Invalid rfg_mover — base chunk has 0 mass"*
says the **base** piece is a distinguished thing, i.e. the ground anchor the
connectivity test runs against.

---

## 6. The budget, and the most honest piece of design in the game

Pre-fracturing the world means the physics load is not bounded by anything the
player can be trusted with. Volition's answer has a name that appears in the
data, in the effect vocabulary and in a console command: **load balance**.

`load_balance_explosion_pct <float small_pct> <float medium_pct> <float large_pct>` —
console command, with the usage string *"Numbers should add up to 100"*
**[EXE]**. So an explosion converts only a *percentage* of the small, medium and
large pieces it frees into real dynamic bodies.

And then, in `rfg_effect_materials.xtbl`, an event type whose own description is:

> **`load_balance`** — *"Effect that plays when pieces are **deleted** for load
> balance reasons."* **[XTBL]**

That is the whole trick, stated by the people who did it:

> **Pieces that cannot be afforded are deleted, and a dust puff is played where
> they were. The player reads "it crumbled"; the engine did "I dropped it".**

The effect is sized by the deleted piece's *minimum physical dimension*, with
Small/Medium/Large thresholds authored per material, so a deleted girder puffs
differently from a deleted brick. Every material in the game gets to define its
own disappearance.

The same idea appears again in the explosion table: `Delay` is *"how long to
delay (in milliseconds) before starting to **delete pieces**, the effect(s) start
immediately"* **[XTBL]**. Seventeen of the 72 explosions set it, to 0, 50, 100,
200, 500 or 700 ms. **The visual fires first and the simulation catches up**,
which buys a fifth of a second of fireball to hide the moment the geometry
changes.

There is a matching admission on the memory side. Matt Gawalek: *"Our textures
were lower quality than comparable titles of that era, just to save some
memory"*, and they were *"literally stealing memory from other systems just to do
basic functionality"* **[RINGER]**. §11 prices that.

---

## 7. The event vocabulary — why it *feels* like a building falling down

This is the part with no equivalent in the other two destruction notes, and it
is the reason RFG's collapses still read better than games with more accurate
physics. `rfg_effect_materials.xtbl` defines, **per physical material**, eight
destruction events — and every one of them is sized by **a different physical
quantity**, chosen to fit what the event actually is. Volition's own
descriptions **[XTBL]**:

| event | when | sized by | Volition's note |
|---|---|---|---|
| `impact` | normal collisions | **kinetic energy** (Steel: 1000 / 8000 / 80000) | per material *pair*, with a Default for undefined pairs |
| `detach` | *"when a chunk breaks off the parent object"* | **min physical dimension** | *"object must have at least one dimension over that value"* |
| `groan` | *"when a tall, skinny chunk breaks off the parent object and **tips over slowly**"* | **min mass** | *"if an object has a valid groan event it will play instead of a detach"*; **Pole type only** |
| `collapse` | *"when a large section collapses"* | **XZ area** | *"this event should only be defined for Concrete:solid, **it is hard coded game side**"* |
| `shatter` | material shatters | min dimension | *"Must have 'shatter' flag for this to happen"* — one material has it: Glass-Weak |
| `stress` | *"when object is under heavy stress"* | **load / yield ratio** | the creak (§3.1) |
| `explode` | *"during heavy destruction"* | dimension | promotes to a full entry in `explosions.xtbl` |
| `load_balance` | pieces deleted (§6) | min dimension | the cover story |

Read the sizing column on its own. **Impact is energy because that is what a
collision has. Detach is dimension because that is what you see. Groan is mass
because a slow topple is about weight. Collapse is footprint area because a
building falling is about how much of the ground it covered.** Nobody reached
for one "importance" scalar and scaled everything by it, which is the obvious
implementation and the one that makes destruction sound generic.

Every event additionally splits by **shape class** — `Solid`, `Pole`, `Sheet`,
plus specialised `Fence` and `Catwalk` **[XTBL]/[EXE]** — so a falling sheet of
panelling and a falling girder of the same material and mass sound and puff
differently. Five shape classes × eight events × three sizes × 27 materials is
the authoring surface, and most cells are empty and fall back.

Two details in that table are worth more than the structure. **`groan` replaces
`detach` rather than layering on it**, and only for poles — a specific,
observed, named failure mode (the water tower that leans and goes over slowly
instead of dropping) given its own event so it does not get the wrong sound.
And **`collapse` carries a comment admitting it is hard-coded to
Concrete:solid** — a designer-facing table that documents its own leaky
abstraction, which is more use to whoever reads it next than a tidy table that
lies.

---

## 8. How damage arrives

Explosions are `explosions.xtbl`, 72 entries, and they carry **a damage channel
for destroyables that is separate from everything else** **[XTBL]**:

- `Structural_Damage` — *"Damage amount to be applied to destroyable objects.
  Damage to destroyable objects is computed **entirely different** than normal
  objects, so this value has little relevance compared to human or vehicle
  damage."*
- `vehicle_damage_max/min` — hitpoints *"to vehicles and ordinary objects (not
  human or destroyable)"*.
- `Human_Damage_Max/min`, with `player_mult`, `player_veh_mult`.
- `Impulse` — *"Maximum impulse to apply at the center… in kg·m/s. This is
  interpolated to 0 at the edge."*
- `CrumbleRadius` — *"Secondary damage radius, **damage is applied without
  impulse, pieces crumble**."*
- `ConeAngle` — *"half angle for a cone around the fvec of the explosion orient,
  things outside that cone are not damaged."*

The spread of `Structural_Damage` is 8,000 (gauss rifle) to **100,000**
(`wmr_bridge`, a scripted bridge demolition), with the rocket launcher at 23,625
and a remote charge at 22,500. Radii are small — the rocket is 3.65 m primary /
4.0 m crumble.

**The two-radius shape is the design.** Inside `Radius` you get damage *and*
impulse, so pieces fly. Between `Radius` and `CrumbleRadius` you get damage with
**no impulse**, so pieces are freed and fall where they stand. That single split
is what makes an RFG explosion look like a demolition rather than a firework:
the near field is thrown, the ring around it just lets go.

`Break_off_pieces` + `Time_between_breaks` — *"Set this to make the explosion
behave like a nano-rifle (breaking off pieces from objects)"* / *"Time between
each **destruction frame** of breaking links"* **[XTBL]** — is the eat-through-a-
wall weapon, expressed as a repeating link-break with an interval rather than as
a separate system.

### 8.1 Debris that hurts you

`tweak_table.xtbl` (323 global constants, each with a description) contains a
complete model of being hit by your own collapse **[XTBL]**:

| constant | value | Volition's description |
|---|---:|---|
| `Character_collision_damage_min` | 15.0 | *"If the damage calculated by a debris collision is below this, do not apply it."* |
| `Character_collision_damage_speed_max` | 20.0 | *"The maximum speed used in damage calculation for debris. This keeps really fast moving small pieces of debris from killing the player."* |
| `Character_collision_stumble_min` | 45.0 | *"…play a stumble animation. If it hits in the leg area, ragdoll the character."* |
| `Character_collision_ragdoll_min` | 120.0 | *"…ragdoll the character."* |
| `Player_collision_damage_multiplier` | 0.333 | player takes a third |
| `Character_collision_push_away_mass` | 300 | *"When a character pushes against a piece of debris with mass lower than this number, it will move away without modifying the character's motion."* |
| `Character_collision_push_away_dim` | 1.3 | *"maximum size in any dimension of a mover that a human is allowed to just push away"* |
| `Character_collision_push_away_rvec_factor` | 1.5 | how far to the side the pushed piece is nudged |

The clamp on speed is the one to keep: **an unbounded debris field will
eventually launch a pebble at 200 m/s into the player's face, and the fix is a
speed cap in the damage function, not a fix to the physics.** And the
push-away pair (under 300 kg *and* under 1.3 m in every dimension → the piece
yields to you and your movement is unaffected) is how a player walks through a
field of their own rubble without the locomotion turning into a wrestling
match. Both are the kind of rule you only write after shipping the honest
version and watching it fail.

`General_mover_collision_damage_cutoff = 20` / `_multiplier = 10` do the same
for clutter.

---

## 9. Persistence, regrow and streaming

`gameplay_properties.xtbl` carries one row per destructible object type
(hundreds of them), each with `chunk_flags` from a 17-flag set **[XTBL]**:

```
casts_shadows   force_dynamic   show_on_map   disable_collapse_effect
casts_drop_shadows   regrow_on_stream   inherit_damage_pct   invulnerable
plume_on_death   one_of_many   mining   supply_crate   kiosk
propaganda   no_cover   building   child_gives_control
```

plus `game_destroyed_pct` and `fully_destroyed_pct` — **the fraction of the
structure that must be gone before the game calls it destroyed**, authored per
building type and commonly 60–80. Two thresholds rather than one, so "the
mission objective is complete" and "this thing is rubble now" can differ.

**`regrow_on_stream` is the answer to the persistence problem, and it is
brutal**: an ordinary house is rebuilt when the streaming system unloads and
reloads its zone. Only what the game has a reason to remember stays broken.
The save format agrees — `RestorableObjData`, `RestoredObjData` and
`DestroyedObjData` are three separate lists in the world save **[RFGM]** — and
`allow_regrow` is additionally a per-*material* flag (§2.1), so a steel frame
can come back while the glass in it does not.

`regrow_test <hex obj handle>` is a shipped console command **[EXE]**.

The multiplayer inverse is the **Reconstructor**, a repair tool that rebuilds
structures; `reconstructor_passes <[2,10]>` is a shipped console range **[EXE]**,
so repair walks the graph back up in 2–10 steps rather than snapping. Damage
Control mode is built on it.

Three chunks are permanently resident (`preload_chunks.xtbl`):
`missing_chunk.rfgchunkx`, `missing_destroyable.rfgchunkx` and
`edfbarricade_vehicle_a.rfgchunkx` **[XTBL]** — two of the three are the
*failure* placeholders, with a matching warning *"Chunk (%s) is getting replaced
with the missing version"*. A streaming system that can always substitute a
valid destructible for one that failed to load is the reason a streaming failure
is a visible pink box rather than a crash in the physics step.

---

## 10. Networking — destruction frames

Multiplayer destruction is a numbered event stream, not replicated physics.
The diagnostics say so almost completely **[EXE]**:

```
server destruction_frame: %d
client next_destruction_frame: %d, last_avail: %d, backlog: %d,
       first_avail: %d, used: %d, free: %d
no free destruction frames
destruction_sync_problem_remove
rfg_mover destroyed desync.  server count: %d, client count: %d, df: %d
rfg_mover create desync.     server count: %d, client count: %d, df: %d
general_mover create desync. server count: %d, client count: %d, df: %d
```

Reading, `[inferred]` but tightly constrained by those strings: **destruction is
quantised into numbered "destruction frames" allocated from a ring buffer by the
host.** Clients track a backlog and a free list. Each frame is an ordered batch
of link-breaks, so every client applies the same breaks in the same order and the
graph state stays identical without replicating 4,000 rigid bodies. The desync
check is a **count** of movers created/destroyed per frame compared against the
server's — cheap, and it detects divergence at the level that matters (how many
pieces exist) rather than at the level that would be hopeless (where they are).

`no free destruction frames` says the ring is finite and can be exhausted, which
is `[inferred]` the reason multiplayer runs a gentler stress configuration (§3)
and why `Time_between_breaks` on the nano-rifle exists as a rate limit.

This is the same conclusion [`bad_company_2_destruction.md`](bad_company_2_destruction.md)
reaches — *replicate the state transition, never the physics* — arrived at with
a much finer-grained state.

---

## 11. What destructibility costs, measured

Over 132 unique chunk files from six shipped multiplayer maps, splitting each
CPU file by its own header's section sizes **[CHUNK]**:

| section | total | share of CPU file |
|---|---:|---:|
| destruction data (§1) | 27.2 MB | **51%** |
| collision (Havok blob) | 16.8 MB | **32%** |
| render CPU data | 4.4 MB | **8%** |
| *(header, textures, materials, general objects)* | ~4.6 MB | 9% |
| **CPU total** | **53.0 MB** | |
| GPU total (`.gchk_pc` vertex/index) | 93.4 MB | |

Destruction is **19% of everything a building weighs** and **six times the CPU
cost of the render mesh**. Add collision — which is only that large because
every subpiece needs its own shape — and **83% of the CPU side of a building
exists solely because it can be broken.**

The largest single object, `0204office_councilor_a`: 2.4 MB CPU + 5.3 MB GPU,
of which 1.15 MB is destruction and 0.89 MB is collision against **0.26 MB of
render data**.

That number is the context for Gawalek's *"our textures were lower quality than
comparable titles of that era"* **[RINGER]**. It was not a lack of care; a
2009 console had ~256–512 MB and this is what a destructible city costs before
you draw anything.

---

## 12. The three answers, side by side

| | **RFG (GeoMod 2.0)** | **Bad Company 2** | **R6 Siege** |
|---|---|---|---|
| unit of destruction | pre-fractured subpiece | authored damage state | runtime-cut polygon |
| what is simulated | **connectivity of a graph** | nothing — a state machine | the cut, then nothing |
| where the hole can be | only where the artist fractured | only where the artist authored | **anywhere** |
| how much of the world | **all of it** | buildings, walls, terrain | one wall layer |
| structural collapse | emergent from connectivity | canned animation past a threshold | none |
| network | numbered destruction frames | replicated state transitions | replicated cut geometry |
| what it cost | **83% of a building's CPU bytes**; low-res textures | authoring every state by hand | a tiny destructible surface area |

Three readings.

**Everyone replicates the transition and nobody replicates the physics.** Three
teams, three completely different geometric models, identical networking
conclusion. That is as close to a settled result as this directory contains.

**The axis that actually separates them is who chooses where the hole goes.**
Siege gives it to the player and pays by making almost nothing destructible.
RFG gives it to the artist at bake time and pays 83% of a building's memory to
have every building in the city. BC2 gives it to the designer at authoring time
and pays in labour. **None of the three can give the player arbitrary holes in a
whole city, and it is not obvious that anyone has since.**

**RFG is the only one of the three where destruction has a *solver*** — and the
solver is 3.4 KB of tuning data over an amortised graph walk. The expensive part
was never the algorithm. It was the 279,262 pre-fractured pieces the algorithm
needed to exist, and the memory to hold them.

---

## 13. What transfers

Ranked by how much it is worth to this project, not by how clever it is.

1. **Ship geometry, resolve physics from a table at load.** Yield is
   `LinkStrength × area`; mass is `density × volume`; the baked file carries
   only the areas and volumes. One 17 KB XML re-tunes the structural behaviour
   of the entire game with no asset re-bake. Directly applicable to any derived
   or baked data in cromwell — bake the *measurement*, look up the *coefficient*.
2. **Amortised background solve with a fixed per-frame quota and a randomised
   repeat delay.** 400 objects/frame, 100–5000 ms between passes. Bound the work
   and let the latency float. This is how a system whose worst case is unbounded
   becomes a system with no worst case, and it matches CLAUDE.md's rule about
   what deserves a profiler zone: one zone, one number, one knob.
3. **When you cannot afford the object, delete it and play the effect that
   makes deletion legible.** `load_balance` is the single most reusable idea
   here. It generalises far beyond debris — any pooled, budgeted, spawn-heavy
   system needs a *disappearance* that reads as an event.
4. **Size each event by the quantity that event is actually about** — energy for
   impacts, dimension for detachment, mass for a slow topple, footprint area for
   a collapse. One "importance" scalar for all of them is the mistake, and it is
   the difference between destruction that sounds authored and destruction that
   sounds procedural.
5. **Export the solver's internal ratio as an authoring signal.** `load / yield`
   is a number the solver has anyway; making it the input to the creak effect
   costs nothing and gives the player a read on structural state that no amount
   of art direction could fake.
6. **Two damage radii: one with impulse, one without.** The near field is
   thrown, the ring around it just lets go. One extra float, and it is most of
   why an RFG explosion looks like a demolition.
7. **A damage LOD.** Merge the pieces into one mesh until something in that
   group breaks. Monotone, so no hysteresis. 10.6:1 measured.
8. **Clamp the speed used in debris damage, and let the player push small light
   things aside without affecting their own motion.** Two constants that fix the
   two ways a debris field ruins a game.
9. **`Brittleness` as a damage-propagation damper, separate from strength.**
   How far a hit travels is a different property from how much force the joint
   survives, and conflating them is why materials feel samey.
10. **Store the graph twice** — edge list for the solver, adjacency for the
    flood fill — when you cannot afford to build the second at load. Costs
    2 bytes per half-edge here.
11. **Difficulty as a physics constant.** Wrecking Crew's Low/High is
    `Force_Scalar` 0.08 vs 0.2, nothing else.

**And two anti-patterns.** The `ObjectSkirt` weld-edge system is a complete,
shipped-in-the-format, never-used feature — evidence that the geometric fix for
broken-edge appearance lost to the art fix, and that the art fix was correct.
And `Indestructo`: a world advertised as fully destructible still needed 90
pieces that are not, so plan the exception before promising the rule.

---

## 14. What was not established

Substantial, and worth stating plainly.

- **The stress solver itself is inferred.** §3.2 is a reading of the
  instance-buffer layout, field names and effect-sizing quantities. Nothing in
  the strings, the tables or any published source states the algorithm. In
  particular: whether the load accumulation is a single upward pass, an
  iterative relaxation, or something else, is **unknown**. `Force_Scalar` being
  a scalar on "how strong the stress is overall" is consistent with all three.
  What *is* exact is the state budget in §3.1 — 36/4/4/12 bytes, verified on
  **288 of 288 destroyables** — and it is what rules out per-link load
  accumulation.
- **The subpiece `flags` byte is one-eighth read.** 0x20's association with
  big, strong, low, well-connected pieces is measured and strong; calling it
  "the anchor bit" is still a reading, and 25% of large destroyables have none.
  Bits 0x02, 0x04, 0x08 and 0x80 sit at the statistical middle of the structure
  and are unexplained.
- **`LinkStrength` has no stated unit**, so §3.3's table is meaningful only as a
  comparison within and between buildings, never as an absolute safety margin.
  `Force_Scalar` is the constant that closes the gap and its units are equally
  unrecoverable from outside.
- **No per-frame cost figures exist.** No profiler capture, no talk, no ms
  number for the stress pass, the flood fill, or the Havok step. `Objects_Per_Frame`
  is a quota, not a budget in milliseconds, and what 400 objects actually cost
  is not recoverable from the outside.
- **The cap on simultaneous dynamic pieces was not found.** `load_balance`
  clearly implies one; the console command exposes explosion percentages, not
  the global ceiling. It is presumably a pool size in code.
- **Link `flags` is unread.** The 81/19 split is measured; the meaning of the bit
  is a guess.
- **`shape_type`** is 255 for 94% of subpieces, 4 for 16,748 and 3 for 520. The
  enum is not in the strings.
- **The RBB tree's runtime use is inferred** from its name, its siblings in
  `render_lib`, and the fact that it is the only spatial structure in the
  destroyable. No code path was traced.
- **The Havok collision blob was not decoded** — it is 32% of the CPU file and
  is skipped by signature. Whether the subpiece shapes are convex hulls, boxes,
  or a compressed mesh per piece is unknown, and it matters for the memory
  argument in §11.
- **The two 2009 Volition GDC talks were not located.** [GDC10]'s own blurb
  refers to *"two highly-rated GDC lectures by the team in 2009"*; neither
  appears in the Vault under a Red Faction title. If either is a destruction-tech
  talk, it would supersede most of §3 and §5 and this note should be revisited.
- **The Re-MARS-tered renderer was not studied.** 366 compiled shaders
  (`.fxo_kg`) ship in `misc.vpp_pc`, including a `cloe_*` set named after
  Volition's level editor and a `dust_skirting_*` pair whose name suggests
  collapse dust. None were decompiled. This note is about the destruction
  system, and the renderer is a separate note if it is ever worth one.
- **The 2009 build was not compared.** Everything here is the 2018 re-release.
  The chunk format version is identical, so the *data* claims should hold for
  the original; the Havok version claim explicitly does not.

### Reader

The chunk reader written for this note walks `.vpp_pc` → nested `.str2_pc` →
`.cchk_pc` → destruction block, including the in-place recursive RBB tree
traversal that is needed to reach the second and subsequent destroyables in a
file. Two traps worth recording, since both fail silently:

- **The zlib streams in `.str2_pc` are not self-terminating within the
  advertised compressed length.** Read *exactly* `len_compressed_data` bytes and
  feed them to a streaming inflater; reading even one byte more makes the
  decompressor run into the following entry's data and raise `invalid stored
  block lengths` after having produced correct output. Reading fewer silently
  truncates.
- **A destroyable's size cannot be computed without walking its RBB tree**, because
  the tree is a variable-length block between the DLOD array and the instance
  data. The traversal is: internal node (`num_objects == 0`) consumes 40 bytes
  for its two children and recurses into the first; leaf consumes
  `num_objects × 4`. Skipping it lands you in the middle of the next
  destroyable's header, where the counts parse as plausible garbage.
