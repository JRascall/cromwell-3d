# Broken Arrow

Extraction for the studies. Unity 2022.3.62f3, IL2CPP, EasyAntiCheat, installed
at `C:\Program Files (x86)\Steam\steamapps\common\broken_arrow` — note **C:, not
the E: SteamLibrary**, and six directories deep, so a shallow sweep of the drive
roots misses it.

```
.\tools\ba\ba_extract.ps1 -List                       # inventory, extracts nothing
.\tools\ba\ba_extract.ps1 -Out D:\ba_extracted        # everything but video and granite
.\tools\ba\ba_extract.ps1 -Kind model -Filter units   # the units, rigged, with animation
.\tools\ba\ba_extract.ps1 -Kind audio                 # FMOD banks -> wav
.\tools\ba\ba_extract.ps1 -Kind granite               # the 27 GB megatexture
```

Nothing extracted here is committed or shipped. Same rule as XCOM, Siege,
UNIGINE, SimCity, Mercenaries and World in Conflict: read for reference,
reimplemented, never redistributed. `ba_extracted/` is gitignored.

**Give it a drive with room.** The install is 40 GB and the decoded output is
larger, so the real library lives on `D:` and the repo-root default exists only
so a run without `-Out` lands somewhere predictable.

## Four storage schemes, four tools

| What | Where | Size | Read with |
|---|---|---|---|
| Addressable bundles | `StreamingAssets/aa/PC/*.bundle` | 9.9 GB / 77 files | AssetStudioModCLI |
| Player data | `BrokenArrow_Data/data.unity3d` | 6.9 GB | AssetStudioModCLI |
| FMOD Studio banks | `StreamingAssets/*.bank` | ~3 GB / 30 files | `ba_bank.py` (see below) |
| Granite virtual textures | `StreamingAssets/*.gts` + `*.gtp` | 27 GB | GraniteTextureReader |

Tools are fetched on first use into `<out>/_tools` and pinned — AssetStudioMod
v0.19.0, vgmstream r2117, GraniteTextureReader 1.1.5. Nothing is vendored.

`units_assets_all_*.bundle` is 3.2 GB and holds **4,443 meshes, 2,708 textures
and 427 Animators**. It is most of what anyone wants, and it is one file, which
is why the sweep resumes per bundle: AssetStudio loads the whole thing before it
writes anything, so a run that restarts from nothing never finishes.

**AssetStudioMod, not AssetRipper.** AssetRipper is the better tool for
reconstructing a whole Unity project — materials, prefabs, shaders — and 1.3.14
handles 2022.3 fine. Its free build ships only a GUI, so it cannot be scripted.
Use it by hand when the question is "how was this material set up"; use this
script when the question is "give me the meshes".

**IL2CPP means empty method bodies.** `-Kind data` gets the ScriptableObject
layer — unit stats, weapon tables — because the bundles carry type trees. It
does not get behaviour. For the C# *shape* (namespaces, fields, offsets, and the
developers' own `[Header]`/`[Tooltip]` prose, which is the valuable part), run
Il2CppDumper over `GameAssembly.dll` + `il2cpp_data/Metadata/global-metadata.dat`
— **v6.7.46 or later**, because the metadata is v31 and older builds refuse it.
Every method body is still empty; anything said about control flow is inference.

## The audio, which is the part that needed work

Every one of the 30 banks is **encrypted**, which is why no off-the-shelf tool
opens them. vgmstream gets as far as `couldn't load bank 0 at 1ee80
(encrypted?)` and stops, and that diagnosis is correct.

Only the FSB5 sample archive inside the `SND ` chunk is enciphered. The RIFF
skeleton, the bus layout, the event and sample names are all plaintext. The
cipher is the FSB scheme vgmstream implements — `plain[i] =
reverse_bits(cipher[i]) ^ key[i % len]`, indexed from the first byte of the
FSB5 — not AES, whatever the internet says about FMOD banks.

