# Mercenaries: Playground of Destruction (PS2, 2004) — extraction

Pandemic Studios / LucasArts, running on **RedEngine**. The disc names it
outright: `SLUS_209.32` contains paths like `../../RedEngine/Source/`.

Everything here reads a retail disc image and writes standard formats. Nothing
is committed — output goes to `mercs_extracted/`, which is gitignored, on the
same terms as the XCOM, Siege, UNIGINE and SimCity sweeps.

**Why a README when no other `tools/<game>/` has one.** The convention in this
repo is that a tool explains itself in its own header, and each of these still
does. But this is twelve tools with real ordering dependencies between them, and
a handful of findings that cost hours to establish and are invisible from any
single file. That belongs in one place.

---

## Order matters

```
1.  mercs_extract.ps1 -Iso <path.iso>     rips the disc, converts audio,
                                          copies text tables, dumps names
2.  mercs_tex.py      → textures_png/     run BEFORE meshes and scenes:
                                          their .mtl files point at these
3.  mercs_mesh.py     → models_obj/       also writes materials.csv
4.  mercs_terrain.py  → terrain/
    mercs_world.py    → worlds/
    mercs_script.py   → scripts/
    mercs_anim.py     → animations/       independent of the others
    mercs_fx.py       → audio_events/     independent; needs no other step
    mercs_gameplay.py → gameplay/         paths, encounters, anim tables,
                                          regions, landmarks
    mercs_ini.py      → config/           the 40 shipped text configs; takes
                                          the DATAPS2 directory, not a .DSK
5.  mercs_gltf.py     → gltf/             needs 2; rig + skin + clips in one
    mercs_rigcheck.py                     after a Blender round trip; gates it
    mercs_scene.py    → scenes/           needs 2 and 3
6.  mercs_elf.py      → executable/       VU microcode, sections, source map
```

`mercs_lzss.py` is a library, not a script — the decompressor everything else
imports.

Audio needs `vgmstream-cli`; `mercs_extract.ps1 -FetchTools` downloads it.

---

## What comes out

| | |
|---|---|
| `models_obj/` | 2,950 OBJ + MTL, 1.85M triangles, `usemtl` per segment |
| `textures_png/` | 4,643 PNG |
| `audio/` | 1.84 GB WAV — 168 streams, 5,388 bank samples |
| `animations/` | 1,835 `.anim`, 172 MB, 130 minutes of motion, 1,735 events |
| `gltf/` | 2,293 rigged `.glb` — skeleton, skin and materials |
| `gltf/animated/` | the three playable mercs (`*_anim.glb`), each carrying 1,639 clips |
| `terrain/` | 14 heightmaps: PNG, RAW, OBJ, JSON metadata |
| `worlds/` | 413 CSV, 51,731 placements, **652 property columns** — see below |
| `gameplay/` | 350 paths (3,133 oriented nodes), 236 encounters, 95 animation tables, 96 landmarks, 12 region sets |
| `config/` | 20,260 directives from 40 text configs — atmosphere/lighting, 1,713 radio lines, 299 traffic zones, 229 emails |
| `scripts/` | 198 Lua files, 2.03 MB — **genuine source, not bytecode** |
| `executable/` | VU microcode, section table, 104 source filenames |
| `audio_events/` | 396 audio events, 10,479 sound cues, the whole music transition graph |
| `asset_names.tsv` | 9,494 named assets |
| `materials.csv` | 2,950 mesh → texture links |

---

## Formats

