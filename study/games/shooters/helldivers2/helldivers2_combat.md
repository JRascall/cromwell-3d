# Helldivers 2 — stratagems, armour and where a bullet actually lands

Two subjects in one note, because the readable evidence for each is thin on its
own and they are the same loop: **stratagems are how force is delivered, the
damageable hierarchy is what it lands on.** Parent note:
[`helldivers2.md`](helldivers2.md).

Be warned about the evidence up front. Both subjects have their tuning in
`data/game/*.dl_bin`, and every one of those is encrypted:

```
generated_stratagem_settings          79 KB     generated_damage_settings      48 KB
generated_stratagem_upgrade_settings   181 B    generated_projectile_settings  93 KB
generated_explosion_settings          66 KB     generated_hit_effect_settings  57 KB
generated_arc_settings               1.7 KB     generated_beam_settings       3.4 KB
generated_status_effect_settings      11 KB     generated_collision_event_settings 14 KB
generated_surface_effect_settings    304 KB
```

**No number below comes from any of those.** What this note has is asset naming,
rig structure and shader constants, which turn out to answer the *architecture*
questions cleanly and none of the *balance* questions at all.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. Armour is bones  [BUILD]

Comparing the rigs of eight Terminids reveals a convention that is not in any
document:

| Creature | armour bones |
|---|---|
| Charger | `headplate1`, `shield1` … `shield6` |
| Impaler | `headplate1`, `shield1` … `shield6`, `head_shield`, **`head_shield_gibs`** |
| Strider | `backplate1` … `backplate6` |
| Warrior | `head_plate`, `l_shoulder_shield`, `r_shoulder_shield` |
| Hunter | `head_plate` |

**Armour plates are rig elements.** They deform with the animation, they are
individually named, and — the giveaway — the Impaler ships a
**`head_shield_gibs`** mesh, so a plate is a thing that can be *shot off* and
leave debris.

**[inferred]** That settles the representation question. Helldivers 2's armour
is not a scalar on a hitbox and not a material property; it is **geometry
attached to named bones**, which is why armour on this game reads as *a place on
the model* rather than a stat. It is also why the community talks about the
Charger's "butt" and the Hulk's eye: those are literally different bones with
different plates over them.

---

## 2. Hit zones are typed collision primitives, and they scale with lifespan  [BUILD]

[`helldivers2_creatures.md`](helldivers2_creatures.md) §2.1 found the Charger's
`damageables` branch — a second, coarser skeleton parallel to the animation rig.
Reading it across the roster shows two things that one creature could not.

**Zones are typed by collision primitive.** The Impaler's zone names carry a
suffix:

```
c_back_leg_1_l_capsule     c_back_leg_2_r_capsule      ← limbs: capsules
c_back_leg_3_l_convex      c_head1_convex              ← extremities, head
c_boss_convex              c_headplate1_convex
c_shield3_convex           c_shield5_convex            ← armour: convex hulls
```

**[inferred]** Capsules for limb segments, convex hulls for heads and armour
plates. So a shot is resolved against a **primitive per zone, chosen for the
shape of the thing** — cheap swept capsules along legs, accurate hulls where
penetration angle matters. That is exactly the representation a
penetration-versus-angle model needs, and it explains why armour in Helldivers 2
is directional in a way that a per-hitbox scalar could never be.

**The number of zones tracks how long the creature lives:**

| Creature | `c_` damage zones |
|---|---:|
| Hunter | **1** (`c_collision`) |
| Warrior | **1** (`c_collision`) |
| Strider | 9 |
| Impaler | 23 |
| Charger | 27 |
| Hive Lord | **41** |

**Hunters and Warriors have no hit zones at all** — one body capsule and nothing
else. The Hive Lord has forty-one, including `c_body_collision_spine0/2/4` for a
segmented body and `c_inner_lower_jaw0/1` for the inside of its mouth.

**[inferred] This is the cleanest cost/benefit gradient in the whole build.**
Zone complexity is proportional to *time on screen under fire*. A Hunter dies to
a burst; nobody will ever aim at its knee, so it does not have one. A Hive Lord
is a two-minute fight, so it gets a mouth interior. The rule generalises past
this game and past damage: **detail budget belongs where the player's attention
dwells, and lifespan is a good proxy for attention.** It is the same
reasoning that gave only 12 units in the game an IK skeleton
([`helldivers2_creatures.md`](helldivers2_creatures.md) §1) and the same
reasoning behind the wound system's two resolution tiers
([`helldivers2_animation.md`](helldivers2_animation.md) §1.1).

The Hive Lord also carries a `c_culling` **bone**, matching the `culling`
branch found on most units — culling geometry is a rig member, not a bounding
box.

