# Helldivers 2 — destruction sized to a footprint

How Helldivers 2 destroys things, read from the retail install. Parent note:
[`helldivers2.md`](helldivers2.md).

This folder already holds the two poles of the destruction question.
[`rainbow_six_siege.md`](../rainbow_six_siege.md) is the game that **genuinely
cuts geometry at runtime** — project to 2D, clip, ear-clip, extrude, never 3D
CSG — because the *shape* of the hole is the gameplay.
[`bad_company_2_destruction.md`](../bad_company_2_destruction.md) is the game
that **never cuts anything**: every destructible is an authored entity stepping
through pre-built states, and nobody noticed because Battlefield engagement
ranges are not Siege's 2–15 m.

Helldivers 2 is the third point, and it lands next to Bad Company 2 — but it got
there by a different constraint, and the constraint is the interesting part.

**`generated_destruction_settings.dl_bin` is 1.6 MB, the second-largest data
file in the game after the 45 MB entity database.** It is encrypted, so no
tuning value is available. What *is* available is the asset naming, and for once
the naming settles the architecture on its own.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. Buildings are a kit indexed by footprint  [BUILD]

Super Earth's cities, read straight from the unit inventory:

```
cities/buildings/_proxies/16x08_destroyed_proxy
cities/buildings/_proxies/16x16_destroyed_proxy
cities/buildings/_proxies/24x12_destroyed_proxy
cities/buildings/_proxies/28x16_destroyed_proxy
cities/buildings/_proxies/36x12_destroyed_proxy

cities/buildings/rubble_combined/12x8x6m_001_rc
cities/buildings/rubble_combined/16x08_001_rc
cities/buildings/rubble_combined/16x16_001_rc
cities/buildings/rubble_combined/16x16_001_rc_collapsed
cities/buildings/rubble_combined/24x12_001_rc
cities/buildings/rubble_combined/24x12_001_rc_collapsed
cities/buildings/rubble_combined/28x16_001_rc
cities/buildings/rubble_combined/32x24_001_rc
```

**Buildings are named by their footprint in metres**, and each footprint owns a
family:

| Asset | Role |
|---|---|
| `<size>` | the intact building |
| `<size>_destroyed_proxy` | its destroyed silhouette, cheap |
| `<size>_001_rc` | "rubble combined" — the debris field, as one mesh |
| `<size>_001_rc_collapsed` | a further-collapsed rubble state |

**[inferred] This is pre-fragmented destruction in Bad Company 2's sense — an
authored state ladder, not a runtime cut — but the indexing is the finding.** A
procedural city generator does not place *a building*; it places a **`24x12`
slot**. Everything that can occupy that slot, in any state, must occupy exactly
the same footprint, or the generated street stops lining up the moment something
is destroyed.

So the destruction model is not merely "authored states because runtime cutting
is expensive". It is **authored states because the generator composes by
footprint, and a footprint is a contract that the destroyed version has to
honour.** Runtime cutting would break that contract: a cut building's rubble is
whatever shape the cut produced, and the generator has no way to guarantee it
stays inside its slot or leaves the adjacent slots navigable.

That is a genuinely different reason from Bad Company 2's, which was engagement
range and cost. Same architecture, arrived at from the other direction, and
[`helldivers2.md`](helldivers2.md) §4's thesis again: **the decision to generate
the world at runtime propagates into systems that look unrelated to it.**

`_rc` — "rubble combined" — is worth its own line. **[inferred]** The debris
field is *one combined mesh per footprint*, not a pile of individual fragments.
One draw, one collision proxy, one navmesh contribution, deterministic across
four peers. Siege needed determinism machinery to keep runtime-cut geometry in
sync ([`rainbow_six_siege.md`](../rainbow_six_siege.md)); a combined rubble mesh
selected by state index needs none, because there is nothing to synchronise
beyond an integer. Compare Bad Company 2's answer to the same problem in
[`bad_company_2_destruction.md`](../bad_company_2_destruction.md) §3.5 —
undergrowth regenerated from a position-hashed seed *"so that everybody sees the
same geometry"*. **Replicate the cause, regenerate the result** works for
generated content; **replicate the state index** works for authored content;
neither requires shipping geometry over the wire.

---

## 2. The rest of the destructible inventory  [BUILD]

Beyond the city kit, 77 units carry destruction-shaped names, and they fall into
three groups.

**Authored rubble variants**, the same pattern at prop scale:

```
il_ruin_rubble_01 … _06        il_ruin_wall_rubble_01 … _03
il_ruin_wall_broken_02 … _05   il_ruin_floor_broken   il_ruin_statue_broken
il_ruin_trim_round_01_broken   se_colony_rubble_{small,medium,large}
se_colony_rubble_slab          rock_rubble (×4)
```

