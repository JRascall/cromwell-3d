# Helldivers 2 — how a Terminid is drawn, and how it comes apart

How Helldivers 2 renders its AI creatures, read from the retail install. Parent
note: [`helldivers2.md`](helldivers2.md); read its Sources block first, because
the evidence grades matter here.

The question "how do they render their creatures" has two halves, and the build
answers them with two completely different mechanisms that are worth keeping
apart:

* **Structural damage** — losing a leg, cracking a carapace, gibbing a claw —
  is **geometry that already exists**, selected by index range. It is authored,
  discrete, and free at runtime.
* **Surface damage** — the holes, char and wetness that accumulate as you empty
  a magazine into something — is a **GPU wound buffer** in the creature's own
  texture space, fed by a queue, cleared and updated by two named passes each
  frame, and *healed* by a `regen_amount` term.

Neither is a mesh swap and neither is a material swap. Get that split and the
rest of the system explains itself.

> **Superseded in two places by [`helldivers2_animation.md`](helldivers2_animation.md).**
> That note dumps the creature's *material* shaders, which this one did not, and
> two claims here need reading against it. **§2.3's "three materials and four
> textures"** is right about *authored* maps and wrong as a description of what
> is bound — the full shading permutation binds **18 samplers**, including
> clearcoat, iridescence, a blood tiler and five wound textures. And **§5's
> "this study cannot say how a Terminid decides anything" is too strong**: the
> animation state machine's 72 named events and 56 named states are the AI's
> visible vocabulary, and they name tasks, taunts, awareness and — the good one
> — `taunt_unreachable`. Corrections are marked inline below.

Tags as [`helldivers2.md`](helldivers2.md). **[BUILD]** dominates: the teardown
below is a glTF export of `content/fac_bugs/cha_charger/cha_charger.unit`
read with a script, and the wound system is DXBC reflection.

---

## 1. The roster, and what its naming gives away  [BUILD]

44 named `unit` resources under `content/fac_bugs/`, 60 under `fac_cyborgs/`, 23
under `fac_illuminate/`. Two things fall out of the names immediately.

**The Automatons are internally "cyborgs".** Every Automaton asset lives under
`content/fac_cyborgs/` — `cha_conscript`, `cha_berserker`, `cha_soldier`,
`cha_lieutenant`, `cha_cyborg_elite`. The Cyborgs were the *first* game's
faction; the Automatons are their content tree with the fiction changed. This
is not trivia — it means the second game's enemy pipeline was built on the
first's, and it dates the pipeline.

**Variants are a suffix, not an asset.** The Terminid roster is a small set of
bases with a tier and biome suffix stacked on:

```
cha_warrior            cha_warrior_tier_1     cha_warrior_gloom_tier_1
cha_warrior_acid       cha_warrior_big        cha_warrior_big_tier2
cha_warrior_burrower   cha_warrior_plus
cha_hunter             cha_hunter_tier3       cha_hunter_gloom_tier1
cha_scavenger          cha_scavenger_gloom    cha_scavenger_captive
cha_charger            cha_charger_acid       cha_charger_bull
cha_charger_burrower
```

`_tier_N` is difficulty scaling; `_gloom` is the Gloom biome variant;
`_burrower` is the emerge-from-the-ground entry animation set;
`_captive` is the objective prop version. **[inferred]** A separate `unit`
resource per variant rather than a runtime parameter suggests each carries its
own material bindings, ragdoll profile and state machine — confirmed for the
Charger below, where `cha_charger_acid` and `cha_charger_bull` are separate
`.unit` files of comparable size.

**Big creatures are assembled from units.** The Hive Lord is not one asset:

```
cha_hive_lord            cha_hive_lord_l_leg      cha_hive_lord_r_leg
cha_hive_lord_l_mandible cha_hive_lord_r_mandible
cha_hive_lord_shard_b_01 ... _b_03    cha_hive_lord_shard_f_01 ... _f_03
```

