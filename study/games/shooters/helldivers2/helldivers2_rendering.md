# Helldivers 2 — the frame, read from shader reflection

How Helldivers 2 renders, reconstructed from the retail install. Parent note:
[`helldivers2.md`](helldivers2.md). Creature-specific rendering has its own
note: [`helldivers2_creatures.md`](helldivers2_creatures.md).

**Particles are deliberately out of scope here** because they already have a
better-sourced note: [`helldivers2_vfx.md`](helldivers2_vfx.md) works from
*decompiled bytecode* rather than the reflection names used below, and reaches
conclusions this method cannot — 189 distinct shader families, a per-material
generated `c_billboard` layout, TAA jitter applied in the vertex shader, and
soft particles sampled from a small mip of the depth pyramid. Where this note
mentions the depth pyramid or the environment-particle tiers, that note has the
maths.

**The method matters here, so it goes first.** The game's executable has an
obfuscated string table and a packed import table, so the usual reading of a
shipped binary is closed. But the shader libraries are **DXBC**, and DXBC keeps
its reflection chunk (`RDEF`) in the clear because the D3D runtime needs
cbuffer, texture and sampler names to bind against. Sweeping every shipped
shader library for identifiers and ranking them by *how many libraries mention
each* produces a frequency-ordered map of the renderer: a uniform that appears
in 318 of 813 libraries is in the global per-frame constant buffer; one that
appears in 2 is a single system.

The raw output is beside this note as
[`helldivers2_shader_vocab.txt`](helldivers2_shader_vocab.txt) — 1,596
identifiers from **813 shader libraries holding 2,153 DXBC blobs**, pulled
2026-08-09. The 173 **named** shader libraries in the 2026-08-15 inventory give
the pass names. Reading the two together is what this note is.

> **What reflection cannot tell you.** It names resources and constants; it does
> not order passes, does not give dimensions or formats, and does not cost
> anything. Everything below tagged **[BUILD]** is a name that is genuinely
> there. Everything about *how the frame is assembled* is **[inferred]** from
> those names, and no capture was taken — `bin/GameGuard` is anti-cheat and
> this machine's rule is that such titles are mined statically, never injected
> into. Where a pass ordering is asserted, treat it as a reading.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. The global frame constants  [BUILD]

One constant block appears in **318 of 813** libraries — everything that draws.
It is the clearest single artefact in the corpus:

```
camera_view              camera_projection         camera_view_projection
camera_inv_view          camera_inv_projection     camera_unprojection
camera_last_view         camera_last_projection    camera_last_view_projection
camera_last_inv_view     camera_last_inv_projection
camera_near_far          cb_camera_pos             camera_center_pos
time                     delta_time                frame_number
global_viewport          vp_render_resolution
raw_non_checkerboarded_viewport
raw_non_checkerboarded_target_size
upscaling_method         post_effects_enabled      imp_transparent_override
vrs_enabled              vrs_tile_size             vrs_coverage_enabled
vrs_single_axis_opacity_multiplier
debug_rendering  debug_lod  debug_shadow_lod  texture_density_visualization
```

Five things are decided by that list alone.

**A full previous-frame matrix set is global.** Every `camera_last_*` variant
ships in the block every shader sees, which means reprojection is not a
post-process concern bolted on at the end — **any** shader can reproject.
Motion vectors, TAA, VRS history and volumetric fog history all draw on it.
`frame_history_invalidation` appears separately in 63 libraries, so history
invalidation is an explicit, widely-consulted signal rather than a heuristic
inside one pass.

**Checkerboard rendering is in the PC build.** `raw_non_checkerboarded_viewport`
and `raw_non_checkerboarded_target_size` exist because, under checkerboarding,
"the viewport" is ambiguous and some shaders need the *un*-checkerboarded
dimensions. **[inferred]** This is PS5 inheritance that stayed: the game is a
console-first title whose renderer was built around checkerboard reconstruction,
and rather than strip it for PC they carried the concept and added
`upscaling_method` beside it. The PC build then ships DLSS, XeSS and FSR as
alternative reconstructors (`nvngx_dlss.dll`, `libxess.dll`,
`amd_fidelityfx_upscaler_dx12.dll`).

**Variable-rate shading is a first-class, reprojected system.** Not just
`vrs_enabled` but a whole sub-system:

```
vrs_mask   vrs_mask_history   vrs_mask_shader_read   __tex_vrs_mask_shader_read
vrs_reprojection_usage   vrs_reprojection_bias
vrs_is_edge_threshold    vrs_use_fine_rate_threshold
vrs_single_axis_opacity_multiplier
```

**[inferred]** The mask is generated per frame, **reprojected from the previous
frame's mask**, and thresholded on edge detection — with a `single_axis`
variant, meaning 1×2 and 2×1 rates are used, not only 2×2. `vrs_coverage_enabled`
suggests coverage feeds into it. This is a considerably more developed VRS
implementation than the usual "shade the periphery coarsely", and it is the
kind of thing a game with hundreds of alpha-tested creatures on screen needs.

**Debug visualisations ship in retail.** `debug_lod`, `debug_shadow_lod`,
`texture_density_visualization`, plus named passes
`gbuffer_debug:albedo_visualization`, `:normal_visualization`,
`:velocity_visualization`, `:ssao_visualization`, `:sun_shadow_visualization`,
`:metallic_visualization`, `:density_visualization`, `visualize_ids`,
`visualize_stable_depth`, `object_density_validation_move_around`. That last
one is a *validation* tool, not a viewer. **[inferred]** A team generating
levels at runtime cannot inspect the output offline, so the inspection tooling
has to be in the shipping build — the same forced move as §7's bakers.

---

## 2. A five-target G-buffer  [BUILD]

```
__tex_gbuffer0  __tex_gbuffer1  __tex_gbuffer2  __tex_gbuffer3
__tex_gbuffer_emissive
```

Deferred, with emissive **separated into its own target** rather than packed or
added forward. **[inferred]** A dedicated emissive target is the choice you make
when emissive drives something downstream that needs it isolated — bloom
thresholding, and the `terrain_emissive_*` system (§5), where glowing ground is
a lighting contributor rather than a decoration.

The debug list confirms what the four base targets carry: albedo, normal,
metallic, velocity, and an SSAO channel.

`__tex_linear_depth` (66 libraries) and an explicit `linearize_depth` pass, with
`__tex_linear_depth_mip6` and a `depth_mip` uniform, say a **linear depth
pyramid** is built and sampled at chosen mips — soft particles want a smooth,
cheap fetch rather than a precise one, which is exactly the trick documented in
the reconstructed particle shader beside this note
([`helldivers2_particle.frag`](helldivers2_particle.frag)).

---

## 3. Clustered shading, with a local-light shadow atlas  [BUILD]

The lighting block, appearing in 22 libraries:

```
clustered_shading_data      cs_active
cs_cluster_buffer           cs_cluster_data_size
cs_cluster_size_in_pixels   cs_cluster_sizes
cs_cluster_max_depth_inv_max_depth
cs_light_data_buffer        cs_light_data_size
cs_light_index_buffer       cs_light_index_data_size
cs_light_shadow_matrices_buffer   cs_light_shadow_matrices_size
cs_shadow_atlas_size        cs_camera_view_proj
__tex_local_lights_shadow_atlas   __tex_ies_lookup
```

with light types as named shader libraries:

```
light_source:omni    light_source:omni:shadow_mapping
light_source:spot    light_source:spot:shadow_mapping
light_source:box     light_source:box:shadow_mapping
```

This is textbook clustered shading — a froxel cluster grid, a light data buffer,
a per-cluster light index list — with three points worth extracting:

**Local lights cast shadows, from a shared atlas.**
`cs_light_shadow_matrices_buffer` plus `__tex_local_lights_shadow_atlas` means
each shadowing local light owns a matrix and a rectangle in one atlas texture,
rather than a texture each. **[inferred]** This is the part most hobby clustered
implementations skip and the part that makes a night-time Automaton base look
right, and it is worth knowing that the shipped answer is one atlas plus a
matrix buffer indexed alongside the light data.

**Box lights are a first-class type.** Omni and spot are expected; a box light
is an artist convenience for corridors, hangars and the Super Destroyer's
interior. Cheap to add, and it saves stacking three spots.

**IES profiles.** `__tex_ies_lookup` means real photometric light profiles are
supported, which is a surprisingly high-end feature next to checkerboard
rendering.

The sun is separate, as it should be, with **four cascades**:

```
vp_min_slice0..3   vp_max_slice0..3
shadow_bias_slice0..3   shadow_depth_bias_slice0..3   shadow_scale_slice0..3
shadow_rotation   __tex_sun_shadow_map   __samp_sun_shadow_map_cmp
sun_shadows_enabled   far_shadows   far_shadows_volume
```

and its own pass chain: `sun_shadow_prepare`, `sun_shadow_cutter`,
`sun_shadow_compute`, `sun_shadow_mask`, `sun_shadow_mask:fill`.
**[inferred]** A **screen-space sun shadow mask** computed once and consumed by
everything downstream — cheaper than sampling four cascades in every shading
shader, and it gives the volumetric fog and the cloud shadows a single shared
occlusion term. `sun_shadow_cutter` is unusual; the likely reading is a
culling/clipping step that limits which geometry enters each slice.

Per-slice **scale** as well as bias and depth bias is a detail worth noting:
three tunable numbers per cascade rather than one, which is what you need when
the cascade split has to serve both a Scavenger at two metres and a Bile Titan
at forty.

> **Relevance to this project.** Helldivers 2 is the *deferred* answer to the
> many-local-lights problem this repo intends to solve on the forward side, and
> the two features worth taking are orthogonal to that choice: the **shared
> local-light shadow atlas with a matrix buffer indexed alongside the light
> data**, and the **screen-space sun shadow mask** as a single occlusion term
> consumed by shading, fog and clouds. Neither is specific to deferred, and the
> first is the piece that decides whether local shadows are affordable at all.
> See [`render_scene_architecture.md`](../../../topics/rendering/render_scene_architecture.md)
> for where these would sit.

---

## 4. Sky, atmosphere, clouds and weather  [BUILD]

The largest coherent subsystem in the corpus, and the one with the most
personality.

**Atmosphere** — a physically-parameterised scattering model, in 55 libraries:

```
c_atmosphere_common
rayleigh_beta   mie_beta   mie_height   mie_tint_hax
atmosphere_light_color   atmosphere_light_direction   atmosphere_saturation
fog_enabled   fog_color   fog_parameters   fog_dustiness
fog_sun_intensity   fog_shadow_intensity   fog_light_pollution
fog_light_ambient_intensity
fog_forwardscatter_phase   fog_backscatter_phase   fog_backscatter_lerp
fog_ambient_during_transition_color_boost
atmospheric_lookup       __tex_far_fog   __tex_far_fog_div4_clouds
```

Rayleigh and Mie coefficients with separate **forward and back scatter phase
terms** and a lerp between them is a real scattering model, not a fog curve.
`fog_light_pollution` is a nice tell — a night-side term for lit settlements.
And `mie_tint_hax` is left in the shipped build, which is the most honest
identifier in the corpus and a reminder that everyone's atmosphere has one
fudge factor in it.

**Volumetric clouds**, Nubis-class, with a second high-altitude layer:

```
__tex_volumetric_cloud_noise_combined
__tex_volumetric_cloud_detail_noise_combined
__tex_volumetric_clouds_weather_current
__tex_volumetric_cloud_current_weather_map
__tex_volumetric_clouds_color   __tex_volumetric_clouds_depth_prev_current_frame
__tex_volumetric_clouds_color_probe   __tex_volumetric_clouds_depth_probe
__tex_volumetric_clouds_shadows_final
cloud_shadows_enabled  cloud_shadows_density  cloud_shadows_opacity
cloud_start_height  cloud_height_mult  cloud_noise_offset  weather_map_lerp

__tex_volumetric_current_high_altitude_clouds
__tex_volumetric_current_high_altitude_weather_map
high_alt_cloud_coverage  high_alt_cloud_sharpness
high_altitude_cloud_smudge_offset  high_altitude_cloud_coverage_offset
high_alt_cloud_shadow_total_distance  high_alt_shadow_intensity
c_high_alt_clouds
```

Shape noise plus detail noise, a weather map, `weather_map_lerp` for transitions
between weather states, cloud shadows projected onto the world, and
`depth_prev_current_frame` for temporal reprojection of the raymarch.
**[inferred]** The `_probe` variants are the interesting extra: a low-resolution
cloud colour and depth probe, most likely for lighting and reflection lookups
that must not pay for a full raymarch. Named passes `cloud_filtering`,
`cloud_weather_new`, `cloud_apply_atmosphere`, `high_altitude_clouds` confirm the
layer separation.