The key is **`jU5n9Ce2ng5T`**, 12 bytes. It was recovered from the ciphertext,
not from the binary: an FSB5 starts with the magic `FSB5` and a version field of
1, which is eight bytes of known plaintext and therefore eight bytes of key, and
the run of zeroed header fields makes the ciphertext repeat with period 12,
which gives the length and the last four bytes. It is *confirmed* rather than
assumed — the decrypted header's four size fields add up, with the 0x3C header,
to exactly the bytes remaining in the chunk. Reading it out of
`global-metadata.dat` would have meant picking one string literal out of tens of
thousands with no cross-reference to say which, because IL2CPP compiles the
method that uses it to native code.

`ba_bank.py` explains all of this at length in its header, including the trap
that cost the most time: the FSB is placed at a **32-byte-aligned file offset**,
so the plaintext padding before it is a different length in every bank — 10
bytes, 16, 22, 26. A search that steps four bytes at a time finds half the banks
and reports "no FSB5" for the rest, which reads as a wrong key and sends you
back to the binary for nothing.

What comes out: **13,735 Vorbis samples at 44.1 kHz** from 28 banks, each
carrying the sound designer's own name from the FSB name table —
`UI_CUSTOMIZE_AIR_Layer-001`, `BA_Ambience_Birds_Forest_1` — so the output is
browsable rather than 13,735 files called `1.wav`. The two banks with no audio,
`CustomDialog.bank` and `Master.strings.bank`, are metadata only and are
reported as such rather than as failures.

## What a full sweep actually produces

Measured, `-Kind all` over the whole install:

| kind | files | size |
|---|---:|---:|
| audio | 13,735 | 26.9 GB |
| texture | 15,297 | 15.0 GB |
| mesh | 11,056 | 8.8 GB |
| model | 3,112 | 4.7 GB |
| data | 70,886 | 0.1 GB |

701 FBX, each sitting beside its own textures in `BaseMap` / `MaskMap` /
`Normal` sets — the URP/HDRP mask-map convention rather than separate
metal/rough/AO maps. The meshes, skeletons and skinning in these are sound.
**The animation in them is not** — see below.

## Animation: why AssetStudio is not enough, and what to run instead

AssetStudio's `-m animator` writes FBX with correctly *named* takes and no
curves in them. Only **22 of 701** carry real animation, and they are the
vehicles — Patriot launchers erecting, mortar hatches, artillery — plus one
infantry family. Every rifleman comes out with 21 empty takes called
`Kneel_aim`, `Prone_aim`, `Stand_run` and so on.

The reason is the clip format. Broken Arrow's 475 clips are Mecanim clips with
`m_Legacy: 0`: the legacy arrays AssetStudio reads (`m_RotationCurves`,
`m_PositionCurves`, `m_EulerCurves`) are all empty, and the data lives in
`m_MuscleClip`. AssetStudio reads the AnimatorController for the state names,
finds nothing in the fields it knows, and writes the names anyway.

**AssetRipper decodes them.** Its free build looks GUI-only but is a local web
server: `--headless --port N` plus its own OpenAPI (`POST /LoadFile`,
`POST /Export/UnityProject`) makes it fully scriptable. Its "Reconstruct
AnimatorController Assets" pass is the step AssetStudio lacks. Out come 472
`.anim` files — and they split two ways:

- **389 generic clips.** Transform curves bound to bone paths. Usable as-is.
- **83 humanoid clips.** Curves of Unity *muscle* values — `Chest Front-Back`,
  `Left Arm Down-Up`, `Left Forearm Stretch` — plus `RootT`/`RootQ` and IK
  goals, with an empty binding path and `classID: 95`. Muscle values are not
  bone rotations.

Those 83 are the infantry, and they are referenced **4,298 times** across 221
humanoid rigs: one shared clip library, one `Kneel_reload` reused by every
rifleman. That is why so few broken clips wrecked so much of the roster, and it
is why the conversion has to run per *rig* — the same muscle curves land
differently on each Avatar's skeleton, which is the entire point of a humanoid
clip.

