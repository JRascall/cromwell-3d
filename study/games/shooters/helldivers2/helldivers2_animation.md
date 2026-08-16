# Helldivers 2 — animating a hundred agents, and what the rig leaks about the AI

How Helldivers 2 animates and draws its AI at the counts it runs them, read from
the retail install. Parent note: [`helldivers2.md`](helldivers2.md). Companion:
[`helldivers2_creatures.md`](helldivers2_creatures.md), which covers damage
geometry and the wound system.

This note exists because the first pass of this study got the method wrong. It
swept the **engine's** shader libraries and found *zero* bone or skin
identifiers, which is true and misleading: character skinning lives in the
**material** shaders, which are a separate resource type and have to be dumped
per material. Doing that turns four open questions into answered ones.

**The short answers, before the evidence:**

| Question | Answer |
|---|---|
| VAT (vertex animation textures)? | **No.** |
| BAT / bone animation textures? | **No.** |
| How then? | **GPU linear-blend skinning, 4 influences, matrices in a shared `samplerBuffer`, hardware-instanced** |
| Billboards or impostors for distant AI? | **No — never.** Skinned meshes all the way out, down to 194 triangles |
| LOD scheme? | **Four levels at a uniform ~30% decimation per step**, plus a separate shadow chain one step down |
| Can we see the AI's thinking? | **Partly, and more than the first pass claimed** — §5 |

Tags as [`helldivers2.md`](helldivers2.md). The state-machine vocabulary is
beside this note as
[`helldivers2_state_machines.txt`](helldivers2_state_machines.txt).

---

## 1. Skinning: the boring answer, done well  [BUILD]

Dumping `content/fac_bugs/cha_charger/cha_charger` as a material folder with
GLSL gives **92 shader programs**, and every vertex shader is named
`*.inst.glsl.vert` — instanced. Its input signature:

```glsl
layout(location = 0) in vec4  iPOSITION0;
layout(location = 1) in vec4  iNORMAL0;
layout(location = 2) in vec2  iTEXCOORD0;
layout(location = 3) in vec2  iTEXCOORD1;
layout(location = 4) in uint  iTEXCOORD15;      // per-vertex part index
layout(location = 5) in uvec4 iBLENDINDICES0;   // 4 bone indices
layout(location = 6) in vec4  iBLENDWEIGHTS0;   // 4 weights
layout(location = 8) in uint  iSV_InstanceID0;
```

`BLENDINDICES` + `BLENDWEIGHTS` as **real vertex attributes** settles it: this is
classic linear-blend skinning with **four influences**, evaluated in the vertex
shader. No animation texture is sampled, no vertex-cache/VAT path exists, and
there is no compute pre-skin writing to a UAV.

Where the matrices come from is the interesting part:

```glsl
layout(std140, binding = 22) uniform c_skin_matrices {
    uint bdata_offset;   // that is the entire constant buffer
    uint _zero;
};
uniform samplerBuffer bdata;   // all bone matrices, for everything
uniform samplerBuffer idata;   // all per-instance data, for everything
```

**The skin-matrix constant buffer contains no matrices — it contains an
offset.** The decompiled body shows the indexing exactly:

```glsl
r2.xyzw = ivec4(3,3,3,3) * iBLENDINDICES0.xyzw + r0.zzzz;   // 3 texels per bone
r3 = texelFetch(bdata, r2.y);  r3 *= iBLENDWEIGHTS0.yyyy;
r4 = texelFetch(bdata, r2.x);  r3 = r4 * iBLENDWEIGHTS0.xxxx + r3;
r4 = texelFetch(bdata, r2.z);  r3 = r4 * iBLENDWEIGHTS0.zzzz + r3;
r4 = texelFetch(bdata, r2.w);  r3 = r4 * iBLENDWEIGHTS0.wwww + r3;
```

Three texels per bone — a 3×4 affine matrix — accumulated three times over, once
per output row. **[inferred]** So there is **one global bone-matrix buffer for
every skinned thing in the frame**, and a draw says only "my bones start at
`bdata_offset`". Per-instance data (`ioffset`, used 114 times across these
shaders) indexes a second global buffer, `idata`, at 11 texels per record.

That is the whole scaling answer, and it is worth stating as a rule:

> **Nothing about a character is per-draw constant data. Bones are an offset
> into a shared buffer; instance state is an offset into another shared buffer.**