Note these are Illuminate *ruins* — pre-broken set dressing, authored as broken
rather than broken at runtime. **[inferred]** A generated map needs a stock of
"already destroyed" pieces as much as it needs intact ones, and a ruin biome is
mostly the former.

**Genuinely destructible props**, and there are very few:

```
rock_basalt_small_destructible
bug_cave_bridge_destructible_01
il_weather_device_battery
self_destruct_drone
```

**Wreck states on deployables** — `aa_turret` and `aa_turret_wreck`,
`illuminate_attack_ship_destroyed`. Same two-state pattern as the buildings, at
entity scale.

**Narrative destruction levels**:

```
narrative_node_no_destruction
narrative_node_lvl2_destruction
narrative_node_lvl3_destruction
narrative_node_lvl4_destruction
```

**[inferred]** A "narrative node" with four damage tiers is city-scale damage
driven by *war state* rather than by player action — a planet further into a
Automaton occupation generates a more ruined city. That is the Galactic War
reaching into the level generator, and it is the only place in the build where
this study found the two touching.

---

## 3. Where the continuous destruction actually is  [BUILD]

Buildings step through states. Two systems in this build *are* continuous, and
both were covered elsewhere:

**Terrain deformation** —
[`helldivers2_worldgen.md`](helldivers2_worldgen.md) §5. Craters and scorch,
written into `__tex_deformable_terrain_mask` with a
`terrain_deformation_scissor_offset`, a `terrain_deformation_max_depth` and a
`terrain_deformation_scorch_material_id`. **Bounded, clipped, masked** — because
the result has to be handed cheaply to the navmesh updater and the bake systems.

**Creature dismemberment** —
[`helldivers2_creatures.md`](helldivers2_creatures.md) §2.3 and §3. Damage-state
and gib geometry as index ranges into one shared skinned buffer, plus the
object-space wound buffer that accumulates holes into a texture array.

Put the three side by side and the build has **one consistent philosophy**:

| Thing destroyed | Representation | Why |
|---|---|---|
| Buildings | authored state ladder, selected by index | the generator's footprint contract |
| Terrain | bounded mask written into a buffer | must be re-handed to navmesh and bakes |
| Creatures | index ranges + object-space wound array | skinning must not get more expensive |

**Nothing in Helldivers 2 destroys geometry by editing geometry.** Every system
either *selects* from pre-authored states or *accumulates into a buffer*. That
is the rule, and it is the same rule
[`helldivers2_creatures.md`](helldivers2_creatures.md) §6 draws for gore.

**[inferred]** The reason is uniform across all three: everything downstream —
navmesh, lightmaps, streaming, four-peer agreement — was already forced into the
runtime by procedural generation
([`helldivers2.md`](helldivers2.md) §4). A system that mutated authored geometry
at runtime would have to re-run all of that. A system that flips a state index
or writes a bounded mask can be handed to them cheaply. **Runtime world
generation does not just cost you the bakers; it constrains what your
destruction is allowed to be.**

---

## 4. What this study cannot say

* **Any threshold.** `generated_destruction_settings.dl_bin` is encrypted. How
  much damage collapses a `24x12`, whether the `_rc` → `_rc_collapsed` step is
  damage-driven or timed, what the health pools are — none of it is readable.
* **Whether debris is simulated.** 2,153 `physics` resources and 75
  `ragdoll_profile`s exist, but nothing in the naming says whether building
  debris gets rigid bodies or is purely a mesh swap with a particle burst.
  Bad Company 2 and Siege both concluded fragments deserve near-zero simulation
  budget ([`bad_company_2_destruction.md`](../bad_company_2_destruction.md));
  **[inferred]** a *combined* rubble mesh strongly suggests Helldivers 2 agrees,
  but that is an inference from a filename.
* **The collapse animation.** Bad Company 2 played a canned collapse; whether
  Helldivers 2 does the same, cross-fades, or drops the proxy behind a dust
  burst is not in the inventory.

---

## 5. What is worth taking

1. **If a generator composes by slot, the destroyed state must honour the
   slot.** That single constraint rules out runtime cutting, before any cost
   argument. §1.
2. **Name destructibles by their footprint.** `24x12` as an asset name makes the
   contract explicit and makes the intact/proxy/rubble/collapsed family
   obviously a family. §1.
3. **Combine the debris field into one mesh per state.** One draw, one collision
   proxy, one integer to replicate. §1.
4. **Destroy by selecting or by accumulating — never by editing.** Three systems
   in this build, three representations, one rule. §3.
5. **Ship pre-broken set dressing.** A generated ruin biome needs authored
   rubble as much as authored walls. §2.
