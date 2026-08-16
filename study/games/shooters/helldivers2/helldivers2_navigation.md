# Helldivers 2 — navigating a world that did not exist a minute ago

How Helldivers 2's AI moves through the world, read from the retail install.
Parent note: [`helldivers2.md`](helldivers2.md).

This note has a sharp boundary and states it up front, because the two halves
of "AI navigation" have completely different evidence available:

* **How agents move** — pathfinding, the navmesh, avoidance, flying — is
  **Havok Navigation**, confirmed three independent ways, and the note can be
  specific about what that buys and why each feature was needed.
* **How agents decide** — target selection, patrol logic, breach calls, the
  pacing of a bug wave — is compiled into `game.dll`, whose strings are
  obfuscated and whose imports are packed. **This install cannot answer it.**
  §6 says what little the resource names hint at and stops there.

The most useful thing in the note is not the middleware name. It is the
argument in §2: **Arrowhead's navigation problem was created by their level
generation decision**, and every Havok Navigation feature they needed is a
feature you only need if your level does not exist until the mission starts.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. Havok Navigation, confirmed three ways  [BUILD] [VENDOR]

**One.** The resource inventory contains exactly one resource of type
`havok_ai_properties`, named `global`, with `havok_physics_properties: global`
beside it. Both are Havok's names, not Stingray's; Stingray shipped its own
navigation, and this build does not use it.

**Two.** Havok lists Helldivers 2 on the Havok Navigation product page's
game roster — alongside Elden Ring, Doom Eternal, Borderlands 3 and World War Z
— and on the Havok Physics and Havok Cloth rosters. **[VENDOR]**

**Three.** Arrowhead's technical director, on Havok's customer page
**[VENDOR]**:

> "We knew early on that HELLDIVERS 2 needed great physics and navigation, and
> Havok was the obvious choice for us."
> — Peter Lindgren, Technical Director, Arrowhead Studios

Cloth corroborates the same integration from a different angle: the build has
exactly **three** `cloth` resources — `medium_cape`, `shock_trooper_cape`,
`melee_flag`. Three capes and a flag. **[inferred]** Nobody licenses Havok Cloth
for three capes; the cloth came along with the physics and navigation licence,
which is consistent with "we knew early on" meaning a single early decision to
take the Havok suite rather than three separate evaluations.

---

## 2. Why it had to be bought: you cannot bake a navmesh for a level that does not exist  [inferred]

[`helldivers2_worldgen.md`](helldivers2_worldgen.md) establishes the fact this
section rests on: **the game contains five levels, none of which is a mission
map.** Every planet surface is assembled at runtime from stamps, biome content
and a generated heightmap.

That single fact eliminates the normal answer. The standard shipping pipeline —
author a level, bake a navmesh offline, tune it by hand, ship both — is not
available, because there is nothing to bake at build time and no human in the
loop at runtime. Whatever generates the level must hand a *working, watertight,
immediately queryable* navigation representation to a hundred agents within the
few seconds of a Hellpod drop, on a map measured in square kilometres, and it
must do it correctly every time on content combinations no one has ever seen.

Line the requirements up against Havok Navigation's stated feature list
**[VENDOR]** and the fit is not approximate:

| What runtime level generation forces | Havok Navigation feature |
|---|---|
| No offline bake — the mesh must be built at mission start | *Fast nav mesh generation*, multithreaded |
| Km-scale maps, only part of which is near a player | *Hierarchical pathfinding*; *cluster graphs* to route through unloaded sections; *nav mesh streaming and stitching* |
| Terrain deforms — craters, destroyed buildings, bug holes | *Runtime nav mesh updates*; *runtime updates to custom edges* |
| Composed geometry leaves regions that look walkable and are not | *Silhouette cutting* and *silhouette painting* |
| A Scavenger and a Charger cannot share a clearance radius | *Multi-radius navigation* |
| Shriekers, Gunships, dropships fly | *Nav volume navigation* (3D) |
| A hundred bugs funnelling into one corridor | *Collision avoidance*, *large crowd support* |
| One player's machine simulates for four ([`helldivers2_networking.md`](helldivers2_networking.md)) | *Cross-platform deterministic algorithms* |

Havok's own description of the crowd behaviour is worth quoting because it
describes what a bug breach actually looks like **[VENDOR]**:

> Our collision avoidance system supports large crowds avoiding moving
> obstacles and characters, creating emergent effects like lane formations and
> swirling.

**[inferred]** The reason Terminids read as a *swarm* rather than as a hundred
individuals each solving a path is that the swarm shape is an emergent property
of the avoidance layer, not authored anywhere. That is also why the failure
mode, when it appears, is a pile-up at a chokepoint rather than agents walking
through each other.