The consequences are exactly what a horde shooter needs **[inferred]**: no
constant-buffer update per creature, no descriptor churn, and creatures of the
same type collapse into instanced draws because everything that differs between
two Chargers is a buffer index. It also explains why `c_per_instance` — which
*does* declare `cb_world`, `lod_fade_level`, `instance_seed` and a 64-bit
`visibility_mask` — is **never referenced in any of the 92 programs**: that
constant buffer is the *non*-instanced fallback path, and the shipped shaders
all take the buffer route.

> **A methodological warning that cost this study a correction.** `c_per_object`
> declares 40 floats including `woundable_id`, `use_large_wound_lookup` and
> `wound_painting_enabled`, and **none of them is referenced** by the program
> that declares them. It is a *generated, shared* layout — the same phenomenon
> [`helldivers2_vfx.md`](helldivers2_vfx.md) reports for `c_billboard`. A field
> in a constant buffer proves the engine has a concept, not that this shader
> uses it. Always check for a reference, not a declaration.

### 1.1 What the creature shader actually binds

The full shading permutation binds **18 samplers**, and the split is
informative:

```
authored for this creature (4 maps):
    cha_charger_albedo   cha_charger_normal   cha_charger_ccras   cha_charger_sss

shared library textures:
    tex_detail_normal_1   tex_blood_tiler   tex_texture_map_02

the wound system (5):
    tex_wounds_512   tex_wounds_256        <- sampler2DArray
    tex_wound_data   tex_wound_normal   tex_wound_derivative

global:
    tex_brdf_lut   tex_specular_brdf_lut   idata
    tex_ui_diffuse_cubemap  tex_ui_specular_cubemap  tex_ui_3d_shadows
```

Two corrections to [`helldivers2_creatures.md`](helldivers2_creatures.md) fall
out of this, both in the same direction — the earlier figures were *exports*,
not bindings. **Four authored maps** is right (`albedo`, `normal`,
`ccras` = clearcoat/roughness/AO/spec, `sss`), and the glTF's "4 images" was
counting those. But the material binds fourteen more, and the creature is
shaded with **clearcoat** (chitin gloss), **subsurface**, **iridescence**
(`tex_albedo_iridescence` with `i_start`, `i_end`, `i_intensity`, `i_thickness`
— cross-reference [`iridescence.md`](../../../topics/surfaces/iridescence.md)),
a **blood tiler** with two tint colours, and a `burn_scorch` term.

And the wound tiers are now unambiguous: `tex_wounds_256` and `tex_wounds_512`
are **`sampler2DArray`**, sampled as `textureLod(tex_wounds_512, uvw, 0.0).x` —
single channel, mip 0, **`w` selecting an array slice**. So the "indirection
table" of [`helldivers2_creatures.md`](helldivers2_creatures.md) §3 is a
**per-creature array slice index**, not a UV offset into an atlas, and the two
tiers are two texture arrays with `use_large_wound_lookup` choosing between
them. The wound *amount* in that slice then drives shared
`tex_wound_data` / `_normal` / `_derivative` lookups, which is how a wound gets
a crater normal and a parallax offset rather than being a stain.

The creature shader also samples `tex_generated_heightmap` and
`tex_water_height` in nine programs. **[inferred]** That is wetness and water
interaction read straight off the generated terrain
([`helldivers2_worldgen.md`](helldivers2_worldgen.md)) — `material_wetness` is a
`c_per_object` field.

---

## 2. No impostors, no billboards, ever  [BUILD]

Asked directly of the data, because it is the obvious way to make hundreds of
agents cheap and it is *not* what they did.

* The only `imp_*` identifier anywhere in the Charger's 92 programs is
  `imp_transparent_override`, and that is a **global frame constant** present in
  318 of 813 engine shader libraries — not the impostor path.
* The impostor bake passes (`imp_bake`, `imp_clear`, `imp_weight_merge`,
  `imp_material_count`) exist for vegetation and static props; all 224
  `speedtree` resources live under `env_*` and `planet_*` trees, none under
  `fac_*`.
* Searching the entire 26,514-resource inventory for `impostor`/`billboard`
  returns **four** hits: a literal `colony_billboard_01` prop, its physics, and
  the engine's `missing_billboard` fallback shader.

**Terminids are skinned, animated meshes at every distance.** They are made
cheap by decimation and by the shared-buffer instancing of §1, not by ever
becoming a card.

---

## 3. The LOD ladder is a policy, not an art decision  [BUILD]

Three creatures spanning 5× in complexity, exported with LODs:

