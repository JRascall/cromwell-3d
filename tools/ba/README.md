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