The last row of the table deserves emphasis. Helldivers 2 runs the mission on
one player's machine, but four machines are rendering and predicting it.
**Cross-platform determinism in navmesh generation means every peer builds the
same mesh from the same seed** — so a client's local prediction of where a bug
is heading agrees with the host's, and the same generated map produces the same
navigation on a PC and a PS5 in the same squad. Determinism is usually sold as
a debugging convenience; here it is a networking requirement.

---

## 3. The engine had an opinion, and Arrowhead bought the thing that matched it  [ENGINE] [inferred]

Bitsquid shipped navigation. Arrowhead replaced it. It is worth reading what
the engine's own architect thought navigation *was*, because it explains what
they were replacing it with rather than merely that they did.

Niklas Frykholm, *A\* is Overrated* (2010) **[ENGINE]**:

> So what we really want is an incremental, parallelizable, hierarchical
> algorithm to find a shortish path, not cookie cutter A\*.

and, on what actually determines whether AI reads as competent:

> In my opinion, local navigation is a lot more important to the impression a
> game AI makes than path finding. Nobody will care that much if an AI doesn't
> follow the 100 % best part towards the target. To err is human. But everybody
> will notice if the AI gets stuck running against a wall because its local
> navigation system failed.

He also lists, in the same post, the questions he considers harder than
pathfinding — and they are, one for one, the rows of §2's table:

> How is the graph created? Hand edited in the editor? How much work is it to
> redo it every time the level changes? Automatic? What do you do when the
> automatic generation fails? … How do you handle dynamic worlds were paths can
> be blocked and new paths can be opened? Can you update the graph dynamically
> in an efficient way? What happens to running queries?

**[inferred]** Arrowhead's build answers every one of those questions by
writing a cheque. That is not a criticism — it is the clearest possible
illustration of when buying is right. The engine's architect identified,
fourteen years earlier, exactly the set of problems that a runtime-generated
world creates; Havok Navigation is a product whose entire feature list is
answers to that set; and Arrowhead's technical director says they knew they
needed it "early on". Three parties independently agreeing on the problem
boundary is about as strong a signal as this kind of study produces.

The reusable form of the rule, and it generalises past navigation:
**buy the systems whose correctness is a research problem and whose behaviour
is not your product.** Nobody plays Helldivers 2 for its navmesh generator. They
do play it for what the bugs *do*, which is why the decision layer stayed
in-house — and, inconveniently for this study, in an obfuscated binary.

---

## 4. What the build shows about movement, independent of Havok  [BUILD]

Three things are readable from the install that constrain the navigation
picture without depending on the vendor's claims.

**Multi-radius is not optional here.** The Terminid roster spans Scavengers to
the Hive Lord, and the Hive Lord ships as a composition of separate units for
each leg and mandible ([`helldivers2_creatures.md`](helldivers2_creatures.md)
§1). A single clearance radius across that range would either wall the small
ones out of gaps they should fit through or let the large ones into geometry
they should not reach.

**Flyers are a separate navigation problem, and the roster has several.**
`cha_shrieker` and `cha_dragon` on the bug side, `cyborg_gunship_spawner` on the
Automaton side, plus dropships on every faction. Ground navmesh does not serve
any of them — which is what Havok's *nav volume* (3D) navigation is for.

**Terrain deformation invalidates navigation, and it ships.**
`__tex_deformable_terrain_mask`, `terrain_deformation_max_depth`,
`terrain_deformation_scorch_material_id` and a `terrain_deformation_scissor_offset`
are all in the shader reflection. **[inferred]** A 500kg bomb crater that
changes the visual terrain but not the navigable terrain is a bug players
notice immediately; the fact that the deformation system is a *masked, bounded,
scissored* region rather than an arbitrary heightfield edit is consistent with
it needing to be handed to the navmesh updater as a localised rebuild rather
than a global one.

**Ragdolls are per-creature, and there are 75 of them.** Every named creature
has a `ragdoll_profile`. **[inferred]** Death transitions the body from a
navigating agent to a Havok Physics rigid body — the two products sharing a
representation is exactly why studios take the suite rather than mixing
vendors.

---

## 5. What the wind field is, and what it is not  [BUILD] [ENGINE]

Worth disposing of, because it looks like navigation and is not.

The inventory contains one `vector_field`, named
`core/entities/vector_fields/global_direction/global_direction`, and there is a
`content/level_generation_settings/wind_shader_settings` entity beside it. The
shader reflection has `wind_generator_direction`, `wind_generator_frequency`,
`wind_generator_lacunarity`, `wind_generator_ridged_octaves`,
`wind_generator_min_intensity`/`max_intensity`, and `clear_wind` / `copy_wind`
render passes.

Frykholm wrote a three-part series on Bitsquid's vector field system
**[ENGINE]**, and this is that system, used for wind. `lacunarity` and
`ridged_octaves` are fBm noise parameters — the wind field is **procedural
noise, generated per level alongside the terrain**, driving foliage and cloth.