| | LOD0 | LOD1 | LOD2 | LOD3 | step ratio |
|---|---:|---:|---:|---:|---:|
| Charger | 35,722 | 10,715 | 3,214 | 964 | 0.30 / 0.30 / 0.30 |
| Hunter | 12,303 | 3,689 | 1,105 | 330 | 0.30 / 0.30 / 0.30 |
| Scavenger | 7,311 | 2,190 | 656 | 194 | 0.30 / 0.30 / 0.30 |

**Four levels, and the same ~30% decimation at every step for every creature.**
**[inferred]** That uniformity across assets of very different budgets says the
ladder is a **pipeline setting** rather than per-asset authoring — three LOD
generations at a fixed ratio, applied to whatever the artist delivered. Cheap to
run, impossible to get inconsistent, and it puts the swarm units where they need
to be: a Scavenger at LOD3 is **194 triangles**, so a hundred distant ones cost
less than one near Charger.

The shadow chain confirms the rule from
[`helldivers2_creatures.md`](helldivers2_creatures.md) §2.2 across all three:

| | shadow LOD1 | game LOD2 |
|---|---:|---:|
| Hunter | 1,107 | 1,105 |
| Scavenger | 656 | 656 |

**The shadow mesh's ladder is the game ladder shifted one step down** — exact for
the Scavenger, two triangles off for the Hunter. With its own topology and its
own `m_character_shadow` material, so the shadow pass never evaluates the wound
lookup, the subsurface or the iridescence.

### 3.1 The unit is more branches than the first pass found

Extracting eight Terminids rather than one shows the full set of sibling
branches under a unit root, and the Charger did not have all of them:

```
skeleton      animation rig
game_mesh     visible geometry + LOD chain + damage/gib parts
shadow_mesh   shadow-only geometry + its own LOD chain
damageables   hit-zone hierarchy   (spelled `damageable` on the Stalker)
collision     physics proxy
culling       a dedicated culling proxy
high_poly     shipped on the Scavenger
```

**A separate `culling` branch is the notable one.** **[inferred]** Visibility is
tested against purpose-built geometry rather than against the render mesh's
bounds or the collision proxy — the same "render geometry and query geometry are
different assets" rule that [`ruse.md`](../../strategy/ruse.md) §5 and
[`dcs_clouds.md`](../../flight/dcs/dcs_clouds.md) §11 both arrive at, here
applied to culling.

Bone counts across the roster: Shrieker 52, Warrior 55, Scavenger 60, Hive Lord
69, Hunter 74, Impaler 75, Strider 82, **Stalker 115**.

---

## 4. The rig: how six legs are no harder than two  [BUILD]

The question "how does the alien rig work when aliens have more than four legs"
has a flatter answer than it deserves: **it does not need to work differently.**

Linear-blend skinning does not care about anatomy. A bone is a matrix; a vertex
names four of them. Six legs is thirty-odd more matrices in `bdata`. The rig is
a naming convention, and the Terminids have one:

```
front_leg_{1..4}_{l,r}     middle_leg_{1..4}_{l,r}     back_leg_{1..4}_{l,r}
spine{1..4}   neck1   head1   head2   headplate1
upper_jaw{1,2}   lower_jaw{1,2}   pincer_{l,r}
shield{1..6}          butt_flab{1..3}
aim_root   voice
```