Same pattern for `cha_impaler` + `cha_impaler_tentacle`, `cha_hunter` +
`cha_hunter_tongue`, `cha_stalker` + `stalker_tongue`. **[inferred]** A
creature whose parts need independent physics, independent damage or
independent spawning is a *composition of units*, not a deeper rig. The
tentacles are the clearest case: an Impaler's tentacles erupt from arbitrary
ground metres away from the body, which is only expressible as separate
entities.

**Legged things get IK, and only legged things.** All 12 `ik_skeleton`
resources in the entire game:

```
cha_strider  cha_strider_gloom                (bug, tall walker)
cyborg_assault_walker  cyborg_walker_scout  cyborg_spawner
cha_tripod  cha_exomech_melee  cha_exomech_ranged   (Illuminate)
avatar_helldiver  fp_cha_avatar  cha_seaf              (humanoids)
```

Chargers, Hunters, Warriors — the four-and-six-legged sprinters — have **no IK
skeleton**. The Strider, the walkers and the Tripod do. **[inferred]** The
dividing line is not leg count, it is *gait speed against terrain relief*: a
slow, tall, long-legged walker on procedurally generated ground will visibly
float or clip without foot placement, and a Charger at full sprint will not be
looked at long enough for anyone to notice. That is a direct application of the
cheapest rule in game AI presentation, and it is the same trade this project's
own notes make about where cost is worth paying. Given that the terrain
underneath is generated at runtime
([`helldivers2_worldgen.md`](helldivers2_worldgen.md)), *some* runtime foot
solve was unavoidable for the tall units — there is no authored ground to
hand-place against.

---

## 2. A Charger, taken apart  [BUILD]

`content/fac_bugs/cha_charger/cha_charger.unit`, exported to glTF. The unit's
root has **exactly four children**, and each is a distinct subsystem:

```
cha_charger
├── damageables     50 nodes  — hit zones
├── skeleton        ~60 bones — animation rig
├── shadow_mesh     4 LODs    — shadow-only geometry
└── game_mesh       4 LODs + 26 part meshes — what you see
```

### 2.1 Two skeletons, not one

The `skeleton` branch is the animation rig, and it is ordinary:

```
root boss neck1 head1 headplate1 head2 pincer_r pincer_l
upper_jaw1 upper_jaw2 lower_jaw1 lower_jaw2
shield1 … shield6         spine1 … spine4      butt_flab1 … butt_flab3
front_leg_{1..4}_{l,r}  middle_leg_{1..4}_{l,r}  back_leg_{1..4}_{l,r}
aim_root  voice
```