Converting muscle space to bone space is Unity's retargeting: it needs the
Avatar's T-pose and per-muscle limits. Rather than reimplement that, `unity/BaBake.cs`
runs it in the engine that owns it — sample each clip onto its real rig through
`AnimationMode.SampleAnimationClip`, read the bone transforms back out, write
them to a compact binary `.batrk` per clip. The 15 Avatars AssetRipper exports
alongside are what makes this possible.

```
# AssetRipper, headless, scripted
_tools/AssetRipper/AssetRipper.GUI.Free.exe --headless --port 17654
curl -X POST localhost:17654/LoadFile          --data-urlencode "path=<bundle>"
curl -X POST localhost:17654/Export/UnityProject --data-urlencode "path=<out>"

# Unity, headless. No install needed - Unity 6 opens the 2022.3 project fine,
# and a bake touches neither the game scripts nor the shaders that an upgrade
# would break. Copy only AnimationClip/ Avatar/ AnimatorController/ GameObject/
# Models/ Resources_moved/ into a fresh project: ~210 MB rather than the 12 GB
# export, because the skeleton lives in the prefabs and meshes are dead weight.
Unity.exe -batchmode -quit -nographics -projectPath <proj> \
          -executeMethod BaBake.Bake -baOut D:/ba_extracted/anim
```

### Two traps, both of which produce clean-looking output

Neither of these throws, and neither leaves a mark in a log. They are recorded
because the only thing that caught them was checking whether the numbers move.

1. **Sample the Animator's GameObject, not the prefab root.** A clip's bindings
   are relative to the object the Animator sits on, and these prefabs wrap the
   rig in an outer transform. Sampling the wrapper animates nothing — and gives
   you a complete file with all 47 bones correctly named, the right frame count
   and the right rate, holding the rest pose. It reads as "the clip is empty".
   The tell is the bone paths: if bone 0 is a rig name rather than `Hips`, the
   root is wrong.

2. **Root motion must be read off the clip, not sampled.** `SampleAnimationClip`
   poses the skeleton but never applies `RootT`/`RootQ` — that is a per-frame
   delta the Animator applies at play time, and nothing applies it in the
   editor. This is worse than a character running on the spot: for these clips
   the *stance height* is in `RootT.y`, **0.15 prone against 0.86 standing**, so
   a prone soldier bakes as a standing-height figure floating in mid-air.
   `Stand_death` likewise falls 0.75 m and travels 1 m, all of it in the root.

### Checking it worked

`ba_track.py` reports motion; `ba_posediff.py` compares two clips' frame-0 poses
bone by bone. The second is the one that matters, because "does it move" cannot
distinguish a prone idle from a broken retarget — a real prone idle moves 3
bones by 2.6°, which looks exactly like failure.

Measured on `US_Ranger`, and this is what correct looks like:

| check | result |
|---|---|
| `Kneel_run` motion | 18 of 47 bones, 62.4° max swing |
| `Kneel_idle` vs `Stand_aim` frame-0 | **118.5° at RightLeg, 107.6° at LeftUpLeg** |
| `Prone_idle` vs `Stand_run` frame-0 | 68.5° at LeftUpLeg, 51.7° at Neck |
| `Stand_death` root | y 0.211→0.937, z 0.013→0.990 |

The middle two rows are the proof: kneeling versus standing *is* a hundred
degrees at the knee and hip, and prone versus running *is* a raised neck. Those
are not numbers a broken retarget produces.

### The full bake, measured

**422 rigs, 4,890 clips, 0 failures** — 4,874 `.batrk` files, 743 MB. (The 16
missing are name collisions where one rig references two clips whose sanitised
names match; the second overwrites the first.) `ba_animcheck.py` sweeps the lot:

| motion | tracks | |
|---|---:|---:|
| large (>60°) | 2,125 | 43.6% |
| moderate (15–60°) | 914 | 18.8% |
| subtle (1–15°) | 1,190 | 24.4% |
| static (<1°) | 645 | 13.2% |