Six legs × four segments, recovered identically on the **Charger** and the
**Impaler** — two creatures of very different silhouette sharing one limb
convention. (Complete with a shipped typo: the Impaler's is `front_reg_r`.)
**[inferred]** That is a **template rig with per-creature proportions**, not a
bespoke skeleton per species, which is what lets one set of animation authoring
conventions and one set of state-machine variable names serve the whole faction.

Two bones carry no anatomy and both matter: **`aim_root`** (where aim/lookat is
resolved) and **`voice`** (where Wwise attaches its emitter — audio position is a
rig concern, decided at rig time).

**Terrain adaptation without IK.** [`helldivers2_creatures.md`](helldivers2_creatures.md)
§1 flagged that only 12 units in the game have an `ik_skeleton`, and no Terminid
sprinter is among them. §5's variable list resolves how they cope anyway: the
Charger's state machine takes **`slope_pitch` and `slope_roll`**, with a
`slope_update` event and a `Group_State/slope_rotation` state on an additive
layer. **The body is oriented to the ground by two floats on an additive layer,
and the feet are left to fall where they fall.** On a runtime-generated planet
surface that is the cheap 90% — and it is why the expensive answer was reserved
for the tall slow walkers where the feet are what you look at.

---

## 5. The state machine, and the correction it forces  [BUILD]

`state_machine` resources decode to readable JSON. The Charger's is 504 KB.

| | layers | states | clip refs | additive | variables | blend masks | events |
|---|---:|---:|---:|---:|---:|---|---:|
| **Charger** (Terminid) | 15 | 121 | 180 | 18 | **8** | 3 × 58 bones | 113 |
| **Conscript** (Automaton) | 17 | 222 | 488 | 11 | **9** | 8 × 67 bones | 123 |
| **Helldiver** (player) | 31 | **2,318** | **5,890** | 153 | **54** | 80 × 90 bones | 1,252 |

**That table is the animation-scaling answer.** The player has **19× the states
and 33× the clip references of a Charger**, and 54 animation variables against
eight. The AI is not made affordable by a clever runtime trick; it is made
affordable by **being authored an order of magnitude smaller**, and the budget
saved goes into the one character the player is looking at from three metres.
Compare [`broken_arrow_animation.md`](../../flight/broken_arrow/broken_arrow_animation.md),
which found a four-float, twenty-event channel for an RTS unit — same principle,
different scale point on the same axis.

State types are `Clip`, `Blend`, `Empty`, `Time` and — worth its own line —
**`Ragdoll`**. Ragdoll is a **state in the animation graph**, not a handoff to
another system, and the events name a graded model:
`ragdoll_powered_1/2/3`, `ragdoll_kinematic`, `ragdoll_fixed`, with states
`Ragdoll_States/Death_Powered_Weak | _Medium | _Strong`. **[inferred]** Death is
a blend from animated to fully simulated with three intermediate stiffnesses,
selected by how the creature died — which is why a Charger shot mid-charge
crumples differently from one that bleeds out.

### 5.1 The simulation → animation channel, in full

This is the entire interface between the AI and the rig.

**Charger — 8 floats:**
`move_speed`, `move_angle`, `yaw`, `turn_rate`, `turn_adjust`,
`slope_pitch`, `slope_roll`, `hurt`

**Conscript — 9 floats:** the same locomotion core, then
`aim_horizontal`, `aim_vertical`, `steering_turn`, and **`aware_state`**

**[inferred]** `move_speed` + `move_angle` is a 2D locomotion blend space; the
three turn variables drive turn-in-place; the two slope variables are §4's
additive body orient. **Everything else the AI wants to say, it says as an
event** — which is why there are 113 of them and only eight floats.

### 5.2 What this says about AI thinking — and where the first pass was wrong

[`helldivers2_creatures.md`](helldivers2_creatures.md) §5 and
[`helldivers2_navigation.md`](helldivers2_navigation.md) §6 both concluded that
this install "cannot say how a Terminid decides anything", on the grounds that
no behaviour-tree or blackboard resource type exists. The first half is still
true — there is no behaviour representation in the inventory, and the decision
*logic* is compiled into an obfuscated binary.

**But the conclusion was too strong.** The state machine is the interface the AI
drives, so its event and state names are the AI's **externally visible
vocabulary**, and that is a great deal more than nothing.

Terminid, from the Charger's 72 named events and 56 named states:

```
ambient tasks   task_sleeping  task_eating  task_investigate(_loop)  task_done
alerting        bark_target_aquired            (Arrowhead's typo, kept)
guarding        guarding_locomotion  idle_guard
attack          charge  charge_accelerate  charge_slow  charge_break
                action_charge_hit / _impact / _end
                action_melee_front / _left / _right / _side
social/failure  taunt_major   taunt_unreachable
spawning        action_burrow
                Spawn/Spawn_Bughole_Left | _Mid | _Right
damage          hit_{light,large,small}_{front,back,left,right}
                knockdown  stunned  stunned_refresh  bleed_dead  action_gore
```

Automaton, from the Conscript:

```
awareness       suspicious_enter / _exit → state_investigate → investigate_look
                task_scanning        + the `aware_state` variable
squad           Actions/Alert_Leader        Actions/order_point
weapons         wield_cannon / _flamer / _pistol   weapon_draw / _holster
                reload_normal / _sniper / _cannon  brace_cannon
                fire_rifle  fire_cannon  Recoil/*  sniper_crouch_{enter,idle,exit}
movement        flight_locomotion(_enter/_exit)  Rocket_Jump
                vehicle_exit / _falling / _land
death           death_head  death_chest  death_leg  death_powered
```

Three findings that are not available any other way **[inferred]**:

1. **The two factions have structurally different AI.** The Automaton has a
   **graded awareness ladder** — idle → `suspicious` → `investigate` →
   engaged — with a dedicated `aware_state` float and a `task_scanning` sweep.
   The Terminid has **no awareness variable at all**: it has ambient *tasks*
   (sleeping, eating, investigating) and a single `bark_target_aquired`. Bugs
   detect or they don't; robots ramp. That is a design difference readable in
   data, and it matches how the two factions play.
2. **`Actions/Alert_Leader` and `Actions/order_point` mean Automaton squads have
   a command relationship**, not just co-located individuals.
3. **`taunt_unreachable` means the AI has a name for navigation failing.** When
   a Charger cannot path to you, that is not a silent stall — it is a state with
   an animation, which is the single most player-facing decision in
   [`helldivers2_navigation.md`](helldivers2_navigation.md)'s whole subject.
   Frykholm's *"everybody will notice if the AI gets stuck running against a
   wall"* is answered here by **giving stuck a performance**.

The honest boundary, restated: we can enumerate the states the AI can be in and
the events it can raise. We cannot see the *policy* that chooses between them —
no scoring function, no thresholds, no timings. Treat §5.2 as a vocabulary, not
a behaviour model.

---

## 5.3 Limb loss: rich geometry, almost no gait  [BUILD]

The obvious follow-up to §5 and to
[`helldivers2_creatures.md`](helldivers2_creatures.md) §2.3 is whether a bug that
has lost a leg *moves* differently, or only *looks* different. The answer is
sharply lopsided, and it is another instance of the gradient in §3 and in
[`helldivers2_combat.md`](helldivers2_combat.md) §2.

**Geometry: detailed, and it scales with the creature.**

| Creature | damaged / gib meshes |
|---|---|
| Strider | **19** — `leg_{left,right}_{front,back}_{inner,outer}_{top,bottom}_gibs` (12), `claw_{left,right}_gibs`, `torso_gibs` + `_left` + `_right`, `head_gib`, `head_damaged` |
| Charger | **15** — 9 `_damaged` (head, butt, `butt_hurt`, both flanks, all four legs), 6 `_gib` (both claws, all four legs) |
| Impaler | 5 `_damaged`, plus `head_shield_gibs` |
| **Hunter** | **0** |

The Strider's leg gibs are resolved to **inner/outer and top/bottom per leg** —
the most granular dismemberment in the Terminid roster. The Hunter has none at
all: small bugs do not come apart, they die. The rig carries pre-authored stump
bones for the parts that do (`back_leg_3_l_gore1`, `middle_leg_2_r_gore1`).

**Gait: four clips in the entire game.** Searched across every named animation
resource, `limp` / `crippled` / `maimed` returns exactly:

```
content/fac_bugs/cha_strider/animations/walk_limp
content/fac_bugs/cha_strider/animations/walk_slow_limp
content/fac_cyborgs/cha_lieutenant/animations/locomotion/walk_limp_left
content/fac_cyborgs/cha_lieutenant/animations/locomotion/walk_limp_right
```

**One Terminid and one Automaton.** No Charger limp, no Hunter limp, no Warrior
limp, no Stalker limp. The Strider gets two (paired with its `walk` and
`walk_slow`) and they are not side-specific; the Automaton Lieutenant gets
**left and right** variants, so its limp knows which leg was hit.

What every other creature gets instead is **pose-level, not gait-level**:

* a **`hurt`** animation variable (one of the Charger's eight, §5.1),
* a `idle_01_hurt` clip — a *hurt idle*, not a hurt walk,
* eight additive hit-reaction states on two dedicated layers —
  `Group_State/hit_large_{front,back,left,right}` and
  `hit_small_{…}` — which perturb the current pose rather than replacing the
  locomotion.

**[inferred]** So losing a leg changes what a Charger *looks* like in
considerable detail and changes what it *does* not at all, at the animation
layer. That is the right call: a Charger is only ever seen sprinting at you or
dead, and authoring a six-legged limp blend space for a creature nobody watches
walk would be the single worst return on animation budget in the game.

**The exception proves the rule, and it is the same creature every time.** The
Strider is the *only* Terminid with a limp. It is also the only Terminid with an
**IK skeleton** (§4, one of just 12 in the game) and the one with the most
granular **leg gibs**. Three independent systems, one creature — because it is
the tall, slow, long-legged one whose legs you actually look at.

### The nuance that matters more than the limp

**Movement can change without any animation changing.** Locomotion is a
multi-clip blend — the Charger's locomotion layer holds seven blend states of
5 and 10 clips each — driven by continuous variables including `move_speed` and
`move_angle`. **Halving a wounded creature's `move_speed` makes it visibly
slower, with the correct foot timing, at zero asset cost.** Nothing new has to
be authored, and nothing in the animation graph has to know a leg is missing.

Whether Helldivers 2 actually applies a speed penalty for a destroyed limb is in
`generated_damage_settings.dl_bin`, which is encrypted, so this study cannot say.
What it can say is that **the mechanism is free and the animation system is
built for it** — and that this is the general lesson: *a blend space driven by
continuous variables gives you graded damage response without graded animation.*

> **A caveat on this section's evidence.** filediver reports
> `blend_variable_index = 0` for every blend state in every layer, which decodes
> as `slope_pitch` and is not credible for a charge-locomotion blend. That field
> is either unparsed or defaulted in the export, so **this note does not claim
> which variable drives which blend** — only that the blends exist, that they
> carry 3–10 clips each, and that `move_speed` and `move_angle` are in the
> variable set.

### Where the real injury model lives: the player

The deepest limb-damage system in Helldivers 2 is not on any enemy. From the
Helldiver avatar's 606 named events:

```
legs_disabled    legs_disabled_done    legs_disabled_ragdoll
limp_left        limp_right            exit_limp
crawl            crawl_enter_done      downed
powered          powered_downed        powered_downed_exit
heal_bleeding    heal_concussion       heal_fracture     heal_adrenalin
death_acid  death_fire  death_gas  death_electrocution  death_headshot
```

**[inferred]** That is a real injury model — **bleeding, concussion and fracture
as distinct conditions with distinct heal animations**, a leg-damage ladder of
limp → `legs_disabled` → `crawl` → `downed`, and a `legs_disabled_ragdoll`
variant so the collapse is physical rather than a canned fall. `powered_downed`
ties it to the graded powered-ragdoll system of §5.

The Automaton Lieutenant is the only NPC that mirrors any of it, with
`crawl_enter`, `crawl_fwd`, `downed_fwd` beside its two limps — which is why a
legless Devastator crawling at you is memorable and a legless Charger is not a
thing you have ever seen.

**The shape of the whole system is an inverted pyramid**: everything on the
player, a fraction of it on one elite enemy, two clips on one tall bug, and for
every other creature in the game, damage is geometry and a `hurt` float.

---

## 6. What is worth taking

1. **Put bone matrices in one global buffer and pass an offset.** Not a
   per-character constant buffer. This is the single decision that makes
   hundreds of skinned agents affordable, and it is not exotic. §1.
2. **Same for per-instance state.** `idata` at a fixed stride, indexed by
   instance — world matrix, LOD fade, seed, visibility mask all live there. §1.
3. **Don't reach for VAT/impostors first.** A shipped horde shooter with a
   hundred agents on screen uses plain 4-influence LBS and a 30% LOD ladder, and
   never draws a creature as a card. §2, §3.
4. **Make the LOD ladder a pipeline policy, not an art task.** Four levels at a
   fixed ratio, uniform across a 5× complexity range. §3.
5. **Give the shadow mesh its own chain one step down and its own material.**
   The shadow pass then costs nothing it doesn't need. §3.
6. **Two floats of slope on an additive layer beats IK for anything fast.**
   Reserve foot placement for slow, tall, long-legged things. §4.
7. **Keep the sim→anim channel tiny and event-heavy.** Eight floats and 113
   events for an enemy; the floats are continuous quantities only, everything
   discrete is an event. §5.1.
8. **Make ragdoll a state type in the graph with graded stiffness**, not a
   handoff. §5.
9. **Give "stuck" an animation.** `taunt_unreachable` converts the worst-looking
   AI failure into a character beat. §5.2.
10. **When reading shipped shaders, check for a *reference*, not a
    declaration.** Generated constant-buffer layouts declare far more than any
    one shader uses, and this study got it wrong once already. §1.
11. **Answer limb loss with geometry and a scalar, not with a gait.** Nineteen
    gib meshes and a `hurt` float beat authoring a limp blend space for a
    creature nobody watches walk. Reserve the limp for the one silhouette that
    earns it. §5.3.
12. **A blend space driven by continuous variables gives graded damage response
    without graded animation.** Drop `move_speed` and a wounded creature is
    slower with correct foot timing, for free. §5.3.
