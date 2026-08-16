# Helldivers 2 — five levels, and an assembler for everything else

How Helldivers 2's environments are built, read from the retail install. Parent
note: [`helldivers2.md`](helldivers2.md). This is the note the others depend on:
[`helldivers2_navigation.md`](helldivers2_navigation.md) §2 and
[`helldivers2_rendering.md`](helldivers2_rendering.md) §7 both rest on the fact
established in §1.

"Are the maps procedural?" is a question players argue about from screenshots.
It is settleable from the install in one line, and the answer is not a
qualified yes.

---

## 1. There are five levels  [BUILD]

Every resource of type `level` in the entire game:

```
content/levels/arrival_travel
content/levels/departure_travel
content/levels/empty_test
content/levels/main_menu
content/levels/ship_hub
content/fallback_resources/missing_level      ← fallback
core/fallback_resources/missing_level         ← fallback
(one unnamed)
```

The Super Destroyer's interior. The menu. The two cinematic transitions. A test
scene. **There is no mission map on disk, for any planet, for any biome, for
any faction.**

This is why the inventory is the strongest of the three evidence channels
([`helldivers2.md`](helldivers2.md) Sources): screenshots can suggest
procedural generation and never prove it, but a complete enumeration of a
resource type proves absence, and absence is decisive here. Everything you drop
onto is assembled at runtime.

The corroboration is a DLL. `bin/plugins/` holds exactly two plugins:

```
level_generation_pluginw64_release.dll     542 KB
wwise_pluginw64_release.dll                6.1 MB
```

One is Wwise. The other is a **bespoke Stingray plugin for generating levels**,
written by Arrowhead, loaded through the same `get_plugin_api` ABI as the game
itself ([`helldivers2.md`](helldivers2.md) §2). Stingray did not ship one.

---

## 2. `spherical_voronoi`  [BUILD] [inferred]

The level-generation DLL is a stripped release build — no gameplay literals
survive — but it kept its compiler-emitted source paths, and they name its
only recoverable algorithm:

```
D:\Work\c0b269749c96c578\stingray\runtime\plugins\level_generation_plugin\plugin.cpp
D:\Work\c0b269749c96c578\stingray\runtime\plugins\level_generation_plugin\spherical_voronoi.cpp
```

with the bare strings `spherical_voronoi` and `level_generation` beside them.

Two readings are available and the build lets us weigh them.

**Reading A — planet-surface partitioning.** A Voronoi diagram computed on a
sphere partitions a planet's surface into irregular cells. Supporting evidence
in `data/game/`: `generated_planet_territory_graphs.dl_bin` at **1.5 MB** — a
*graph* of *territories* per planet, which is exactly the adjacency structure a
Voronoi tessellation produces — plus `generated_planet_region_settings.dl_bin`
(61 KB) and `generated_planet_data.dl_bin` (482 KB).

**Reading B — mission-map layout.** Voronoi cells as the zone/district
structure of an individual mission map, with objectives and points of interest
placed per cell.

**[inferred]** Reading A is better supported. The 1.5 MB territory graph is the
kind of artefact you generate once per planet and persist; a mission map's
layout would be generated from a seed and thrown away, and would not need a
shipped 1.5 MB file. A single spherical Voronoi pass over each planet, cached
as a territory graph, also explains how the Galactic War can fight over
*regions* of a planet with stable adjacency. The two readings are not
exclusive — the same plugin plausibly does both — but only A has a shipped
artefact shaped like its output.

**This is the limit of what the DLL yields.** 542 KB of stripped release code,
one recovered algorithm name. Everything below comes from other channels.

---

## 3. The composition grid: archetype × biome × faction × objective  [BUILD]

164 `package` resources. A package is Stingray's streaming unit, and the way
they are cut is the clearest available picture of what the generator composes
from — because **you only need a package boundary where something is
independently included or excluded.**

**Mission archetypes** — five, plus per-faction infiltration variants:

```
mission_defense   mission_horde   mission_extraction_return
mission_infiltration        mission_tutorial
mission_infiltration_bugs   mission_infiltration_cyborg
mission_infiltration_illuminate
```

**Biomes** — thirteen shared environment packages:

```
env_arctic_shared      env_coniferous_shared   env_deciduous_shared
env_desert_shared      env_forest_shared       env_magma_shared
env_moor_shared        env_oasis_shared        env_primordial_shared
env_rocky_shared       env_swamp_shared
```

with matching `utility_<biome>_day` lighting/atmosphere packages —
`utility_arctic_day`, `utility_magma`, `utility_oasis`, `utility_savanna`,
`utility_sandy`, `utility_rocky` — and, tellingly, shipped
`utility_<biome>_editor_testing` packages for five of them. The editor's biome
test scaffolding is in the retail build.

**Factions** — `env_bugs`, `env_cyborgs`, `env_illuminate`, plus
`content/env_super_earth` (557 named units — by far the largest content tree,
the human architecture) and `env_shared`.