**[inferred]** It is a rendering and simulation input, not a steering field.
There is no evidence in the build that agents read it. Flagged because a
`vector_field` resource in a game with hundreds of agents invites the flow-field
reading, and the parameter names rule it out.

---

## 6. What the decision layer might be, and why that is a guess  [BUILD]

Stated as honestly as possible: **no behaviour representation exists in the
inventory.** 26,514 resources, and not one behaviour tree, blackboard, utility
curve or HTN domain. The 372 `state_machine` resources are *animation* state
machines — they exist per weapon and per attachment
(`conscript_flamer`, `lieutenant_saw`, `cyborg_tank_turret_mortar`) as well as
per character, which is not how an AI behaviour graph is organised.

The only hints are filenames in `data/game/`, all of them encrypted:

| File | Size | What the name suggests |
|---|---:|---|
| `generated_entities.dl_bin` | 45.6 MB | the entity/tuning database |
| `generated_route_settings.dl_bin` | 460 B | patrol or convoy routing — but 460 bytes |
| `generated_candidate_descs.dl_bin` | 860 B | "candidates" for *something* being selected |
| `generated_scenario_settings.dl_bin` | 8.8 KB | mission scenario pacing |
| `generated_galactic_presence_settings.dl_bin` | 142 KB | faction presence per planet |
| `generated_location_groups.dl_bin` | 393 KB | grouped placement locations |

**[inferred]** and weakly: `route_settings` and `candidate_descs` being tiny
suggests they are *policy* — a handful of global knobs — rather than data, which
would put the actual routing and candidate selection in code. `location_groups`
at 393 KB is large enough to be real placement data, and belongs more to
[`helldivers2_worldgen.md`](helldivers2_worldgen.md) than here.

> **Correction — there is one more channel, and it was missed.** Decoding the
> `state_machine` resources recovers the AI's **event and state vocabulary**,
> because the animation graph is the interface the AI drives. Three of its
> entries bear directly on navigation:
>
> * **`taunt_unreachable`** — a named Terminid state, with an animation, for
>   *the target cannot be pathed to*. Navigation failure is not a stall here; it
>   is a designed character beat. Read against §3's Frykholm quote about AI
>   getting stuck being the thing everybody notices, this is the mitigation.
> * **`guarding_locomotion` / `idle_guard`**, and Terminid ambient tasks
>   `task_sleeping`, `task_eating`, `task_investigate` — the non-combat
>   movement modes a patrol system would drive.
> * The Automaton's **`aware_state`** variable plus `suspicious_enter` →
>   `state_investigate` → `task_scanning`, and `Actions/Alert_Leader` — a graded
>   detection ladder and a squad command link that the Terminids do not have.
>
> Full lists in [`helldivers2_animation.md`](helldivers2_animation.md) §5.2 and
> [`helldivers2_state_machines.txt`](helldivers2_state_machines.txt).

That is the honest end of it. The **policy** — what makes a Terminid choose to
charge, when a patrol calls a breach, how a target is scored — remains
unreadable, and **anything this study said about that would be inference from
playing the game, not from reading it**. The house rule is that those get
written down as observations or not at all.

---

## 7. What is worth taking

1. **Deciding to generate the world at runtime is a navigation decision.** It
   deletes the offline bake, and with it the human tuning pass that catches
   generation failures. Budget for runtime generation, runtime update, and a
   silhouette/annotation mechanism *before* committing to procedural levels, not
   after. §2.
2. **Determinism in navigation generation is a networking feature.** If peers
   generate the same world, they must generate the same navmesh, or client
   prediction disagrees with the host about where anything is going. §2.
3. **Local navigation over pathfinding.** The engine's architect said it in
   2010 and the shipped game demonstrates it: the swarm reads as a swarm because
   of the avoidance layer, and the visible failure mode is a chokepoint pile-up,
   never a bug walking into a wall. §3.
4. **Multi-radius from the start.** A roster spanning Scavenger to Hive Lord
   cannot share a clearance value, and retrofitting that is expensive. §4.
5. **Buy correctness, write behaviour.** Arrowhead bought the layer whose
   failure is a bug and wrote the layer whose quality is the product. §3.

> **Relevance to this project.**
> [`plans/nav_architecture.md`](../../../plans/nav_architecture.md) takes the
> opposite route — representations and algorithms built in the open, no
> middleware — and that is defensible for a game whose world is authored. The
> two rows of this note that survive the difference are §2's determinism point
> (if peers generate the world, they must generate the navigation identically)
> and §3's Frykholm quote, which is upstream of that plan's own layering.
> Helldivers 2 is worth keeping as the counter-case: **the moment the world
> stops being authored, the build-versus-buy answer flips**, because the cost
> of navigation moves from "write a pathfinder" to "write a navmesh generator
> that never fails on content no human has reviewed."