4,284 humanoid, 590 generic. **The 645 static tracks are correct, and checking
that was the last real question.** They are dominated by `Kneel_aim` (176 rigs)
and `Prone_aim` (176) — blend-tree pose inputs whose aim direction comes from
runtime IK rather than the clip. Verified against the source rather than
assumed: `Kneel_aim`'s curves have a value range of *exactly* 0.0, `Eject` and
`WaterShield` have no multi-key curves at all, and `Prone_aim`'s largest range
is 0.277 muscle units against `Stand_run`'s 1.68, its second largest being
`LeftHand.Little.Spread`. Meanwhile `Kneel_reload` carries 48 keys per curve and
bakes to 107 frames of real motion.

The general rule this leaves: a clip that bakes static should have 1–2 keys per
curve in its `.anim`. If a *movement verb* — run, reload, shoot, death — turns
up in `ba_animcheck.py`'s static list, that is a regression, not authoring.

### FBX out: `unity/BaExport.cs`

```
Unity.exe -batchmode -quit -nographics -projectPath <proj> \
          -executeMethod BaExport.Export -baOut D:/ba_extracted/fbx
```

One FBX per rig — mesh, skeleton, skinning and every clip as a take. Resumable:
an existing output is skipped. Needs `Mesh/` and `Material/` copied into the
project alongside the folders the bake needs, or you get skeletons with no
geometry.

**The export runs in Unity rather than Blender on purpose.** The baked tracks
are Unity local transforms: left-handed, Y up, against Unity's bone rest poses.
Blender is right-handed Z up *and* its FBX importer rewrites bone rest
orientations on the way in, so applying these by hand means composing an axis
conversion with a per-bone rest-basis change and being wrong in a way that looks
almost right. Unity already knows how to write an FBX.

**The clips are rebuilt as legacy generic clips before export.** The FBX
exporter takes its takes from an `Animation` or `Animator` component — hand it
the originals and it writes the humanoid muscle curves straight into the FBX,
which is the thing this whole exercise exists to escape. Legacy clips on an
`Animation` component specifically, because they can be built in memory; an
AnimatorController would mean thousands of `.anim` and `.controller` assets on
disk for files nobody keeps. The `Animator` must be destroyed first, or the
exporter sees both components and prefers the controller.

**Two `ExportModelOptions` defaults are wrong here**, and only one of them says
so:

| option | default | why it matters |
|---|---|---|
| `ExportFormat` | `ASCII` | Blender refuses outright — "ASCII FBX files are not supported". Loud, harmless. |
| `AnimateSkinnedMesh` | `false` | **Silent.** Valid binary FBX, mesh, skeleton, all takes present — and no animation on the skinned mesh. |

**The exporter leaks a native `FbxManager` per call** and kills the editor with
an access violation inside `FbxManager_Create` at around 170 rigs. Nothing in
the managed API exposes that lifetime, so it cannot be fixed here — the only
lever is process lifetime, which is why `ba_anim.ps1` relaunches Unity until a
pass adds no files rather than running one long export.

**Names are assigned over the rig set, up front, before any export.** Three
rigs share a prefab basename — `RU_Morskaya_MG`, `US_MARINE_Radio`,
`US_MARINE_rifle` — and naming output after the prefab alone silently drops the
second of each pair as "already exported". Within a group the first by asset
path keeps the plain name; the rest get an 8-hex FNV-1a suffix of their path.
Deterministic, so a re-run assigns identical names and resume-by-existence stays
honest.

The filtering to rigs has to happen *before* that grouping. There are 1,410
prefabs and 422 rigs; let a non-rig into the grouping and it takes the plain
name and pushes a real rig onto a hashed one — and since nothing ever exports
the non-rig, the plain name simply never appears. Getting this wrong once left
three orphan files that a directory listing could not distinguish from real
output. `BaExport.Manifest` exists for exactly that: it emits the authoritative
name→prefab mapping without exporting, so what is on disk can be diffed against
what should be.

Note that **FBX files cannot be compared byte-wise**. Two exports of the same
object differ — creation timestamp, file GUID, and freshly generated object IDs
throughout, so even skipping the header does not help. To prove an orphan was a
duplicate, recompute the FNV-1a of the prefab path and match it against the
suffix.

