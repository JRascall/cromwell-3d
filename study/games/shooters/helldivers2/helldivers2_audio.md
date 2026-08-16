# Helldivers 2 — a quarter of the install is audio

How Helldivers 2 does sound, read from the retail install. Parent note:
[`helldivers2.md`](helldivers2.md).

This note exists because the first pass through this build gave audio one line
in a dependency table, and audio is **31.6 GB of the 127 GB install** — the
largest single asset class by a wide margin, 16,666 streamed files against 1,060
textures. On a build where the engine is eight years dead and the renderer is
carefully economical, the audio is the most expensive thing shipped, and it is
running **the newest middleware in the box**.

Three findings carry the note:

* **Wwise 2024.1.9, upgraded from 2024.1.1** — the audio middleware is current
  to within months of this build, on an engine discontinued in 2018. §1.
* **Wwise Spatial Audio is linked with its whole geometric-acoustics surface** —
  `SetGeometry`, `QueryReflectionPaths`, `QueryDiffractionPaths`,
  `SetNumberOfPrimaryRays`, `ResetStochasticEngine`. §2, with a clear statement
  of what that does and does not prove.
* **The bank taxonomy is the level generator's vocabulary again.** One bank per
  biome, per hazard, per weapon, per stratagem, per walkable surface. The same
  structural answer as objectives in
  [`helldivers2_worldgen.md`](helldivers2_worldgen.md) §3, arrived at by the
  audio team. §3.

Tags as [`helldivers2.md`](helldivers2.md).

---

## 1. Wwise 2024.1.9, and it was deliberately upgraded  [BUILD]

Unlike `game.dll`, the Wwise plugin **kept its source paths**, so the version is
not inferred:

```
C:\work\wwise_libs\2024.1.9_UPGRADE\wwise-2024.1.9.8920-v1\source\SoundEngine\...
C:\Program Files (x86)\Audiokinetic\Wwise 2024.1.1.8691\SDK\source\SoundEngine\...
```

Two versions in one binary, and the newer one lives in a directory called
`2024.1.9_UPGRADE` on someone's working drive. **[inferred]** That is a
migration in progress, or one finished but with a couple of 2024.1.1-era plugin
objects still linked — either way it is evidence of *active maintenance of the
audio stack*, which is a striking contrast with the engine underneath it.

The effect plugins compiled in, read from the same source paths:

```
AkConvolutionReverb  +  AkHalfPrecisionConvolutionEngine
AkFDNReverb          (feedback delay network)
AkAcousticRoom
AkCompressor / AkCompressor2021   AkExpander   AkMeter
AkDelay  AkDelayPitchShift  AkFlanger  AkHarmonizer
AkFXSrcSilence  AkFXSrcAudioInput
```

**Two reverbs, not one.** **[inferred]** Convolution reverb is accurate and
expensive; FDN reverb is cheap and generic. Shipping both is the standard
two-tier answer — convolution for the handful of signature spaces, FDN
everywhere else — and the corroboration that convolution is genuinely used is a
shipped bank named **`content/audio/Helldiver_IR`**. An impulse-response bank
exists for exactly one reason.

`AkHalfPrecisionConvolutionEngine` is worth its own line: convolution at half
precision is a deliberate quality-for-cost trade inside the expensive path.

---

## 2. Geometric acoustics, and what the evidence actually supports  [BUILD]

The Wwise plugin exports 37 `AK::SpatialAudio` entry points, and the set is not
a partial integration:

```
Init  RegisterListener  SetGameObjectInRoom  SetGameObjectRadius
SetGeometry  SetGeometryInstance  RemoveGeometry  RemoveGeometryInstance
SetRoom / RemoveRoom      SetPortal / RemovePortal    RemoveReverbZone
SetPortalObstructionAndOcclusion    SetGameObjectToPortalObstruction
QueryReflectionPaths   QueryDiffractionPaths   QueryWetDiffraction
SetImageSource / RemoveImageSource / ClearImageSources
SetDiffractionOrder    SetMaxDiffractionPaths    SetMaxGlobalReflectionPaths
SetNumberOfPrimaryRays    ResetStochasticEngine    SetLoadBalancingSpread
SetEarlyReflectionsAuxSend    SetEarlyReflectionsVolume
```

That describes **ray-traced geometric acoustics**: the game hands world geometry
to the audio engine (`SetGeometry`), the engine casts primary rays
(`SetNumberOfPrimaryRays`) through a stochastic solver (`ResetStochasticEngine`)
and returns reflection and diffraction paths, bounded by explicit budgets
(`SetMaxGlobalReflectionPaths`, `SetDiffractionOrder`), with the solve spread
across frames (`SetLoadBalancingSpread`).

> **What this proves and what it does not.** These are exports of the *Wwise SDK
> build Arrowhead linked*, so they prove the Spatial Audio module is compiled
> in — **not that the game calls it**. This is the same trap that cost this
> study a correction over `c_per_object` in
> [`helldivers2_animation.md`](helldivers2_animation.md) §1, and it is recorded
> here rather than glossed. Corroborating evidence is circumstantial but
> consistent: an IR bank ships, `AkAcousticRoom` is compiled in, and the game
> has interiors, bunkers and caves where portalled reverb is audible. **Graded
> [BUILD] for the linkage, [inferred] for the usage.**

**[inferred] If it is used, the reason is the same one that governs everything
else in this build.** Acoustics cannot be precomputed for a level that does not
exist until the mission starts
([`helldivers2_worldgen.md`](helldivers2_worldgen.md)). You cannot bake reverb
volumes by hand into a generated map any more than you can bake its navmesh or
its lightmaps. `SetGeometry` at runtime plus a stochastic solver is the audio
department's version of buying Havok Navigation — and it makes audio the
**fifth** system in this study forced from an offline pipeline into shipped
runtime code, after navmesh, lightmaps, impostors and terrain.