Six legs of four segments; six `shield` bones for the carapace plates; three
`butt_flab` bones for the abdomen jiggle. Two non-anatomical bones matter:
**`aim_root`** (where the creature's aim/lookat is resolved) and **`voice`**
(where Wwise attaches the emitter — audio position is a rig concern).

The `damageables` branch is a **parallel 50-node skeleton**, and it is the
interesting one. 27 of the 50 have recovered names, all `c_`-prefixed:

```
c_head1 c_neck1 c_lower_jaw1 c_spine1 … c_spine4  c_boss  c_collision
c_front_leg_{1..3}_{l,r}  c_middle_leg_{1..3}_{l,r}  c_back_leg_{1..3}_{l,r}
```

The remaining 23 are `Bone_<hash>` — thin hashes filediver could not reverse.

**[inferred]** This is a hit-zone skeleton kept deliberately separate from the
animation skeleton, and the separation buys three things. The hit zones are
**coarser** than the rig — three segments per leg where the rig has four, no
jaw articulation beyond `c_lower_jaw1`, no `shield` or `butt_flab` entries at
all — so raycast and overlap tests run against a simplified body. They are
**stable**: an animator can add a bone without silently changing where a
weakpoint is. And `c_collision` sitting in the same list says the physics proxy
is a member of this hierarchy rather than a fifth thing.

This is the piece worth stealing. The instinct is to hang damage zones off
animation bones because they are already there; **the shipped answer is a
second, coarser, independently-owned hierarchy**, and the reason is that damage
zones and animation have different owners, different rates of change, and
different correctness requirements.

### 2.2 A separate shadow mesh, with its own LOD chain

| | LOD0 | LOD1 | LOD2 | LOD3 |
|---|---:|---:|---:|---:|
| `game_mesh` | **35,722** | 10,715 | 3,214 | 964 |
| `shadow_mesh` | **10,716** | 3,213 | 641 | 319 |

Two observations. The game mesh drops **70%** at the first step — an aggressive
LOD0→LOD1 ratio that only works because a Charger is either right on top of you
or in a crowd. And the shadow mesh's LOD0 is **10,716 triangles against the game
mesh's LOD1 at 10,715** — the same budget, one triangle apart. That is not
coincidence; **[inferred]** the shadow chain is the game chain shifted one LOD
down, with its own authored topology so it can also be watertight and hole-free
where the visible mesh has cutouts.

It carries its own material, `base_shaders/m_character_shadow`. **[inferred]**
A dedicated shadow material means the shadow pass never touches the creature's
real shader — no wound lookup, no subsurface, no customization — which matters
when the sun cascade is redrawing every bug on screen four times
([`helldivers2_rendering.md`](helldivers2_rendering.md) §3).

### 2.3 The part library: 26 meshes, one vertex buffer

Beyond the four LODs, `game_mesh` holds **26 further meshes**:

| Part | Intact | Damaged | Gib |
|---|---|---|---|
| body | `body` | — | — |
| head | `head` | `head_damaged` | — |
| abdomen | `butt` | `butt_damaged`, `butt_hurt` | — |
| flanks | `side_left`, `side_right` | `side_*_damaged` | — |
| legs ×4 | `{left,right}_{front,back}_leg` | `*_damaged` | `*_gib` |
| claws ×2 | `claw_{left,right}` | — | `claw_*_gib` |

11 intact parts, 9 damaged variants, 6 gibs. **Every one of the 26 reports the
same `POSITION` accessor of 193,494 vertices.** They are not 26 meshes in the
ordinary sense — they are **26 index ranges into a single shared skinned vertex
buffer**.

That is the load-bearing implementation detail of Helldivers 2's gore, and its
consequences are all good ones **[inferred]**:

* **Skinning happens once.** One vertex buffer, one skinned result, regardless
  of which damage state each part is in. Blowing three legs off a Charger does
  not add a skinning pass.
* **State change is free.** Swapping intact→damaged→gibbed is choosing a
  different `(first_index, index_count)`. No buffer creation, no upload, no
  allocation — which is exactly what you need when a Hellbomb changes the damage
  state of forty creatures in one frame.
* **It costs draw calls, not memory.** A fully intact Charger draws its parts
  as a set of ranges; the cost scales with *how many distinct ranges are
  active*, not with how damaged it is. This is the tradeoff they took, and on a
  D3D12 renderer with cheap draws it is the right one.
* **Gore bones are pre-authored.** The rig carries `back_leg_3_l_gore1`,
  `middle_leg_3_l_gore1`, `middle_leg_2_r_gore1`, `back_leg_2_r_gore1` —
  dedicated bones for the exposed stump geometry, which exists in the shared
  buffer whether or not the leg has come off yet.

**The whole Charger is 3 materials and 4 authored maps.** `m_character_shadow`
for the shadow mesh, `m_collision` for the proxy, and exactly one real material
— `cha_charger/cha_charger` — for all 26 parts and all four LODs. The authored
set is `cha_charger_albedo`, `_normal`, `_ccras` (clearcoat / roughness / AO /
spec) and `_sss`.

> **Correction.** The glTF export carries four images and this note originally
> read that as "four textures". It is four *authored* maps; the material binds
> **18 samplers** in its full permutation — shared library textures (detail
> normal, blood tiler), the five wound textures, BRDF LUTs, and the terrain
> heightmap for wetness. See
> [`helldivers2_animation.md`](helldivers2_animation.md) §1.1.

**[inferred]** The point survives the correction and is arguably stronger: the
*authored, per-creature* budget is four maps, and everything else a Charger
needs is either shared across the game or written at runtime. Variety does not
come from textures; §3 and §4 are where it comes from.

---

## 3. The wound system  [BUILD]

Two named passes ship in the retail shader libraries:

```
clear_wounds
update_wounds
```

and the reflection data around them names the whole structure:

| Identifier | Reading |
|---|---|
| `c_wounds` | the constant buffer |
| `wounds_256`, `wounds_512` | **two resolution tiers** of wound buffer |
| `wounds_indirection_256`, `wounds_indirection_512` | which slot belongs to which creature |
| `wounds_object_space_lookup_256/512` | **wounds live in object space, not screen space** |
| `wounds_queue_256`, `wounds_queue_512` | pending hits, applied by `update_wounds` |
| `__tex_wound_lut_to_add` | the brush — what a hit stamps |
| `wound_dt`, `regen_amount` | time step, and **healing** |
| `character_id` | which creature this fragment belongs to |

**[inferred]** Reading that as a system: each wounded creature is allocated a
slot in a shared atlas at one of two resolutions, `wounds_indirection_*` maps
creature → slot, hits are pushed into `wounds_queue_*` by the simulation, and
once per frame `update_wounds` drains the queue and stamps `wound_lut_to_add`
into each affected slot in the creature's **own UV space** — which is why the
lookup is called `object_space`, and why a wound stays put on the carapace as
the bug turns. `clear_wounds` recycles a slot when a creature dies or leaves.
`wound_dt` and `regen_amount` together mean the buffer decays: wounds fade, and
on the creatures that visibly regenerate, they close.

Four things about this design are worth pulling out:

**Two tiers, not one.** `256` and `512` in parallel with separate queues,
indirection tables and lookups. **[inferred]** A Bile Titan and a Scavenger do
not deserve the same wound resolution, and a shared atlas with one size would
either waste most of itself on the small ones or starve the big ones. Two
fixed tiers is the cheap answer — no allocator, two pools, a size class chosen
per creature type.

**Queue, not immediate write.** Hits arrive during simulation, in arbitrary
order, potentially many per creature per frame. Writing them immediately would
mean a render-target bind per hit. A queue drained by one pass makes the cost
proportional to *creatures wounded*, not *hits landed* — which is the
difference between a flamethrower being free and a flamethrower being a spike.
This is the same shape as the "cull cheaply before testing expensively" rule
this project applies to spatial queries: batch at the boundary where the cost
changes class.

**Object space, not screen space.** A decal system would put these in world
space and reproject; that fails on a skinned, deforming, fast-moving target.
Object-space texels follow the skin for free.

**Indirection, so the buffer is not per-creature.** `wounds_indirection_*`
exists because most bugs on screen are unwounded and should own nothing. Slots
are handed out on first damage. **[inferred]** This is also why the system can
afford 512² for the big ones: the pool is sized for *concurrently wounded*
creatures, not for the spawn cap.

Against `cha_charger`'s three materials, the picture completes: **the authored
asset provides a small, uniform base look, and everything that makes one
Charger look different from another Charger is a buffer written at runtime** —
its wound slot, its damage-state index ranges, and its customization
parameters. That is why 44 bug units can populate a screen without a texture
budget explosion.

---

## 4. Where the rest of the surface variety comes from  [BUILD]

Three more mechanisms show up in the reflection and the inventory, and they
share the same philosophy — shared arrays, per-instance parameters:

**Tiler arrays.** `customization_camo_tiler_array`,
`customization_detail_tiler_array`, `glitter_tiler`, `ripples_tiler` are single
array textures shared across the game. `visualize_camo_customization`,
`visualize_material_customization`, `visualize_pattern_customization` and
`visualize_cape_customization` are shipped passes. **[inferred]** Armour and
faction paint are an index into a shared array plus parameters, not a texture
set — which is the only way a warbond can add cosmetics without shipping
gigabytes.

**Subsurface scattering.** `__tex_sss_lut` plus a `scatter_*` array set —
`scatter_albedo_opacity_array`, `scatter_normal_array`, `scatter_rsh_array`,
`scatter_subsurface_array`, `scatter_lookup`. **[inferred]** The `_array`
suffix throughout says these are shared slices indexed per instance. `rsh` is
almost certainly radiance spherical harmonics — a cheap directional lighting
term baked per slice — which is the standard trick for making translucent
foliage and chitin read correctly without a per-pixel scatter integration.

**Terrain scorch, which is a creature-adjacent system.**
`terrain_deformation_scorch_material_id`, `__tex_deformable_terrain_mask`,
`terrain_deformation_max_depth` — the ground takes damage in the same
"accumulate into a buffer" style the creatures do. Different buffer, identical
idea. [`helldivers2_worldgen.md`](helldivers2_worldgen.md) §5.

---

## 5. Animation, and what is *not* there  [BUILD] [ENGINE]

372 `state_machine` resources, 1,147 `animation`, 416 `bones`. Crucially, state
machines exist **per component, not per character** —
`cha_cyborg_elite`, but also `conscript_flamer`, `lieutenant_saw`,
`cyborg_tank_turret_mortar`, `lieutenant_flag`. A weapon owns its own animation
state machine and is composed onto the wielder.

Frykholm's description of Bitsquid's animation layer **[ENGINE]** is the
context: the low-level system is deliberately a *data* layer — blend trees and
state machines driving bone poses, with gameplay above it — and the engine
never took a position on AI. Which is consistent with what is missing here:

**There is no behaviour resource type in the entire inventory.** No behaviour
tree, no blackboard, no utility curve, no HTN domain. 26,514 resources and not
one of them describes a decision. Enemy decision-making is compiled C++ inside
`game.dll`, whose strings are obfuscated and whose imports are packed
([`helldivers2.md`](helldivers2.md) Sources).

> **Correction.** This section originally concluded "this study cannot say how a
> Terminid decides anything", and that overstated it. The *policy* is indeed
> unreadable — no scoring, no thresholds, no timings. But the state machine is
> the interface the AI drives, and **decoding it recovers the AI's vocabulary**:
> ambient tasks (`task_sleeping`, `task_eating`, `task_investigate`), alerting
> (`bark_target_aquired`), guarding, and `taunt_unreachable` — a named state for
> *pathing failed*. The Automaton's machine goes further, with a graded
> `aware_state` ladder (`suspicious_enter` → `state_investigate` →
> `task_scanning`) and `Actions/Alert_Leader`, which the Terminid has no
> equivalent of. See [`helldivers2_animation.md`](helldivers2_animation.md) §5.2.
> **A vocabulary is not a behaviour model, but it is not nothing.**

What can be said is that the *animation* side is data and the *decision* side
is code, and the seam between them is the state machine. That is a defensible
split and the opposite of the fashion for authoring AI in data.

---

## 6. What is worth taking

Ranked by how much they would change a design, not by how clever they are.

1. **Two hierarchies: animation bones and damage zones.** Coarser hit zones,
   separately owned, with the collision proxy as a member. Costs one extra
   skeleton per creature; buys stability across animation changes and cheaper
   hit tests. §2.1.
2. **Damage states as index ranges into one shared skinned buffer.** Intact,
   damaged and gibbed geometry all resident; state change is a draw-range
   change. Skinning cost is independent of how mangled the creature is. §2.3.
3. **Accumulate surface damage into an object-space buffer, drained from a
   queue, with two size tiers and an indirection table.** Cost scales with
   *creatures wounded*, not *hits landed*, and unwounded creatures own nothing.
   §3.
4. **A dedicated shadow mesh at one LOD step down, with its own material.** The
   shadow pass never evaluates the wound lookup, the SSS or the customization.
   §2.2.
5. **IK only where the gait and the terrain make it visible.** Eleven rigs in
   the whole game. §1.
6. **Composition over rig depth for large creatures.** Tentacles, mandibles and
   shards are separate units because they need separate physics and separate
   damage. §1.

Items 2 and 3 are the pair worth understanding together: **discrete damage is
authored geometry selected at zero cost, continuous damage is a buffer written
at bounded cost.** Neither is the naive answer, and the naive answers — mesh
swapping and per-creature decal render targets — are exactly the two things
that would have made a horde shooter impossible.