### Verified

`US_Ranger_Rifle1`, imported into Blender: binary, 18.5 MB, 3 LOD meshes at
32,343 verts, a 39-bone armature with vertex groups intact, and **22 takes
totalling 641,190 keyframes** — `Kneel_reload` at 399 fcurves / 42,693 keys over
107 frames. 399 is 39 bones × 10 channels plus 9 for the root, which is the
arithmetic working out.

| rig | meshes | bones | takes | keyframes |
|---|---|---|---|---|
| `US_Ranger_Rifle1` | 3 LODs, 32,343 v | 39 | 22 | 641,190 |
| `RU_VDV_rifle` | 3 LODs, 21,746 v | 39 | 22 | 641,190 |
| `RU_2S7M_Malka` | 24, 92,666 v | 156 | 56 | 1,524,144 |

`RU_VDV_rifle` is the check that matters: it is one of the rigs AssetStudio
exported with 21 correctly-named, entirely empty takes.

**Final: 422 FBX, 4.99 GB — one per rig, zero missing, zero stale**, diffed
against the manifest rather than counted.

## Into the asset browser

`tools/asset_browser` reads `.obj` and `.glb` and plays animation from any
rigged `.glb` — clip list, playback, repeat. So the FBX are converted rather
than the browser taught a new format:

```
.\tools\ba\ba_anim.ps1 -Stage glb              # 6 Blender shards + fallback
py -3 tools/ba/ba_materials.py --bundles <aa/PC>   # material -> texture table
py -3 tools/asset_browser/asset_browser.py --refresh
```

**422 glb, 1.62 GB, 4,845 clips.** In the catalogue as `ba mesh dif+msk+nrm`
(the rigs) and `ba mesh none` (11,056 raw OBJ from the sweep), plus textures
split by role.

### What is solid, and what is not

| | |
|---|---|
| rigs converted | 423 / 423 |
| load with a clip list | 423 (4,873 clips) |
| with geometry | 423 |
| textures resolve | 416 |
| unloadable | 0 |
| humanoid rigs on the real Avatar | 221 |

The two without textures — `US_DELTA_FORCE` and `Destroyed_Arleigh_Burke` — bind
no texture in **any** slot in the bundle, checked by reading the Material
objects directly. That is the game's data, not a gap in the join, so 420 is the
ceiling.

A third of the roster has *some* untextured material, and that is also correct:
almost all of them are canopy and window glass, which carries no albedo.

### The Avatar, which is why every human animation was wrong

The single biggest defect in this whole extraction, and the one that survived
every headless check. Reported by eye - "the majority of the human animations
were all odd", "feet coming up past their waist" - while clip counts, keyframe
totals, joint counts and posed bounding boxes all read healthy.

**Humanoid clips are muscle values.** They carry no bone transforms at all; they
retarget onto whatever the Avatar maps and leave everything it does not map
frozen in the rest pose. The Avatar AssetRipper reconstructs resolves **22 of
Unity's 55 human bones**, so hips, spine, head, both shoulders, one hip, one
knee and one foot never moved. Measured on `Stand_run`: 11 bones moving out of
47, `LeftUpLeg` 51 degrees against `RightUpLeg` 0.0, `RightLeg` 55 against
`LeftLeg` 0.0. Alternating down each limb, which is what "half the body" looks
like in numbers.

It also explains the split that was visible from the start: **vehicles were
always fine** because generic clips carry transform curves directly and never
touch an Avatar.

`ba_avatars.py` pulls the game's own `Avatar` objects out of the bundles. Two
things come from there and neither can be inferred:

- `m_TOS` + `m_Human.m_HumanBoneIndex` - the real human bone mapping.
- `m_Human.m_SkeletonPose` - **the reference pose the muscles were authored
  against.** Rebuilding the mapping from bone names alone gets the first and
  misses this, and Unity then treats the rig's current pose as the T-pose. These
  rigs are not in a T-pose, so every clip retargets onto a bad reference and
  plays crouched. That intermediate attempt is worth knowing about because it
  looks like progress: full bone coverage, wrong poses.