**Objectives — one package each, and there are over fifty.** This is the part
worth staring at:

```
gen_evacuate_civilians   gen_extract_civilians   gen_fire_artillery
gen_icbm                 gen_radar_station       gen_raise_flag
gen_seismic_probe        gen_upload_data         gen_restore_power
gen_prospecting_drill    gen_refueling_station   gen_horde_defend   …

bug_destroy_stalker_lair bug_assassinate_chargers bug_retrieve_larva
bug_central_core         bug_oilpump             bug_thumper        …

cy_destroy_airbase       cy_hijack_fabricators   cy_neutralize_cannon
cy_steal_platinum        cy_raze_city            cy_shutdown_grinder
cy_convoy_assault        cy_sabotage_pipes       …

il_board_mothership      il_reclaim_location     il_destroy_weather_device
```

Three prefixes: `gen_` (faction-agnostic), and `bug_` / `cy_` / `il_`.

**[inferred] The streaming granularity *is* the generator's vocabulary.** A
mission is composed by choosing an archetype, a biome, a faction and a set of
objectives, and the package set is exactly that product. The generator loads
`env_swamp_shared` + `utility_swamp_day` + `env_bugs` +
`bug_destroy_stalker_lair` + `gen_upload_data` and nothing else. That is a
notably clean design: **the memory footprint of a mission is determined by what
the composer chose, not by a level author remembering to unload things**, and
it is only achievable because there is no authored level to accumulate
dependencies.

Two more packages name the mechanism directly: **`packages/content/stamp_proxies`**
and **`packages/terrain`**, with `generated_stamp_settings.dl_bin` (674 KB) and
`generated_generation_settings.dl_bin` (10 KB) in `data/game/`. "Stamp" is the
standard term for a prefabricated chunk placed into generated terrain — a
bunker complex, a bug nest, a crashed ship — and a *proxy* is its cheap
stand-in for placement and distant rendering. §4 shows the terrain side of the
same idea.

---

## 4. The terrain editor ships in the retail game  [BUILD]

Named shader libraries in the retail build include:

```
terrain_editor_brush
terrain_editor_brush:sample_based:flatten
terrain_editor_brush:sample_based:box_filter
terrain_editor_brush:sample_based:sample
terrain_editor_brush:sample_height
terrain_editor_brush:sub
terrain_editor_brush_marker
terrain_decoration
height_blend    height_modify
road_blend      road_blend_height
project_water
```

and the reflection data around them carries:

```
replay_brush_pos        part_of_replay_stroke      replay_layer_value
__tex_brush_texture     mask_idx    height_sample    terrain_size
inverse_terrain_wtm     terrain_displacement_map
__tex_generated_terrain_albedo        __tex_generated_terrain_emissive
__tex_generated_world_height_projection    generated_heightmap_slope
generated_materials     __tex_road_direction_target
```

**[inferred]** `part_of_replay_stroke` is the giveaway. A shipping game does not
need a *brush* unless it is painting terrain at runtime, and it does not need
*stroke replay* unless the terrain's definition is **a recorded sequence of
brush operations rather than a heightmap**. The generator emits strokes —
flatten here, subtract there, sample the height, blend a road — and the runtime
replays them on the GPU to produce the heightmap, the albedo, the slope and the
material assignment, all prefixed `generated_`.

That is a materially different design from "generate a heightmap and stream
it", and the reasons to prefer it are good ones:

* **A stroke list is tiny.** A seed plus a few hundred operations reproduces a
  square kilometre of terrain; a heightmap does not. That matters for a game
  where four peers must arrive at the same world
  ([`helldivers2_networking.md`](helldivers2_networking.md)).
* **Composition is trivial.** Placing a stamp is appending strokes — flatten a
  pad, subtract a crater, blend a road to it — rather than merging heightfields.
* **Artists and the generator share one representation.** The `_editor_brush`
  naming says the same shaders serve the authoring tool and the runtime
  generator, so a hand-placed feature and a generated one are the same kind of
  thing. This is the same discipline as
  [`helldivers2.md`](helldivers2.md) §4's rule that the slow path and the fast
  path must remain one implementation.

`road_blend` and `road_direction_target` say roads are a first-class terrain
layer with a direction field, not decals. `project_water` and `__tex_water_patch`
put water in the same generated pipeline.

---

## 5. Deformation is a bounded, masked region  [BUILD]

Separate from generation, and running per-frame:

```
__tex_deformable_terrain_mask     terrain_deformation_max_depth
terrain_deformation_scissor_offset
terrain_deformation_scorch_material_id
```

**[inferred]** A *scissor offset* and a *max depth* mean deformation is not an
arbitrary edit to the world heightmap — it is a clipped region with a bounded
displacement, written into a mask. That is the shape you choose when the result
must be handed to something else cheaply: a localised navmesh rebuild
([`helldivers2_navigation.md`](helldivers2_navigation.md) §4), or a bounded
re-bake. A `scorch_material_id` alongside says the visual scorch and the
geometric crater are one system with two outputs.