---

## 3. What the animation graph says about being hit  [BUILD]

From the decoded state machines
([`helldivers2_state_machines.txt`](helldivers2_state_machines.txt)), the damage
vocabulary is richer than the material evidence suggests:

```
Charger:    hit_light_{front,back,left,right}      hit_light_done
            Group_State/hit_large_{front,back,left,right}
            Group_State/hit_small_{front,back,left,right}
            knockdown  stunned  stunned_refresh  stunned_end
            bleed_dead  action_gore  death_end
Conscript:  death_head   death_chest   death_leg   death_powered
            Miss_Staggers/Miss_Stagger_{Left,Right}
```

Three readings **[inferred]**:

* **Hits are classified by direction *and* magnitude** — light / small / large,
  each with four directions, as separate animation states. Twelve reaction
  states before you count knockdown and stun.
* **`stunned_refresh`** means stun is a *refreshable duration*, not a
  one-shot — consistent with stun grenades and EMS stacking rather than
  re-triggering.
* **`Miss_Stagger_Left` / `_Right` on the Automaton is the best one.** A stagger
  animation for a shot that **missed** — the robot flinches from near-misses.
  That is a deliberate legibility feature: suppression that the player can see
  without a HUD element.

Deaths differ by faction in a way that matches §2's zone counts: the Automaton
has `death_head` / `death_chest` / `death_leg` — death animations selected by
*which zone was fatal* — while the Charger's deaths are `Deaths/Charging` plus
the graded ragdoll states.

---

## 4. Stratagems: the delivery side  [BUILD]

71 stratagem-related units, 48 dedicated audio banks
([`helldivers2_audio.md`](helldivers2_audio.md) §3), and a very legible split
between the three places a stratagem exists.

**On the ship, as visible hardware.** Eagle armaments are *ship upgrade module*
units:

```
env_ship/upgrade_modules/eagle_weapons/eagle_500kg
env_ship/upgrade_modules/eagle_weapons/eagle_500kg_fourstack
env_ship/upgrade_modules/eagle_weapons/eagle_airstrike
env_ship/upgrade_modules/eagle_weapons/eagle_airstrike_upgraded
env_ship/upgrade_modules/eagle_weapons/eagle_rocket_pods
env_ship/upgrade_modules/eagle_weapons/eagle_gun_pods
env_ship/upgrade_modules/eagle_weapons/eagle_air_to_air_missile
env_ship/hangar/se_destroyer_supportcannon_orbital_cannon_single
env_ship/hangar/se_destroyer_supportcannon_orbital_gatling_single
env_ship/bridge/hellpod_delivery_system    env_ship/bridge/hellpod_launcher
env_ship/hangar/robotics_module_hellpods
```

**[inferred]** `eagle_500kg_fourstack` and `eagle_airstrike_upgraded` as
*separate units* means the hangar visibly reflects your upgrade state — the
loadout is modelled hardware, not a menu value. That is unusually expensive and
entirely in keeping with the game's tone.

**On the ground, as deployables.** Everything callable lives under
`fac_helldivers/hellpod/`:

```
hellpod/aa_turret            hellpod/aa_turret_wreck
hellpod/ammo_rack            hellpod/ammo_box
hellpod/antitank_emplacement hellpod/drilling_charge
hellpod/landingzone_beacon   (with animations: deploy / retract / undeployed)
```

The beacon carries its own `bones`, `physics`, `state_machine` and a three-clip
animation set. **[inferred]** A thrown beacon is a full entity with a rig, not a
particle — which it has to be, because it bounces, lands on uneven generated
terrain, and can be picked up and thrown back.

`aa_turret` + `aa_turret_wreck` is §1 of
[`helldivers2_destruction.md`](helldivers2_destruction.md)'s two-state pattern
applied to your own equipment.

**In the UI, as a hologram.** The system
[`helldivers2_rendering.md`](helldivers2_rendering.md) §8 found is the
stratagem/map display:

```
c_hologram_common     hologram_position    hologram_sphere
hologram_wp_to_real_wp0 … _wp3       hologram_lower_upper_bounds(_fade)
hologram_fade_power   hologram_no_fade_distance   hologram_distortion_factor
hologram_separate_grid    hologram_overlay_color
hologram_filtering                                        (named pass)
```

`hologram_wp_to_real_wp0..3` is a four-row **world-position remap matrix**.
**[inferred]** The hologram is not a model of the battlefield — it is *the real
world space transformed into the table's volume*, clipped by
`lower_upper_bounds` and faded at the edges. That is why the tactical map agrees
with the terrain you are standing on, on a map that was generated ninety seconds
ago and for which no map art could possibly exist. **The procedural constraint
forced the good solution again.**