**Unity's internal human bone order is not the `HumanBodyBones` enum order.**
`UpperChest` sits between `Chest` and `Neck` in `m_HumanBoneIndex`, where the
enum appends it last as bone 54. Getting that wrong shifts everything from index
9 and still produces a *plausible-looking* mapping - `Head`→`Neck`,
`LeftShoulder`→`Head`. The tell is the left/right swap it creates:
`LeftHand`→`RightForeArm`. A correct mapping never crosses sides.

Result on `Stand_run`, per bone:

| bone | broken Avatar | real Avatar |
|---|---:|---:|
| Hips | 0.0° | 7.8° |
| Spine | 0.0° | 5.1° |
| Head | 0.0° | 4.9° |
| LeftShoulder | 0.0° | 13.8° |
| LeftArm | 0.0° | 20.7° |
| RightUpLeg | 0.0° | 44.5° |
| LeftLeg | 0.0° | 75.2° |
| RightFoot | 0.0° | 65.3° |

Moving bones 11 → 23, and symmetric left to right. Visually: `Stand_aim` goes
from arms hanging limp to the rifle shouldered in a firing stance, and
`Kneel_reload` from standing to actually kneeling.

Re-baking is `BaExport.Export -baAvatarData avatars.txt`, which rebuilds the
Avatar per rig before sampling. 221 humanoid rigs.

### And do NOT add root motion on top

A separate fault, found only after the Avatar was fixed, because until then the
poses were too wrong to see it: a soldier would kneel correctly and then flip
into an impossible position partway through a death.

An earlier version wrote each clip's `RootT`/`RootQ` curves onto the Animator's
transform, on the reasoning that sampling never applies root motion and that
stance height lives in `RootT.y` - 0.17 prone against 0.88 standing. That was
reasoning about the clip data rather than about what retargeting produces.

Measured on `Stand_death` with the real Avatar: **the Animator transform stays
at (0,0,0) for the entire clip** while the hips fall from y=0.99 to y=0.25,
travel a metre and rotate 88 degrees onto the character's back. The retargeted
pose already carries the whole body motion. Adding the root curves applied every
bit of it twice - and doubling an 88-degree rotation is why the first half of a
death read correctly and the second half flipped. A correct pose with corrupted
placement.

After removing it, only `Hips` translates, and the posed bounding box falls from
1.81 m standing to 0.71 m on the ground.

**Both sampling paths agree.** `AnimationMode.SampleAnimationClip` (what the
bake uses) and a `PlayableGraph` (the runtime path) produce identical per-bone
rotation ranges once the real Avatar is applied - 22 moving bones, bone for
bone. Worth knowing, because it rules the export path out when something still
looks wrong. Note that `BaPreview.Ranges` must be given `-baAvatarData` too, or
it silently measures the broken Avatar and reports ten moving bones whatever it
is comparing.

**How to check this, since nothing automated caught it:** render the clip from
Unity itself with `BaPreview.Shoot` and compare against the browser. Anything
downstream of Unity - the FBX exporter, Blender, the glTF loader, the viewer's
rasteriser - can be the culprit, and only a reference drawn by the engine that
owns the clip distinguishes them. `BaPreview.Ranges` prints per-bone rotation
ranges for both sampling paths, which is what turned "looks odd" into "22 of 55
bones are frozen".

### The crew-rig problem, which is the big one

Found by rendering every rig and looking, after every headless check had passed.
Pilots and vehicle crew appeared **huge and lying sideways**, swamping their
vehicle — a blue-suited pilot 185 m across against an 18 m MiG-35. It reads as
broken animation and is not: it is a broken bind pose.

**These prefabs contain two skeletons** — an airframe rig (`root`/`body`) and a
crew rig (`Hips`) — 134 of the 422. Unity's FBX exporter writes the second one's
mesh into a mangled bind space: the pilot lands 145 m from the aircraft with a
185 m span. Nothing in Unity shows this; no renderer sits more than 20 m from
the root there. It is also what Blender's importer chokes on
(`KeyError: root`), so the two symptoms share a cause.

