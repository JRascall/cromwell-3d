# Helldivers 2 — a dead engine, three bought middlewares, and one plugin

Deep dive on **Helldivers 2** (Arrowhead Game Studios / Sony, 2024), read from the
retail install on this machine against a build current as of **2026-08-15**.

The received story is that Helldivers 2 is a triumph of stubbornness: a 2024
system-seller built on **Autodesk Stingray**, an engine discontinued in 2018,
by a studio that kept the licence after everyone else left. That story is true
and it is also the least interesting thing about the build. What the files
actually show is a **much sharper set of decisions than "we were stuck with it"**:

* **The game is a plugin.** `helldivers2.exe` is stock Stingray — its PDB path
  still says `stingray_win64_release.pdb`. The entire game ships as
  `data/game/game.dll`, which exports **exactly one symbol**, `get_plugin_api`.
  That is the ABI Bitsquid's own architect published in 2014. Arrowhead did not
  fork the engine; they wrote against its plugin API, which is why an engine
  nobody maintains was survivable for eight years. §2.
* **Everything hard was bought, and bought early.** Physics, navigation and
  cloth are **Havok**; audio is **Wwise**; UI is **NoesisGUI**; vegetation is
  **SpeedTree**; transport and voice are **PlayFab Party**; matchmaking is
  **PlayFab Multiplayer**. Arrowhead's technical director is on the record about
  the first of those. What Arrowhead wrote is the game, the renderer's content,
  and — the one genuinely bespoke engine-level system — **level generation**. §3.
* **There are five levels in the game.** Not five per faction. Five:
  `arrival_travel`, `departure_travel`, `empty_test`, `main_menu`, `ship_hub`.
  Every planet you have ever dropped onto is built at runtime by a Stingray
  plugin whose surviving source paths name its core algorithm as
  **`spherical_voronoi`**. This is the fact that organises the whole build, and
  most of the other odd decisions fall out of it. §4.