It is also, structurally, the same idea as the creature wound buffer
([`helldivers2_creatures.md`](helldivers2_creatures.md) §3): **accumulate damage
into a bounded buffer in the surface's own space, rather than mutating the
authored asset.** Two systems, different teams' problems, identical answer.

---

## 6. Beyond the playable area: vistas and impostors  [BUILD]

A generated map still needs a horizon, and the reflection names a second,
cheaper terrain system for it:

```
__tex_outside_map_vista_heightmap
__tex_outside_map_vista_heightmap_frequency_map
vista_water_height      vista_environment_particles_position_size
__tex_vista_cloud_atlas __tex_vista_cloud_subsurface_atlas
rendering/shader_libraries/vista        (shader library group)
```

**[inferred]** A *frequency map* beside the vista heightmap means the distant
terrain is procedural noise whose octave content varies spatially — a cheap way
to make mountains where the biome wants mountains without storing them. The
vista has its own water height, its own cloud atlas and its own environment
particles: it is a parallel, lower-fidelity world, not a LOD of the real one.

The scatter and impostor systems complete the middle distance:

```
scatter_tiler_lookup   __tex_scatter_albedo_opacity_array
__tex_scatter_normal_array   __tex_scatter_rsh_array
__tex_scatter_subsurface_array

imp_bake   imp_clear   imp_weight_merge   imp_material_count
imp_merge_transparency   imp_override_transparency   imp_map
```

224 `speedtree` resources feed the near vegetation. **[inferred]** `imp_bake`
shipping in the retail build is the same story as the lightmap baker
([`helldivers2_rendering.md`](helldivers2_rendering.md) §7): impostors for a
runtime-generated forest cannot be authored offline, so the impostor baker runs
on the player's machine. `scatter_*_array` being array textures with an
`rsh` (radiance spherical harmonics) slice says scattered instances share a
small pool of slices and are lit by a baked directional term rather than shaded
individually.

---

## 7. The planet and the sky are generated too  [BUILD]

The space and sky systems are procedural on the same terms:

```
star_generation_seed   star_count   star_magnitudes   star_temperatures
star_clearance_scale   realtime_update_stars   skydome_rotation_row0..2
nebula_generation_seed  nebula_macro_scale  nebula_dust_density
nebula_distortion_intensity  realtime_update_nebula
__tex_cosmic_dust_lut  __tex_emissive_nebula_lut  __tex_space_star_lut
planet_positions   space_probe_backdrop   planet_atmosphere_space
planet_processing
```

Two seeds — `star_generation_seed`, `nebula_generation_seed` — and the stars
are described by **magnitude and temperature**, i.e. generated with real
astronomical parameters and coloured through a LUT rather than painted.
`realtime_update_stars` and `realtime_update_nebula` say the skybox is rebuilt
on the GPU rather than shipped as cubemaps, which is what you need when every
planet has a different sky and the planet list runs to hundreds.

`planet_processing` and `planet_atmosphere_space` are named passes; per-planet
atmosphere parameters are why `generated_planet_data.dl_bin`,
`generated_sky_settings.dl_bin`, `generated_weather_settings.dl_bin` (214 KB)
and `generated_weather_color_set_settings.dl_bin` exist. All encrypted; the
names are the evidence.

The extraction tool exposes this seam directly: filediver's `--planet-name`
option applies **per-planet asset overrides**, enumerating the game's planets
as valid values, and `--planet-city` selects city generation. **[inferred]** A
planet is therefore not a level and not a skin — it is a **parameter set that
overrides shared assets**, which is the only representation that scales to a
galaxy map.

---

## 8. What is worth taking

1. **Enumerate the levels.** The single cheapest test of whether a game is
   procedural, and it beats any amount of visual analysis. §1.
2. **Represent terrain as a replayable stroke list, not a heightmap.** Tiny to
   transmit, trivial to compose, and it lets the authoring tool and the runtime
   generator share one implementation. §4.
3. **Cut streaming packages along the generator's vocabulary.** If the composer
   picks archetype × biome × faction × objectives, make those the package
   boundaries, and memory follows the composition for free. §3.
4. **Bound your deformation.** A scissored, depth-limited, masked region is
   handable to the navmesh and the bake systems; an arbitrary heightfield edit
   is not. §5.
5. **Accept that runtime generation moves your whole tool chain into the
   runtime.** Terrain brushes, impostor bakers, lightmap bakers and navmesh
   generation all ship. That is the real price, and it is paid in engine code,
   not content. [`helldivers2.md`](helldivers2.md) §4.
6. **A vista is a separate cheap world, not a LOD.** Own heightmap, own noise
   frequency map, own water height, own clouds. §6.