Three fixes, and it took all three:

1. **Inverse bind matrices are per (skin, joint), not per joint.** The
   multi-skin support below deduplicated by bone node and kept the first skin's
   matrix, so a pilot sharing a node with the airframe inherited the
   *aircraft's* bind. Now keyed by the pair.
2. **`-baOneSkeleton`**: group skinned meshes by `rootBone` and export only the
   largest group, dropping 250 crew skeletons across 134 rigs. HMMWV 536 m →
   4.9 m, M113 188 → 5.9, S-300 193 → 10.5. Group by `rootBone` *itself*, not by
   its top-level ancestor — a pilot's `Hips` hangs under the airframe's `body`
   bone, so walking to the top merges the rigs back together and drops nothing.
   Installed **only where the result measured smaller**: on 11 rigs the drop
   removed a bone the surviving meshes needed and made things worse.
3. **An outlier guard in the loader**, because the export fixes could not reach
   every case. A primitive more than 8× the median span of its own file, and
   over 50 m, is skipped — a `US_Pilot_Plane` at 187 m inside an 18 m F-15E, a
   `PKT_MAT` machine gun at 264 m on a 2S4. Judged against the file's own median
   so a genuinely large object stays whole.

**Result: rigs with plausible dimensions went 347 → 406 of 422**, nothing
emptied. The 16 that remain are led by `Destroyed_LHA`, a single 26 km
primitive — there the broken mesh *is* the main body, so no outlier rule helps.

### Two bugs only visible in the GUI

Both were found by opening the browser and looking, after the headless checks
above all passed. Worth recording as the limit of what a scripted check sees.

**Models faced down.** The preview showed the bind pose, and for these files the
bind pose genuinely is face down — see the orientation note below. The browser
now poses a standing clip by default for this library, chosen by name: *not*
clip 0, which sorts to `Kneel_death` on most infantry and would open every
soldier lying dead on the floor.

**Nine US Marine rigs had no geometry at all** — 10 MB of animation curves and
not one triangle, which in the browser is an invisible or untextured model
rather than an error. Those prefabs put their Animator on a child called
`SupportBones`: the skeleton is under it, the fourteen SkinnedMeshRenderers are
siblings *outside* it, and `BaExport.cs` exported the Animator's subtree.
Fixed by exporting the prefab root whenever the Animator is not on it —
`US_MARINE_rifle` went from 0 to 21,089 verts.

Seven of those nine come from a `US_Marines/old/` folder and still pose badly
with their geometry restored: deprecated prefabs whose rig no longer deforms.
They are browsable rather than invisible, and the current Marine roster
(`US_Marine_rifle2`, `US_Marine_officer`, the Raiders) was never affected.

**54 rigs need the FBX2glTF fallback.** Blender's FBX importer dies on them with
`KeyError: root` at `mesh.armature_setup[self]`, and every import option fails
identically including `use_anim=False`, so it is structural, not a setting.
Measured cause: these prefabs contain **three separate skeleton roots** —
`root`, `body` and `Hips` — an airframe rig plus a crew rig in one file. Every
mesh belongs to exactly one of them (nothing skins across two, and no bones sit
outside the exported subtree), so it is the plurality itself that io_scene_fbx
cannot represent.

Two wrong guesses preceded that measurement, both worth recording because they
sounded right: a *nested Animator* (only 1 of the 54 has one — `BaExport.cs`
gained `-baStripNested` for it, which fixed exactly that one rig), and *bones
outside the exported subtree* (zero rigs). `BaExport.Diagnose` exists to answer
this kind of question with numbers instead.

**The fallback's output was fine all along; the browser was reading it wrong.**
FBX2glTF writes **one skin per mesh** — 24 for `RU_Morskaya`, 17 for
`RU_AN72P` — where Blender writes a single merged skin. `load_glb_rig` took
`skins[0]`, so a 50-bone soldier was posed through a 4-joint skeleton and tore
itself apart. Fixed by building a union skeleton across all skins and remapping
each primitive's joint indices into it: `RU_Morskaya` now resolves 49 joints,
`RU_AN72P` 137. This is a genuine multi-skin glTF gap and helps any such file,
not only this game's.