> **Sources, and their limits.** Everything tagged **[BUILD]** was read from
> `E:\SteamLibrary\steamapps\common\Helldivers 2` — PE import/export tables, PE
> string tables, the resource inventory read with
> [filediver](https://github.com/xypwn/filediver) v0.7.40, DXBC reflection
> chunks in the shipped shader libraries, and glTF exports of individual units.
> Nothing here comes from running, capturing or profiling the game, and nothing
> should: `bin/GameGuard` is nProtect anti-cheat, and this machine's standing
> rule is that anti-cheat titles get mined statically, never injected into.
>
> **The one caveat that governs every claim below.** The main executable's
> string table is **obfuscated** — its identifiers are seven-character tokens
> like `wucug19`, `ow2d10s` — and its import table is **packed**, exposing one
> anchor import per DLL instead of the real list. `game.dll` is the same. So the
> usual move of reading a shipped binary's symbol soup is closed here, and it is
> closed *deliberately*. Three channels survived, and this note is built
> entirely on them:
>
> | Channel | Why it survived | What it proves |
> |---|---|---|
> | **PE export tables** | The loader needs them by name | Which DLL is which product, and that game code is a plugin |
> | **DXBC reflection (RDEF)** | The D3D runtime needs cbuffer and resource names | The renderer's systems, pass by pass — §5 |
> | **Resource inventory** | The bundle index is not encrypted | What exists, what it is called, and — decisively — what *does not* exist |
>
> The third channel is the strongest and the least obvious. **An inventory
> proves absence**, and absence is what carries §4: you cannot argue a game is
> procedural from screenshots, but you can argue it from a complete list of its
> levels that has five entries in it.
>
> `data/game/*.dl_bin` — 58 files including a 45 MB `generated_entities`, plus
> `generated_damage_settings`, `generated_weather_settings`,
> `generated_planet_data` — are **encrypted**, and so is `dl_library.dl_typelib`
> beside them, which in the open-source
> [datalibrary](https://github.com/wc-duck/datalibrary) format would have
> described every schema. Their *filenames* are evidence; their contents are
> not available. Where a note wants a tuning value it cannot have, it says so.

Tags: **[BUILD]** read from the install. **[VENDOR]** the middleware supplier's
own product documentation for a product this build demonstrably links against.
**[ENGINE]** Bitsquid's published design notes for the engine this game runs on
— primary, but written 2009–2016 and about the engine, not the game.
**[PRESS]** journalism or a developer statement. **[inferred]** our reading.

Container formats — how to get at any of this — have their own note:
[`helldivers2_formats.md`](helldivers2_formats.md).

---

## The four notes

**Creature rendering has its own note.**
[`helldivers2_creatures.md`](helldivers2_creatures.md) answers how a Terminid is
drawn and taken apart. The headline is that **damage is not a mesh swap** — a
Charger's intact, damaged and gibbed body parts are 26 index ranges into
**one shared 193,494-vertex skinned buffer**, so blowing a leg off costs a
different draw range and nothing else. Bolted to that is a GPU **wound system**
(`clear_wounds` / `update_wounds` passes, object-space wound buffers at two
resolutions, a per-character queue and an indirection table) that accumulates
holes in a texture rather than in geometry — and, from a `regen_amount`
constant, heals them. A whole Charger is **three materials and four textures**.

**Animation and AI scaling have their own note.**
[`helldivers2_animation.md`](helldivers2_animation.md) answers how a hundred
agents are animated and drawn, and corrects two claims made elsewhere in this
folder. The mechanism is deliberately unexotic: **plain 4-influence GPU
linear-blend skinning** — no vertex or bone animation textures, and **creatures
never become billboards or impostors at any distance** — made to scale by
putting **every bone matrix in one shared `samplerBuffer` and passing a draw
nothing but an offset into it**. LODs are four levels at a uniform **~30%
decimation per step** across a 5× complexity range, so a Scavenger at LOD3 is
194 triangles. The animation *content* is where the real economy is: a Charger
is 121 states and **8** animation variables, the player is **2,318 states and
54**. And decoding the state machines recovers the AI's vocabulary, which
§5 of that note argues is a partial answer to a question this note's §5 calls
unanswerable.

**Navigation has its own note.**
[`helldivers2_navigation.md`](helldivers2_navigation.md) answers how a hundred
bugs cross a map that did not exist ninety seconds ago. The answer is
**Havok Navigation**, confirmed three ways, and the reason it had to be bought
is §4: **you cannot bake a navmesh for a level that is generated at runtime**,
so every feature Arrowhead needed — fast generation, runtime updates, silhouette
cutting, multi-radius, nav volumes for the flyers — is a feature of the product
they bought. The note also reads Bitsquid's own stated position on pathfinding
(*"A\* is overrated"*, 2010) and finds Arrowhead bought precisely the thing that
post says matters.

**World generation has its own note.**
[`helldivers2_worldgen.md`](helldivers2_worldgen.md) is the load-bearing one.
Five levels; a dedicated `level_generation_plugin` whose only recovered
algorithm name is `spherical_voronoi`; thirteen biome packages; one package per
*objective* rather than per map; **`terrain_editor_brush` shaders shipping in
the retail runtime** with `flatten`, `sub` and `box_filter` variants and a
`part_of_replay_stroke` uniform — the runtime rebuilds terrain by replaying
brush strokes. And because the level does not exist until you drop, the
**lightmap baker ships too**: `path_tracing:lightmap`, `bake_compute`,
`lightmap_edge_dilate`.

**Networking has its own note.**
[`helldivers2_networking.md`](helldivers2_networking.md) separates the three
tiers that get conflated every time this game's netcode is discussed. The
mission is a **peer mesh over PlayFab Party** with one player's machine
authoritative; lobbies and matchmaking are **PlayFab Multiplayer**, which
carries an owner-migration policy; the Galactic War is a **REST backend over
libcurl**. Under all of it sits Bitsquid's **object replication** model, whose
error strings survive in the shipped plugin foundation
(`ReliableReceiveBufferOverflow`, `PongTimeout`) — and whose author documented
its host-driven ownership migration, and a race condition in it, in 2013.

**Audio has its own note, and it is the biggest thing in the game.**
[`helldivers2_audio.md`](helldivers2_audio.md) covers the **31.6 GB — a quarter
of the install** — that the dependency table below gives one line to. It runs
**Wwise 2024.1.9**, upgraded from 2024.1.1 on a working drive called
`2024.1.9_UPGRADE`: the audio middleware is current to within months of this
build, on an engine dead since 2018, and §1 of that note explains why the plugin
architecture is what made that possible. **Wwise Spatial Audio is linked with
its entire geometric-acoustics surface** — `SetGeometry`, `QueryReflectionPaths`,
`SetNumberOfPrimaryRays`, `ResetStochasticEngine` — which, if used, makes audio
the fifth system forced out of the offline pipeline by §4. And the 427-bank
taxonomy is the level generator's vocabulary a third time: one bank per biome,
per hazard, per objective, per weapon, per stratagem, and **per walkable surface
— fourteen of them, including `flesh`.**

**Destruction has its own note.**
[`helldivers2_destruction.md`](helldivers2_destruction.md) is the third point
between this folder's two poles, [Siege](../rainbow_six_siege.md) (cuts geometry
at runtime) and [Bad Company 2](../bad_company_2_destruction.md) (never cuts
anything). Helldivers 2 lands beside Bad Company 2 but for a different reason,
and the asset names give it away: buildings are **named by footprint** —
`24x12`, `16x16`, `28x16` — each owning an intact version, a
`_destroyed_proxy`, a combined-rubble `_rc` and an `_rc_collapsed`. **A
generator places a slot, and every state that can occupy that slot must honour
the same footprint**, which rules out runtime cutting before any cost argument.

**Stratagems, armour and hit zones share a note.**
[`helldivers2_combat.md`](helldivers2_combat.md) — merged because the readable
evidence for each is thin and they are one loop. **Armour is bones**
(`shield1..6`, `backplate1..6`, and an Impaler `head_shield_gibs` you can shoot
off), hit zones are **typed collision primitives** (capsules on limbs, convex
hulls on heads and plates), and their count **scales with lifespan** — a Hunter
has one, a Hive Lord has forty-one. The stratagem side finds Eagle armaments
modelled as visible hangar hardware and the tactical hologram implemented as a
**world-position remap matrix** rather than a map asset, which on a map
generated ninety seconds ago is the only thing it could be.

**Particles already had their own note, and it is the deepest evidence in this
folder.** [`helldivers2_vfx.md`](helldivers2_vfx.md) was written from
*decompiled shader bytecode* rather than reflection names, so where the notes
below reconstruct systems from identifiers, that one reads the actual maths. Its
governing finding is a warning for everything here: grouping 1,450
particle-referenced materials by parameter schema gives **189 distinct shader
families**, and `c_billboard` is a *generated* struct whose layout differs
between two materials that both declare one — **there is no "the Helldivers 2
particle shader" to copy.** Its best transferable idea is a 128×1 colour LUT
indexed by **alpha²** — by how dense the sprite is at this pixel, not by
particle age — so one greyscale density sheet becomes a white-hot core with an
orange body and a smoky rim.

**Rendering has its own note.**
[`helldivers2_rendering.md`](helldivers2_rendering.md) reads the frame from
2,153 DXBC blobs' worth of reflection data: a five-target G-buffer, **clustered
shading with a local-light shadow atlas**, four-slice cascades, **checkerboard
rendering** (`raw_non_checkerboarded_target_size` — the PS5 inheritance, still
in the PC build), **VRS with a reprojected mask**, Nubis-class volumetric
clouds with a high-altitude second layer, a froxel fog volume with history, and
a full procedural starfield driven by `star_temperatures` and a
`star_generation_seed`.

---

## 1. What is actually on disk  [BUILD]

```
Helldivers 2/
  bin/            230 MB   engine exe, middleware DLLs, anti-cheat
    plugins/               level_generation_pluginw64_release.dll
                           wwise_pluginw64_release.dll
  data/            23 GB   30 x bundles.NN.nxa + 10 loose triples
    game/          70 MB   game.dll + 58 encrypted generated_*.dl_bin
  tools/           13 MB   GameGuard installer
```

The 23 GB of `.nxa` unpack to **127 GB** of Stingray bundles holding **26,514
resources** — the dedup ratio, and the container format, are
[`helldivers2_formats.md`](helldivers2_formats.md)'s subject.

By resource type, whole install:

| Type | Count | | Type | Count |
|---|---:|---|---|---:|
| `wwise_stream` | 16,666 | | `speedtree` | 224 |
| `physics` | 2,153 | | `xaml` | 181 |
| `unit` | 2,087 | | `shader_library` | 173 |
| `animation` | 1,147 | | `package` | 164 |
| `texture` | 1,060 | | `ragdoll_profile` | 75 |
| `material` | 714 | | `prefab` | 46 |
| `wwise_dep` / `wwise_bank` | 429 / 428 | | `ik_skeleton` | 12 |
| `bones` | 416 | | `lua` | 10 |
| `state_machine` | 372 | | **`level`** | **8** |

Four rows in that table are worth more than the rest put together.

**`level`: 8**, of which three are fallbacks and test scaffolding. Five real
levels, none of them a mission map. §4.

**`lua`: 10**, and all ten are engine boilerplate — `boot`, `debug`,
`core/animation/lua/runtime/animationflowcallbacks`, five Wwise callback
shims. This is the sharpest divergence from the engine's other well-known
tenant: Fatshark's Vermintide is a famously Lua-heavy Bitsquid game, and
Frykholm's blog spends whole posts on Lua binding costs **[ENGINE]**. Arrowhead
took the other road entirely — **the game is C++ in a plugin, and Lua is
vestigial**. Given how much of Helldivers 2 is per-frame simulation over
hundreds of agents, that is the defensible call, and it is also why nothing
useful is readable as script.

**`xaml`: 181** — NoesisGUI. Corroborated independently in the shader
reflection, where `c_noesis`, `noesis_projectionMtx`, `noesis_opacity` and
`noesis_radialGrad0/1` appear across 79 shader libraries. The UI is authored as
XAML and rendered by a bought vector-UI runtime, not by Stingray's own GUI.

**`havok_ai_properties`: 1, named `global`** — with
`havok_physics_properties: global` beside it. One global tuning resource each.
§3, and [`helldivers2_navigation.md`](helldivers2_navigation.md).

---

## 2. The game is a plugin, and that is the whole survival strategy  [BUILD] [ENGINE]

`bin/helldivers2.exe` carries this in its debug directory:

```
D:\Work\c0b269749c96c578\sr_bin_dir\engine\win64\release\stingray_win64_release.pdb
```

It is Stingray's own release executable, renamed. Now the export tables:

| Binary | Exports |
|---|---|
| `data/game/game.dll` (15.6 MB) | **1** — `get_plugin_api` |
| `bin/plugins/level_generation_pluginw64_release.dll` (542 KB) | **1** — `get_plugin_api` |
| `bin/plugins/wwise_pluginw64_release.dll` (6.1 MB) | 414 — the Wwise SDK's `AK::` surface |

`get_plugin_api` is not a name Arrowhead invented. It is the exact entry point
Niklas Frykholm specified when he published Bitsquid's plugin design in 2014
**[ENGINE]**:

> Instead of exposing individual functions *init()*, *update()*, etc, the plugin
> just exposes a single function *get_plugin_api()* which the engine can use in
> the same way to query APIs from the plugin.
> — *Building an Engine Plugin System*

The post's stated motivation is worth quoting too, because it reads as a
prophecy of Arrowhead's exact situation:

> Since you work directly in the source code, instead of against a published
> API, refactoring of engine systems might force you to rewrite your code from
> scratch.

**[inferred]** This is the answer to "how did they ship on an abandoned engine".
They didn't maintain a fork. The engine binary is upstream Stingray; the game
is a versioned, C-ABI plugin hanging off it, and so is the one engine-level
system they had to write themselves. When the CEO says the studio
"underinvested" in technology **[PRESS]**, the architecture is what let them
get away with it for as long as they did — and the packed, string-obfuscated
`game.dll` is where all the investment that *did* happen is hiding.

The plugin foundation is visible too: the level-generation DLL retained its
build paths, and they are Stingray's SDK layout with an Arrowhead plugin
dropped into it —

```
D:\Work\c0b269749c96c578\stingray\runtime\plugins\level_generation_plugin\plugin.cpp
D:\Work\c0b269749c96c578\stingray\runtime\plugins\level_generation_plugin\spherical_voronoi.cpp
D:\Work\c0b269749c96c578\stingray\runtime\sdk\plugin_foundation\array.inl
D:\Work\c0b269749c96c578\stingray\runtime\sdk\plugin_foundation\vector.inl
```

`runtime/plugins/` next to `runtime/sdk/plugin_foundation/` is stock Stingray.
`level_generation_plugin` is not. §4.

---

## 3. The bought stack, and where the seams are  [BUILD] [VENDOR]

Read off `bin/`, the plugin folder, and the resource inventory:

| System | Product | How it is evidenced |
|---|---|---|
| Physics | **Havok Physics** | `havok_physics_properties: global`; 2,153 `physics` resources; 75 `ragdoll_profile` |
| Navigation | **Havok Navigation** | `havok_ai_properties: global`; vendor lists the title |
| Cloth | **Havok Cloth** | 3 `cloth` resources — two Helldiver capes and a melee flag |
| Audio | **Wwise** | `wwise_pluginw64_release.dll`, 428 banks, 16,666 streams |
| UI | **NoesisGUI** | 181 `xaml`; `c_noesis` cbuffers in 79 shader libraries |
| Vegetation | **SpeedTree** | 224 `speedtree` resources |
| Transport + voice | **PlayFab Party** | `PartyWin.dll` — `PartyEndpointSendMessage`, `PartyNetworkCreateEndpoint` |
| Lobbies, matchmaking | **PlayFab Multiplayer** | `PlayFabMultiplayerWin.dll` — `PFLobby*`, `PFMatchmaking*` |
| Backend HTTP | **libcurl** | `libcurl.dll`, plus WinHTTP/WININET |
| Scripting VM | **LuaJIT/Lua 5.1** | `lua51.dll` — present, barely used (§1) |
| Video | **Bink 2** | `bink2w64.dll` |
| Streaming I/O | **DirectStorage** | `dstorage.dll`, `dstoragecore.dll` |
| Upscaling | **DLSS + XeSS + FSR** | `nvngx_dlss.dll`, `libxess.dll`, `amd_fidelityfx_upscaler_dx12.dll` |
| Crash reporting | **CRS** | `crs-client.dll`, `crs-handler.exe` |
| Anti-cheat | **nProtect GameGuard** | `bin/GameGuard/`, `GameGuard.des` |

Arrowhead's technical director on the first three **[VENDOR]**, from Havok's
own customer page:

> "We knew early on that HELLDIVERS 2 needed great physics and navigation, and
> Havok was the obvious choice for us. Besides their solid products, what
> really pleased us was their amazing support staff that helped us through
> thick and thin."
> — Peter Lindgren, Technical Director, Arrowhead Studios

Note *"knew early on"*. **[inferred]** Given §4, they had to: the decision to
generate levels at runtime and the decision to buy navigation are the same
decision, taken once, and everything downstream follows from it.

**The seam worth noticing is where the buying stops.** Rendering, level
generation and the game simulation are Arrowhead's. Every system where an
off-the-shelf answer existed was bought; the two systems where the game's
identity lives — how a planet is assembled, and how a bug comes apart — were
written. That is a cleaner split than most studios manage, and it is the same
finding as [`broken_arrow.md`](../../flight/broken_arrow/broken_arrow.md) §4
from a completely different direction: Steel Balalaika bought the renderer and
wrote the simulation, Arrowhead wrote the renderer *and* the simulation but
bought everything with a solved-problem shape. The consistent rule across both
builds is not "buy engines" or "write engines" — it is **buy the systems whose
correctness is a research problem and whose behaviour is not your product**.

### 3.1 D3D12, and shader bytecode that says so

Three independent signals put the renderer on **D3D12**: `dstorage.dll` (a
D3D12-only API), `amd_fidelityfx_upscaler_dx12.dll`, and the `vrs_*` uniform
block in the shader reflection — variable-rate shading is a D3D12 feature. The
shipped shader libraries hold **DXBC**, not DXIL, which is legal on D3D12 via
shader model 5.1 and is what you would expect from a codebase whose shader
pipeline predates DXC by years. `WinPixEventRuntime.dll` ships too, so the
retail build is still emitting PIX markers.

---

## 4. Why this build is shaped the way it is

Every unusual thing in the preceding sections resolves to one decision, and it
is worth stating in one place before the topic notes take it apart.

**Helldivers 2 has no mission levels.** It has an assembler. The consequences
propagate:

* **Navigation cannot be baked** → buy Havok Navigation, whose selling points
  are runtime navmesh generation and runtime updates.
  [`helldivers2_navigation.md`](helldivers2_navigation.md)
* **Lighting cannot be baked offline** → ship the baker. `path_tracing:lightmap`,
  `bake_compute`, `lightmap_edge_dilate`, `c_ship_hub_probe_bake` are all
  retail passes. [`helldivers2_rendering.md`](helldivers2_rendering.md) §7
* **Impostors cannot be authored per-map** → ship the impostor baker too:
  `imp_bake`, `imp_clear`, `imp_weight_merge`, `imp_material_count`.
* **Streaming cannot be per-level** → make the streaming unit the *objective*.
  164 packages, and they are named `cy_destroy_airbase`,
  `bug_destroy_stalker_lair`, `gen_evacuate_civilians`. The composer loads what
  it placed. [`helldivers2_worldgen.md`](helldivers2_worldgen.md) §3
* **Terrain cannot be a shipped heightmap** → ship the terrain editor's brush
  shaders and replay strokes at runtime. `terrain_editor_brush:flatten`,
  `part_of_replay_stroke`.
* **Creature variety cannot come from unique authored assets** at the count
  needed → three materials per creature, shared tiler arrays, and all
  per-instance variation from GPU buffers.
  [`helldivers2_creatures.md`](helldivers2_creatures.md)

**[inferred]** The generalisable lesson, and the one worth carrying into
`cromwell`: *deciding to generate the world at runtime is not a content
decision, it is an engine decision*, and it converts an entire column of the
build pipeline — navmesh, lightmaps, impostors, streaming manifests — from
offline tools into shipped runtime code. Arrowhead paid that bill in full.
It is the same shape as this project's own rule that a derived cache needs an
escape hatch: once you generate the authoritative thing at runtime, everything
downstream of it has to be generated at runtime too, and there is no partial
version of the commitment.

---

## 5. What this build cannot tell us

Stated plainly so the topic notes do not have to keep hedging.

* **Enemy decision *policy*.** Whatever selects a bug's target, decides to call
  a breach, or paces a patrol is compiled into `game.dll`, whose strings are
  obfuscated and whose imports are packed. There is no behaviour-tree or
  blackboard resource type in the inventory.
  **[Amended]** This originally read "what it *decides* is not readable here",
  which was too strong. The `state_machine` resources are *animation* state
  machines, but they are the interface the AI drives, so decoding them recovers
  the AI's **vocabulary** — tasks, awareness ladders, `Alert_Leader`,
  `taunt_unreachable`. See
  [`helldivers2_animation.md`](helldivers2_animation.md) §5.2. What stays shut
  is the *policy*: no scoring, no thresholds, no timings.
* **All tuning numbers.** `generated_damage_settings`,
  `generated_weather_settings`, `generated_planet_data`,
  `generated_generation_settings`, `generated_stamp_settings` — every one is
  encrypted. Their names are strong evidence for the *existence* and *shape* of
  a system; none of them yields a value.
* **The Galactic War server.** Only the client half is on disk. What the
  backend does with a mission result — and how the "Game Master" steers the war
  — is outside anything an install can show.
* **Frame cost.** No capture was taken and none should be. Where this study
  says a pass is expensive, that is **[inferred]** from what it reads and
  writes, never measured.

---

## 6. Reproducing any of this

```bash
# resource inventory — the channel most of this note rests on
filediver --gamedir "<install>" -i "*" -lf "%T\t%N"

# PE exports: proves the plugin architecture in one line
python imports.py "<install>/data/game/game.dll"

# shader reflection vocabulary: DXBC keeps RDEF names in the clear
python shadervocab.py <extracted>/shaders vocab.txt
```

Two evidence files sit beside this note:

* [`helldivers2_inventory.txt`](helldivers2_inventory.txt) — the type histogram,
  every named structural resource, and the full faction unit roster.
* [`helldivers2_shader_vocab.txt`](helldivers2_shader_vocab.txt) — 1,596
  identifiers recovered from 813 shader libraries, ranked by how many libraries
  mention each. This is the raw material for
  [`helldivers2_rendering.md`](helldivers2_rendering.md); reading it top to
  bottom is a fast tour of a renderer.

**Version drift is real and worth budgeting for.** The shader corpus here was
pulled 2026-08-09; the install updated 2026-08-13; the inventory was read
2026-08-15, and counts differ between the two dates. The patched filediver from
the earlier session **fails outright** on the newer build — `expected final
bytes read to be 0xDEADBEE7` while parsing `unit_customization_settings` — and
v0.7.40 was needed. Pin the tool version next to the date whenever a number
from this study gets quoted.