**Volumetric fog** as a froxel volume with history:

```
__tex_volumetric_fog_3d_image   volumetric_fog_3d_image_tmp
__tex_volumetric_fog_3d_image_history
volumetric_fog_3d_reprojection_history
near_ground_fog_height  near_ground_fog_density  near_ground_fog_curve
near_ground_fog_color   near_ground_fog_noise_amount   fog_noise_amount
```

A separate **near-ground fog** layer with its own curve and noise, on top of the
froxel volume. **[inferred]** That is the ankle-deep mist that makes swamp and
moor biomes read differently from the same geometry — a cheap per-biome
identity lever.

**Wind is a generated noise field**, shared with the simulation:

```
wind_generator_direction   wind_generator_frequency
wind_generator_lacunarity  wind_generator_ridged_octaves
wind_generator_min_intensity  wind_generator_max_intensity
wind_generator_offset
clear_wind   copy_wind                                (named passes)
```

`lacunarity` and `ridged_octaves` are fBm parameters — ridged multifractal
noise generating a wind field per level, with `clear_wind`/`copy_wind` managing
its double-buffered texture. It backs the single `vector_field` resource in the
game ([`helldivers2_navigation.md`](helldivers2_navigation.md) §5), and there is
a `content/level_generation_settings/wind_shader_settings` entity, so the wind
is generated alongside the terrain.

**Distant weather effects** appear in the *global* block (64 libraries):
`distant_weather_effect0`, `distant_weather_effect1`,
`distant_weather_effects_color`, matching
`generated_distant_war_effect_settings.dl_bin`. **[inferred]** Rain sheets and
distant storms are frame-global state every shader can see, which is how they
tint the whole scene consistently.

**The sky is procedural and rebuilt at runtime** —
`star_generation_seed`, `star_temperatures`, `star_magnitudes`,
`nebula_generation_seed`, `realtime_update_stars`, `realtime_update_nebula`,
`skydome_rotation_row0..2`, `planet_positions`. Covered in
[`helldivers2_worldgen.md`](helldivers2_worldgen.md) §7, because it is a
generation story more than a rendering one.

---

## 5. Terrain and water  [BUILD]

```
__tex_generated_terrain_albedo    __tex_generated_terrain_emissive
__tex_generated_terrain_emissive_direction
__tex_generated_world_height_projection   generated_heightmap_slope
__tex_lighting_heightmap          terrain_displacement_map
heightmap_ao_enabled  heightmap_ao_intensity  heightmap_ao_falloff
heightmap_ao_bias  heightmap_ao_clamp  heightmap_ao_angle_falloff
heightmap_ao_height_fade  heightmap_ao_near_fade  heightmap_ao_near_fade_distance
heightmap_disk_size
terrain_emissive_enabled  terrain_emissive_light_intensity_multiplier
terrain_emissive_fog_intensity_multiplier
```

Two systems worth naming. **Heightmap AO** — nine tunables — is a large-scale
ambient occlusion computed analytically from the terrain heightmap with a disk
size, separate from and complementary to the screen-space `ao_radius` /
`ao_intensity` / `ao_bias` / `ao_falloff` set. **[inferred]** SSAO cannot see a
valley; a heightmap disk query can, and the `near_fade` pair blends between them
so the cheap large-scale term does not fight the expensive small-scale one.

**Terrain emissive is a lighting contributor**, with both a light intensity
multiplier and a *fog* intensity multiplier, plus an
`emissive_direction` texture. **[inferred]** Lava on a magma planet lights the
scene and lights the volumetric fog above it, with a direction so it does not
glow uniformly upward. That is the difference between a magma biome looking
painted and looking lit.

Water is tessellated and its own subsystem:

```
water_enabled  vista_water_height  water_deep_tint  water_shallow_tint
water_murkyness  water_opacity  caustics_intensity  __tex_water_caustics
ripple_res  ripple_displacement  water_bending_enabled  __tex_water_bending
__tex_water_height  __tex_water_patch  __tex_water_rt
replace_water_with_material_height_offset
replace_water_with_material_water_submerge_offset
project_water                                       (named pass)
SV_TessFactor  SV_InsideTessFactor  PCSG            (patch constant shaders)
```