### Two browser-side rules this needed

- **Drop primitives whose material is `LODs`.** Unity's LOD-crossfade proxy is
  a 24-vertex box sized to enclose the whole object, so fitting a camera to the
  bounding box frames the *box*: a BTR-82A renders as a plain grey cube with
  85,000 verts of armour hidden inside it. Dropped by material name, not vertex
  count — 24 verts is also a perfectly good detail part.
- **Material names are mangled by the exporter.** Unity's FBX exporter has
  `UseMayaCompatibleNames` on by default, so `RU_1BTR80/82` arrives as
  `RU_1BTR80_82` and matches nothing. Both spellings are indexed, exact first.
- **Take the first candidate row that has MAPS, not the first that matches.**
  `US_Ranger_1` and `US_Ranger` both exist in the table; the mesh names the
  first, whose three columns are empty, and matching exactly shadowed the row
  carrying the whole texture set. The soldier rendered untextured, which looks
  exactly like a material that legitimately has none — the same way glass does.
- **Skip animation channels targeting `weights`.** glTF morph-target animation,
  which this viewer does not do. Two transports carry one such channel, and
  indexing the path table blindly raised `KeyError` — costing the entire file
  rather than one wobbling flap.

### Orientation, and how not to debug it

`export_yup=True` is all that is needed, and the reason this is written down is
that verifying it the obvious way is wrong.

Rendering the mesh straight from the file shows a character lying down or
upside down, which looks exactly like a broken export. It is not: for a
**skinned** mesh the positions in the file are mesh-local and mean nothing
alone — the world orientation only exists once the skin is applied, because the
joint matrices carry the node hierarchy. Judging by that render produced a
180-degree flip that made the rest pose look right and every clip play upside
down; moving the flip onto a parent empty then fixed the symptom and preserved
the error.

**The test that works: pose the mesh with a clip, then measure.** A standing
character's tallest axis must be Y. `US_Ranger_Rifle1` playing `Stand_run`
measures x=0.96 **y=1.81** z=1.32, and renders as a soldier running.

The same trap applies to the *pose* used for a check: clip 0 is alphabetically
`Kneel_death` on most infantry, so a figure lying on the ground is correct and
proves nothing either way. Check with a `Stand_` clip.

**The output tree exceeds `MAX_PATH`.** Container paths like
`Assets/Prefabs/GUI/GameMenu/Settings/Property Labels Description/...` plus the
`@pathID` suffix push past 260 characters, and anything not opted into long
paths — including PowerShell's own `Get-ChildItem` — fails to enumerate with
"could not find a part of the path". AssetStudio (.NET 9) writes them without
complaint; it is only reading them back that trips. Prefix the path with `\\?\`,
or enable `LongPathsEnabled` machine-wide. Worth knowing before pointing
`tools/asset_browser` at this library.

## Not in the default sweep

- **Granite** (`-Kind granite`). 27 GB of `.gts`/`.gtp` — by volume the biggest
  thing in the install by a wide margin, and it decodes to more than it stores.
  Graphine's tile-streaming format; Unity bought Graphine and shelved it, so the
  format is frozen and [Nenkai's GraniteTextureReader](https://github.com/Nenkai/GraniteTextureReader)
  reads it. Four layers: albedo, normal, and two RGB masks — the script takes
  all four, because taking only albedo loses the half that says how the material
  system is authored.
- **Cutscenes** (`-Kind video`). 2.5 GB of MP4 inside the addressables.

## Not touched

`StreamingAssets/Navigation/*.ng` — one file per map plus a `_roads` companion,
proprietary, and given what this project is for, the most interesting small
thing in the install. Nothing reads it yet.

EasyAntiCheat ships with the game and is irrelevant here: this reads files on
disk and never attaches to a running process.