| what | where it was read from | notes |
|---|---|---|
| `.DSK` archive | — | `{u32 count; u32 reserved}` then `{u32 size; u32 nameHash; u32 groupHash}`. Group is the asset TYPE, not a streaming bucket |
| LZ (meshes, scripts) | `0x003A6358` | 16-bit LSB flag word. `1`=literal, `00`=short match, `01`=long match |
| texture pixel RLE | `0x0036C0B0` | `b & 0x80` → run of `b-127`, else `b+1` literals |
| palette RLE | `0x0036BA30` | same codec, unit is a 4-byte ENTRY not a byte |
| palette entry | — | **A,B,G,R**; alpha is full 0..255, not PS2's 0..128 |
| `segm` vertices | swbf-unmunge | POSI = **unsigned** u16 → model bbox; NORM = i8/127; TEX0 = i16/2048 |
| `anim` container | `0x00399098` | Zephyr. INFO `{u16 frames; u16 joints; f32 1/staticScale; u16 events; u32 packed; u32 unpacked}`; CJNT is the mesh LZ |
| `jont` curves | `0x003999b8` | QROT = four channel byte-counts; DXLT = per-joint f32 scale + `frames*6`; SXLT = 3×s16. **No offsets — a running cursor in chunk order** |
| rotation channel | `0x00399c88` | `0x80 n` hold, `0x81 vv` absolute s16, else s8 delta; component = value / **2047** |
| `tern` heightmap | — | 512×512 @ 8 m. HGT8 tiled **16×16**; HEXP is **signed** i16 |
| `tern` PTCH | `RedTerrain.cpp` | 4 bytes per 8×8 patch: `{u16 layerMask; u8 decalCount; u8 reserved}` |
| `tern` COLR/ALPH | `RedTerrain.cpp` | per-patch blocks in patch order, `{u16 packed; u16 unpacked}` + **mesh LZ**, not the texture RLE. COLR is 162 B = 81 vertices (9×9, shared edges) × u16. ALPH is 45 B × popcount, and only for patches with more than one layer |
| `tern` DCAL | — | 24-byte records grouped by patch, run length from PTCH |
| `wrld` placement | — | `inst` = XFRM (3×3 + position) + PROP; TABL is `{hash,len,text,0}` |
| `.MSH`/`.MSB` audio | — | `{u32 size; u32 id; u32 offset; u32 rate}` per sample |
| `fx3_`/`xcl_`/`xch_`/`xsh_`/`xsl_` | — | **not a binary format.** A `ucfb` tree whose leaves are null-separated ASCII `key\0value\0…`; a float is the text `"0.400000"`. `SSET`→`SNDS` are weighted alternatives, `TTBL`→`TRNS` the music transition rules |

`.MIB`/`.MIH` are stock Sony MultiStream — vgmstream reads them unaided.

---

## Things that cost hours. Read this part.

**A tool that reads a field and does not emit it hides the whole chunk.**
`mercs_world.py` resolved every instance property through the world's string
table and then wrote four of them — model, name, objectType, transform. So the
world CSVs looked like a placement list, and the conclusion drawn from them was
that the game shipped no lighting data, no destruction tuning and no cover
data. All three were in the file the whole time, one function call away:

    light_a_rgba  light_a_omniradius  light_a_conelength  light_a_conewidth
    light_a_flaresize  light_a_flareamount  light_a_flickertype/period
    destructible  hitPoints  damageTexture  physicsMass  ArmorType
    coverRating  ObstacleMaterialType  targetPriority  winchable  faction

1,703 instances carry a light block; 2,919 carry hit points; 1,419 carry a
cover rating. Emitting the union of resolved keys turns 15 columns into **652**.

The general lesson, and it is not about lighting: an extractor's output is the
only thing anyone reads, so **a field that is parsed but not written is
indistinguishable from a field that is not there**. Absence in the output was
taken as absence in the data, and that is a conclusion no amount of staring at
the CSV could have corrected. When a chunk has open-ended properties, emit all
of them and let the reader narrow.

**Check for an existing reader before reverse-engineering.** Four rounds of
parameter sweeping failed on the `segm` vertex format. `swbf-unmunge` — a mature
reader for the same Pandemic `ucfb` container — settled it in minutes. Positions
are *unsigned*, and reading unsigned data as signed wraps everything above 32767
into large negatives, tearing meshes apart along no consistent axis. No amount
of later scaling recovers that.

**Self-invented metrics only rank hypotheses you already had.** None of the
scoring functions could ever surface "the integers are unsigned", because that
possibility was never in the scored set. One metric was also read backwards:
"all segments come out the same size" was scored as good when it *was* the bug.
Prefer tests that cannot be read backwards — does the decoder consume its input
*exactly*, is the height field continuous across patch boundaries, does the
output parse as a well-formed chunk tree.

**Deduplicate on content, never on the name hash.** A model and its geometry
share a name hash and differ only by group. Deduplicating on the name hash
silently discards every `CSEG` in the archive, which made the geometry look
absent entirely on the first pass.

**Partial success is more misleading than total failure.** The palette reader
was right for 16-entry tables by accident — their single RLE control byte is
indistinguishable from a 1-byte header. Only 256-entry tables, which need two
control bytes with the second sitting mid-data, exposed it. 4bpp textures looked
perfect while 8bpp came out speckled.

**The decompiled ELF pays for itself on a compressed format.** The animation
curve codec — three opcodes, a 1/2047 quantiser and an implicit cursor with no
offsets anywhere in the file — would have been a long guessing game from the
bytes. `ZephyrAnim.cpp` gave all of it in an afternoon, and every field was then
checked against something that could have contradicted it: the cursor lands
exactly on the unpacked size for all 1,835 clips, decoded quaternions are unit
length to within the quantiser, and the translations agree with joint positions
in the `modl` chunks the decoder never reads. Guessing gets you a format that
looks right; a second source gets you one that is.