---

## 3. The bank taxonomy is the composition vocabulary  [BUILD]

427 named `wwise_bank` resources. Grouped by prefix:

| Prefix | Count | What it is |
|---|---:|---|
| `wep_` | 111 | one per weapon |
| `obj_` | 85 | one per mission objective |
| `stratagems_` | 48 | one per stratagem |
| `foley_` | 20 | player/faction foley, **split by walkable surface** |
| `env_` | 19 | one per biome |
| `haz_` | 15 | one per environmental hazard |
| `bugs_` / `bots_` / `illuminates_` | 36 | one per enemy type |
| `music_` | 8 | per faction and mission type |
| `gore_` | 4 | per faction, plus the player |
| `vehicle_`, `seaf_`, `vo_`, `us/…` | ~20 | vehicles, allied NPCs, VO |

**Every one of those axes is a generator axis.** The mission composer picks a
biome, a faction, an objective set and four loadouts; the bank set that loads is
exactly that product. This is the third independent system in the build to
partition on the same vocabulary — content packages
([`helldivers2_worldgen.md`](helldivers2_worldgen.md) §3), and now audio banks,
with the third being loadout:

**111 weapon banks and 48 stratagem banks is a streaming design, not a content
count.** **[inferred]** Loading all of them would be absurd; loading four
weapons and four stratagems per Helldiver, deduplicated across a squad of four,
is bounded and small. The bank boundary is drawn where the *player's choice*
is, which is the only place it can be drawn.

### 3.1 Fourteen walkable surfaces, each its own bank

```
foley_player_surface_media_common          _concrete_stone   _dirt
_flesh        _foliage      _glass       _grass      _gravel
_ice_snow     _metal        _mud_water   _rubber     _sand      _wood
```

**Separate banks, not switch values inside one bank.** **[inferred]** A switch
would mean every surface's samples are resident on every planet; a bank per
surface means a desert mission loads sand and rock and leaves ice, foliage and
wood on disc. That only works because the generator knows the biome's surface
set before it loads.

**`flesh` is a walkable surface**, which is a very Helldivers detail — the
ground of a bug breach genuinely becomes a different material to walk on.

This closes a loop across three notes. The generated terrain assigns a material
([`helldivers2_worldgen.md`](helldivers2_worldgen.md) §4, `generated_materials`);
`generated_surface_effect_settings.dl_bin` (304 KB) maps material to effect; the
player's animation graph carries a **`step_switcher`** and a **`terrain`**
variable ([`helldivers2_animation.md`](helldivers2_animation.md) §5.1); and the
audio side has a bank per surface. **Four systems, one shared surface
taxonomy, decided at terrain-generation time.**

### 3.2 Hazards are first-class, not weather decoration

Fifteen hazard banks, and the list is the design document:

```
haz_acidstorm     haz_bugcluster       haz_dustdevil    haz_explosivemushroom
haz_fire          haz_firetornado      haz_frozenflowers  haz_meteorstorm
haz_rift_spore_flower   haz_sandstorm   haz_snowstorm    haz_tornado
haz_toxchimney    haz_tremor           haz_volcanicactivity
```

**[inferred]** A hazard having its own bank puts it at the same granularity as a
weapon or an objective — it is a *placed content type* with an audio budget, not
a property of the weather system. That matches the `generated_weather_settings`
(214 KB) and `generated_weather_color_set_settings` pair being separate files:
weather is look, hazards are things.

### 3.3 Everything else the taxonomy gives away

* **Ten localised VO projects** — `us`, `jp`, `fr`, `de`, `es`, `it`, `ru`,
  `ms`, `bp`, plus the base project, as `wwise_metadata` resources. Full VO
  localisation, and `packages/localization/jp` is a separate content package.
* **`gore_bots` / `gore_bugs` / `gore_illuminate` / `gore_player`** — gore is a
  per-faction bank, i.e. dismemberment sound is authored per enemy family and
  loads with the faction.
* **`Mission_Civilian_Exertions` and `Mission_SEAF_Exertions`** — allied NPCs
  have their own exertion sets.
* **The `voice` bone.** Every creature rig carries a bone named `voice`
  ([`helldivers2_animation.md`](helldivers2_animation.md) §4). Emitter position
  is decided at rig time and travels with the animation, which is why a bug's
  scream comes from its head and not its origin.

---

## 4. What is worth taking

1. **Cut audio banks on the axis your content composer chooses along.** Biome,
   faction, objective, hazard, weapon, stratagem, surface. If the composer picks
   it, it should be a bank. §3.
2. **A bank per walkable surface, not a switch inside one bank.** Residency
   follows the biome. §3.1.
3. **One surface taxonomy shared by terrain generation, surface effects,
   animation and audio.** Decide it once, at generation time. §3.1.
4. **Two reverbs: convolution for signature spaces, FDN for everywhere else.**
   Plus half-precision convolution as the quality knob inside the expensive
   path. §1.
5. **If the world is generated, acoustics must be too.** Feeding geometry to a
   stochastic solver at runtime is the same forced move as runtime navmesh and
   runtime lightmaps. §2.
6. **Put the audio emitter in the rig.** A `voice` bone costs nothing and fixes
   positional audio on animated creatures for free. §3.3.
7. **Keeping audio middleware current is cheap and worth it even when the engine
   is not.** Wwise 2024.1.9 on a 2018-discontinued engine, because the audio
   plugin is a plugin. §1, and
   [`helldivers2.md`](helldivers2.md) §2 for why that was possible at all.