### 4.1 The stratagem roster, read off the audio banks

The 48 stratagem banks are the cleanest enumeration of the system in the build,
and they group into five mechanical families — sentries (8), eagle strikes (8),
orbital strikes (10), backpacks (9), and emplacements/mines/barriers (13):

```
eagle_    500kg_bomb  airstrike  airstrike_smoke  clusterbombs
          gas_strike  napalm_strike  strafing_run  110m_rockets
orbital_  380mm_he  380mm_he_dss  airburst_strike  ems_strike  gas_strike
          gatling_barrage  laser  napalm_barrage  precision_strike  rail_cannon
          smoke_barrier
sentry_   auto_cannon  flamethrower  gatling  laser_cannon  machine_gun
          mortar  mortar_gas  rocket  static_field
packs     jump_pack  hoverpack  displacement_pack  supply_pack
          shield_generator_pack  ballistic_shield_pack  bomb_backpack
          c4_backpack  guard_dog{,_flamethrower,_gas_projector,_stun}
static    anti_personnel_mine  anti_tank_mine  gas_mine  incendiary_mine
          anti_tank_emplacement  defense_wall  directional_energy_shield
          shield_generator_relay  mini_silo
```

**[inferred]** `orbital_380mm_he_dss` as a *separate bank* from
`orbital_380mm_he` is worth flagging — DSS is the Democracy Space Station, a
galaxy-level structure that modifies planetary missions. A community-controlled
strategic asset reaching down into a per-mission audio bank is the same
war-state-into-level-content coupling as the `narrative_node_lvl*_destruction`
units in [`helldivers2_destruction.md`](helldivers2_destruction.md) §2.

---

## 5. The damage systems that exist but stay shut  [BUILD]

Named, sized, encrypted. Recorded because the *existence* of a separate settings
file is architectural evidence even when the contents are not:

| File | Size | Implies |
|---|---:|---|
| `generated_projectile_settings` | 93 KB | projectiles are simulated entities with per-type config, not hitscan |
| `generated_arc_settings` | 1.7 KB | arc weapons (Blitzer, Arc Thrower) are a distinct propagation system |
| `generated_beam_settings` | 3.4 KB | beams (laser cannon, orbital laser) likewise |
| `generated_explosion_settings` | 66 KB | explosions are a system, not a damage number |
| `generated_status_effect_settings` | 11 KB | burning, gas, stun, EMS as tracked states |
| `generated_hit_effect_settings` | 57 KB | impact VFX/SFX selected per hit |
| `generated_surface_effect_settings` | **304 KB** | per-surface impact response — see below |
| `generated_collision_event_settings` | 14 KB | collision → gameplay event routing |
| `generated_damage_settings` | 48 KB | the damage/armour model itself |

**Three separate weapon-propagation systems** — projectile, arc, beam — is the
finding here. The arc and beam files being tiny (1.7 KB, 3.4 KB) next to
projectiles at 93 KB **[inferred]** says arc and beam are a handful of global
parameters and few weapons, while projectiles carry per-type ballistics for the
111 weapons in the audio bank list.

`generated_surface_effect_settings` at **304 KB** is the largest of them and
closes the loop begun in [`helldivers2_audio.md`](helldivers2_audio.md) §3.1:
the same surface taxonomy that gives the game 14 foley banks and the player's
animation graph a `step_switcher` also drives what a bullet does when it hits
sand versus flesh versus ice. **One taxonomy, decided when the terrain is
generated, consumed by four systems.**

---

## 6. What is worth taking

1. **Make armour geometry on bones, not a scalar on a hitbox.** It animates for
   free, it can be shot off, and it makes armour a *place* players can learn.
   §1.
2. **Type your hit-zone primitives to the shape of the part** — capsules along
   limbs, convex hulls at heads and plates. It is what a directional penetration
   model needs. §2.
3. **Scale hit-zone count with expected lifespan.** One capsule for a trash mob,
   forty-one zones for a boss. Detail belongs where attention dwells. §2.
4. **Classify hit reactions by direction *and* magnitude**, and give near-misses
   a stagger. Suppression the player can see beats a HUD element. §3.
5. **A remap matrix, not a map asset.** If the world is generated, the tactical
   display has to be a transform of the real world space. §4.
6. **Separate propagation systems for projectile, arc and beam** rather than one
   parameterised weapon type. §5.
7. **Model the loadout as visible hardware** if tone allows — the hangar
   reflecting your upgrades costs assets and buys a great deal. §4.