**Check a channel order against a texture whose colour you know.** Palette
entries are **A,B,G,R** with alpha over the full 0..255 range. They were read as
A,R,G,B with PS2's 0..128 alpha for a while, and "confirmed by eye" on asphalt,
concrete and metal — all grey, and grey survives a red/blue swap untouched. The
characters did not: skin came out corpse-blue. A Chinese flag that decodes to
navy settles it in one look; a thousand grey walls never will.

**`segm` INFO's third field is primitive topology, not a scale exponent.**
Values 1–15 look like an exponent and are not. Segments at different values use
different textures and do not form a detail pyramid.

**There are no LOD meshes.** The game culls by distance instead — every `modl`
carries thresholds in its INFO (80/180/1000 m on 412 models, −1 = never on 902).

**Generic MIPS cannot decompile this binary.** Stock Ghidra produced 7,967
`halt_baddata` markers across 8,249 functions — roughly one per function, each
truncating the body. With the R5900 processor extension
(`chaoticgd/ghidra-emotionengine-reloaded`, exact build for Ghidra 11.2 is
v2.1.24) that drops to **0**, functions grow from 236 to 645 median bytes, and
string references resolve.

**The asset browser caches its catalogue.** Anything extracted after the last
scan is invisible until `--list --refresh`.

---

## Not decoded

`path` (346 — road network: PNTS/ORNT/JNCT junctions), `rgns` (10), `enc_`
(232 encounters), `atbl` (95), `lrg_` (91). All extracted as raw chunks by
`mercs_dsk.py --extract`. Every one of them is the same self-describing `ucfb`
tree the audio tables turned out to be — `enc_` opens `NAME "Encounter0"` then
`SQDS "SQD_…"`, `atbl` opens `NAME "allies_boss"` then `ANAM` — so these are
walking exercises rather than reverse engineering. Do them before anything that
needs a real codec.

**Visual particle effects do not exist as data anywhere on the disc.** This is
a proven negative rather than a gap, and it was worth proving because the
alternative is looking for them forever:

- `RedEffect.cpp`, `RedEffectGenerator.cpp`, `RedEffectSystem.cpp` and
  `RsEffect.cpp` contain **no chunk-tag constants** — nothing is loaded
- the ELF string table contains **zero** occurrences of `particle` or
  `emitter`, so there are no parameter names even in the binary
- no shipped config file defines one: `RS.INI`, `PS2.INI`, `MSH2TEMP.INI` and
  `RSM.CFG` are language/debug flags, mesh→template mappings and the audio
  stream manifest respectively
- the Lua exposes three high-level toggles and no definitions —
  `Effect_SetCloudFlightEffect`, `Effect_SetRushingAirEffect`,
  `Effect_CeilingDust`

What does exist: sprite textures (`explosion`, `flame`, `flame_anim`, `flames`,
`csmoke`, `fx_b_dust_puff`), ~300-byte billboard models (`global_flame01`,
`global_flamesmall01`, `global_flametiny`), and `RsEffect.cpp` at 98 KB to read
the behaviour out of. Emitters are hard-coded C++. Re-authoring is the only
route, and no tool in this directory will ever produce them.

Note the name collision that cost an afternoon: `fx3_` reads like the particle
chunk and is the **audio** event table.

Two useful things found while proving this. `MSH2TEMP.INI` maps meshes to actor
templates (`allies_veh_apache` → `template_allies_apache`), which is the other
half of the `template_*` names the mission Lua spawns. `RSM.CFG` is the stream
manifest — name, path and sample rate for every streamed sound.

The **other three bits of an ALPH nibble**. The plane layout is solved from the
console loop — 9×9 vertices, one nibble each, five bytes a row — and the loop
consumes exactly one bit of it (`0x04` low, `0x40` high) to select the layer.
What the remaining three bits do is not visible in that path.

The **DCAL record fields**. 24 bytes, grouped by patch, and the loader's own
shape is known: eight header bytes of which five are discarded, then eight s16
as four alternating-axis pairs in 1/16 units. Byte 3 is zero and bytes 4–7 are
`0xFFFFFFFF` on every record in the game. But the file→runtime byte mapping
does not reproduce the runtime nibbles, and reading the s16 as world position
puts a decal in its own patch 0.3% of the time. Emitted as hex against the
owning patch.