`water_bending` is displacement from things moving through it;
`replace_water_with_material_*` is how the generator swaps water for mud or lava
per biome while keeping the same patch system. **[inferred]** One water
implementation, re-tinted and re-materialised per planet, which is the only way
to afford water across thirteen biomes.

---

## 6. The post chain  [BUILD]

Reconstructed from named passes and their constants; ordering is **[inferred]**.

| Stage | Evidence |
|---|---|
| SSR | `ssr_hiz_pass`, `ssr_hiz_pass_compute`, `ssr_ray_march_pass`, `ssr_ray_march_pass:one_rpp`, `ssr_noisy_upsample`, `__tex_hdr_ssr` |
| Motion blur | `mb_bake_velocity_depth`, `mb_tile_max:horizontal_pass`/`vertical_pass`, `mb_neighbour_max`, `mb_reconstruct_filter_blur` |
| Depth of field | `calculate_coc`, `clear_dof`, `depth_of_field:horizontal_pass`, `:ascending_diagonal_pass`, `:descending_diagonal_pass`, `dynamic_dof_lookup`, `dof_circular_amount` |
| Auto-exposure | `compute_histogram`, `adapt_exposure`, `display_histogram`, `exposure_min_log_luma`, `exposure_max_log_luma`, `__tex_current_exposure` |
| Bloom | `bright_pass`, `bright_pass_compute`, `blend_bloom`, `bloom_level_weights`(+`_night`, `_cont`), `bloom_tint`, `anamorphic_bloom_tint_level0..2` (+ night) |
| Lens | `lens_effects`, `c_lens_flare_common`, `flare_ghosts_scale`, `__tex_sun_flare_visibility_lookup_sum`, `__tex_global_lens_dirt_map`, `bloom_lens_dirt_exponent` |
| Tonemap | `aces`, `aces_slope`, `aces_mid_point`, `aces_min_point`, `aces_max_point`, `aces_coefs_low`, `aces_coefs_high` |
| Grading | 27 `pcg_*` constants — full channel mixer (`pcg_mixer_red_to_blue` …), lift/gamma/gain by range (`pcg_shadows_start`/`_end`/`_balance`), `pcg_temperature`, `pcg_tint` |
| AA / sharpen | `temporal_aa_depth`, `temporal_aa_depth_compute`, `fxaa`, `sharpen_filter`, `sharpen_amount`, `quantize_luma` |
| Utility | `mip_spd` (single-pass downsampler), `bilateral_upsample`, `filter:separable_bilinear_gaussian_5tap_x`/`_y` |

Three observations.

**Motion blur is the McGuire tile-max reconstruction filter**, in full:
tile max in two separable passes, neighbour max, then a reconstruction blur.
That is the good implementation, not the cheap one.

**Almost everything has a `_night` variant.** `bloom_level_weights_night`,
`bloom_tint_night`, `anamorphic_bloom_tint_night_level0..2`,
`lens_flare_bloom_level_weights_night`, and a `night_amount` uniform to blend
by. **[inferred]** Night missions are not the day pipeline with a darker sun —
they are a parallel set of post-processing constants crossfaded by one term.
Cheap, and it is why a night drop reads as a different game.

**`color_grading_cloudy_new_math_enabled`** is a shipped boolean toggling
between an old and a new grading formulation for cloudy conditions. A live
migration flag left in the retail build, and a small reminder that this is a
service game whose renderer changes under players.

`__tex_bluenoise_texture` appears in 69 libraries — blue noise as the standard
dither source across the renderer, not per-effect.

---

## 7. The bakers ship  [BUILD]

The consequence of [`helldivers2_worldgen.md`](helldivers2_worldgen.md) that
shows up hardest in the renderer:

```
path_tracing:lightmap    path_tracing:debug_materials
bake_compute             baker_copy_material_component
baker_material_downsample   baker_indirect_scale
lightmap_average   lightmap_edge_dilate   lightmap_filter
c_ship_hub_probe_bake   ship_hub_probe_lerp
__tex_ship_hub_specular_lerp_from_array  __tex_ship_hub_specular_lerp_to_array
filter_cubemap:diffuse  filter_cubemap:radiance  filter_cubemap:specular
realtime_cubemap_current_side   cubemap_blend
imp_bake  imp_clear  imp_weight_merge  imp_material_count
imp_merge_transparency  imp_override_transparency
```