Skinning is **rigid — one bone per vertex, no weights**, which is a PS2-era
constraint and not something lost in extraction. The `.glb` writes weight 1.0
rather than inventing a smooth falloff.

### Editing the rig without losing the animations

The rig has no IK chains, no twist bones and no helper bones — 36 joints, plain
FK. Adding them in Blender is the obvious way to study IK on this character set,
and it works, because **a clip binds by joint name and sets local TRS outright**:

| Edit | Safe? | Why |
|---|---|---|
| Re-weighting the mesh | **yes** | no clip refers to a vertex weight |
| Adding joints | **yes** | a joint no clip names keeps its rest pose — which is what a twist bone wants, since it is driven in the constraint layer |
| Moving a joint's rest pose | **yes** | a clip's local TRS is absolute, not an offset from rest |
| Renaming / reparenting / deleting one | **no** | the name *is* the binding, and the parent decides what a local transform means |

Never edit the generated `.glb` in place — export, copy, edit the copy, or the
next `mercs_gltf.py` run destroys the work.

`mercs_rigcheck.py <original.glb> <edited.glb> --anims animations/` gates the
round trip and exits non-zero on a break. Two things it does that a joint-count
diff does not:

**It counts lost TRACKS, not lost clips.** Whether a clip still "binds" is far
too coarse — losing 1 joint of 36 still clears the 75% threshold, so the clip
count sits at 1,639 either way while every walk quietly stops driving a knee.
Renaming `bone_l_calf` the way Blender renames a duplicate costs **1,337 clips
one track each**, and each of them still plays. A limb going stiff is not an
error anyone reports as one.

**It counts influences per vertex, not `JOINTS_` sets.** A mesh with four real
influences and one rigidly bound to a single bone both have exactly one
`JOINTS_0`; an earlier version of the check reported "17 sets" for both and
looked healthy. Reading the weights says 1.00 before and 2.00 after, which is
the only number that shows the re-weighting happened.

**Every human shares one skeleton.** 1,639 of the 1,835 clips bind to the merc
rig by joint name, which is why one character's `.glb` carries the whole human
library. The rest are vehicles, cameras and doors.

**Vehicles are rigged and almost never animated by a clip.** Matching all 1,835
clips against all 2,297 skeletons by joint name: of 272 vehicle models, 95 are
rigged, and **3** have a clip that fits — the towed artillery, whose single clip
is `nk_veh_type66artillery_fire`, a recoil. An Apache has 37 joints (rotors,
gear, doors) and no clip at all; the `*_veh_*` animation names are the
*character* climbing in. Vehicle parts are driven procedurally, so a rig with no
animation is the correct result rather than a gap in the extraction.

**It is not only characters, though.** 154 models can play at least one clip:
flags and windsocks (`global_flagallies_anim`), guard-post gates
(`dmz_bld_skpost_leftopen_anim`), chairs, bikes, trees, HQ interiors, the C-17
set pieces and `global_animated_camera`. 54 clips fit no model in the archive —
mostly gates and doors, whose geometry the world chunks place rather than
shipping as a standalone `modl`.

**One human skeleton, 88 models.** Every human — 31 North Korean, 14 allies, 11
mafia, 9 Chinese, 8 SK, 8 prokat, 7 civilian — shares the rig the playable mercs
use, so any of them can play any of the 1,639 human clips. That is why the
`.glb` exports attach clips to three characters and not to all 88: the curves
are identical, and duplicating them per model would be ~10 GB.

---

## Decompilation

```
E:\Tools\ghidra\ghidra_11.2_PUBLIC        + emotionengine-reloaded v2.1.24
analyzeHeadless <proj> <name> -import SLUS_209.32 \
    -processor "r5900:LE:32:default" \
    -scriptPath tools/mercs/ghidra -postScript mercs_label_and_export.py
```

10,502 functions, 15.1 MB of pseudo-C. The post-script attributes functions to
their original source file via the assert strings they reference — an assert
lives inside the function it guards. That names **366 functions across 93
modules** (`ps2RedTerrain.cpp`, `RsTrafficManager.cpp`, `RsAi.cpp`,
`RsHavokObstacleManager.cpp`). The other 10,136 never mention their own
filename and stay anonymous in `_unattributed.c`. ~3.5% attribution is the
ceiling of the technique, not a bug.

It is reconstructed pseudo-C: no names, types, comments or class structure.
Good for reading how a system works — the LZ decompressor decompiles into
something matching the hand-derived version mechanism for mechanism, which is a
useful way to check a format guess against real code. The Lua scripts are the
only genuine source on the disc.