**A path tracer ships in the retail game.** `path_tracing:lightmap` with
`lightmap_average`, `lightmap_edge_dilate` and `lightmap_filter` is a complete
lightmap bake pipeline — the padding-dilate and filter stages are the giveaway
that this is real lightmap production, not a debug renderer. **[inferred]** It
exists because a level generated at runtime has no lightmaps, and the only place
left to bake them is the player's machine.

The ship hub takes a different route: `ship_hub_probe_bake` with
`specular_lerp_from_array` / `specular_lerp_to_array` and a `probe_lerp`
scalar. **[inferred]** The Super Destroyer *is* an authored level
([`helldivers2_worldgen.md`](helldivers2_worldgen.md) §1), so its probes can be
baked ahead into arrays — several sets, crossfaded as conditions change. The
authored level gets the offline treatment and the generated ones get the runtime
treatment, in the same renderer.

`realtime_cubemap_current_side` says the realtime reflection cubemap renders
**one face per frame**, amortising a cubemap update over six frames. Standard,
and worth remembering because it is the cheapest correct answer.

---

## 8. UI, HUD and screen effects  [BUILD]

**NoesisGUI** for the UI: `c_noesis`, `noesis_projectionMtx`,
`noesis_textureSize`, `noesis_opacity`, `noesis_radialGrad0/1`,
`c_noesis_conic_gradient`, `conic_gradient_stops`, `c_noesis_crossfade`,
`c_noesis_monochrome` — across 79 libraries, matching 181 `xaml` resources. Text
is **MSDF** (`__tex_msdf_texture`, `px_range`) with a separate `__tex_glyphs`
path.

The HUD is curved, as its own shader variants: `gui:curved`, `hud:curved`,
`hud:drop_shadows`, `hud_curve_amount`. **[inferred]** The curvature is a
per-frame scalar, so the "CRT screen" feel is dialled rather than baked into the
layout.

Holograms — the ship's map table, stratagem previews — are a real system:

```
c_hologram_common   hologram_position   hologram_sphere
hologram_wp_to_real_wp0..3    hologram_lower_upper_bounds
hologram_lower_upper_bounds_fade    hologram_fade_power
hologram_no_fade_distance   hologram_distortion_factor
hologram_separate_grid      hologram_overlay_color
hologram_filtering                                    (named pass)
```

`hologram_wp_to_real_wp0..3` is a **4-row world-position remap matrix**.
**[inferred]** The hologram is not a model of the map — it is the *real world
space* transformed into the table's volume, clipped by
`lower_upper_bounds`, faded at the edges. That is why the tactical map is
consistent with the actual terrain.

Player-state screen effects are a named family: `concussion_amount`,
`concussion_ro_ri_gh`, `confusion_amount`, `confusion_rota_rots_cr`,
`mind_scramble_opacity` + `__tex_mind_scramble_texture`, `glitch_amount`,
`glitch_grid_size`, `glitch_distortionamount`, `heathaze_amount`,
`mirror_screen`, `camera_band_amount`, `screen_effect_buffer`. **[inferred]**
Driven from `generated_status_effect_settings.dl_bin`, and worth noting that
they share one `screen_effect_buffer` rather than each owning a pass.

---

## 9. What is worth taking

1. **Put the full previous-frame matrix set in the global block.** Reprojection
   stops being a post-process feature and becomes available to every shader —
   which is what lets VRS, TAA, fog and clouds all reproject without bespoke
   plumbing. §1.
2. **A screen-space sun shadow mask as a shared occlusion term.** Sample the
   cascades once, then let shading, fog and clouds read one texture. §3.
3. **One shadow atlas plus a matrix buffer for local lights.** The part that
   makes clustered shading actually usable, and the part most implementations
   omit. §3.
4. **Analytic heightmap AO alongside SSAO, cross-faded by distance.** Two
   occlusion systems at two scales, because neither covers the other's range.
   §5.
5. **A `_night` variant of every post constant, blended by one scalar.** The
   cheapest possible way to make a time-of-day feel like a different game. §6.
6. **Reprojected, thresholded VRS with an explicit mask history.** Not the naive
   peripheral-VRS. §1.
7. **Ship your debug and validation visualisers.** If content is generated at
   runtime, the only place to inspect it is the runtime. §1, and §7's bakers
   are the same forced move.
8. **`mie_tint_hax`.** Every atmosphere has one. Name it honestly and move on.
