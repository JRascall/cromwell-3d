# World in Conflict (MassTech) extraction

Reads the retail install of **World in Conflict** (Massive Entertainment, 2007)
for the studies in
[`study/games/strategy/world_in_conflict/`](../../study/games/strategy/world_in_conflict/).

Output lands in `wic_extracted/` at the project root, gitignored, matching where
`xcom_bulk.ps1`, `hd2_extract.ps1`, `r6_extract.ps1`, `sc3k_extract.ps1` and
`mercs_extract.ps1` put their sweeps. Massive's art and audio are read for
reference and never committed or shipped.

```powershell
.\tools\wic\wic_extract.ps1 -List            # inventory only
.\tools\wic\wic_extract.ps1 -Kind data       # .sur shaders, .pe effects, config
.\tools\wic\wic_extract.ps1 -Kind script     # .pyo -> readable Python
.\tools\wic\wic_extract.ps1                  # everything except audio
.\tools\wic\wic_extract.ps1 -Kind audio
```

| script | job |
|---|---|
| `wic_extract.ps1` | driver: sweeps the 26 `.sdf` archives, dispatches the decoders |
| `wic_sdf.py` | the `RYS` container — list, glob, extract |
| `wic_tex.py` | headerless textures → PNG, with format/geometry recovery |
| `wic_pyo.py` | Python 2.3 bytecode → source (needs `uncompyle6`) |
| `wic_mrb.py` | meshes → Wavefront OBJ (**partial — see below**) |
| `wic_nuke_sheets.py` | contact sheets of one effect's textures, for looking at |

Requires `py -3` with `numpy`, `pillow` and `uncompyle6`.

## What comes out

Measured on this machine, from a 6.2 GB install:

| | Files | Size |
|---|---:|---:|
| `raw/` | 36,319 | 9.6 GB |
| `texture/` — PNG | **9,855 of 9,866** | 4.9 GB |
| `script/` — Python | **375 of 375** | 11 MB |
| `model/` — OBJ | **2,691 of 3,308** | 1.8 GB |
| `raw/sound/` | 13,585 mp3 + 894 wav | 0.8 GB |

Extraction itself is **complete**: every distinct path in the 26 archives is on
disk (13,585 mp3, 894 wav, 88 bik, 9,866 dds, 1,344 tga, 3,308 mrb, 2,181 pe,
108 sur). A raw count against archive *entries* looks short by a few dozen per
type — that is the patch re-issue duplication described above, not a miss.

Texture formats recovered: DXT1 6,564, DXT5 1,815, R8G8B8 62, A8R8G8B8 32,
A8L8 5, plus 26 particle flipbook atlases. Most common resolution is 1024×1024
(3,027), then 512×512 (1,607) and 256×256 (1,055).

**Read `texture/_lowconfidence.txt` before trusting a specific texture.** 5,055
of the identifications land under the confidence margin — usually because the
runner-up is a plausible alternative reading of the same bytes. The winner is
generally right (spot checks decode to recognisable art) but the file is there
so that "this one is a guess" is visible rather than implied.

## Models: how `.mrb` was solved

`.mrb` has no public documentation and no existing reader; the format was
reversed here from the shipped files. A mesh block is:

```
u32  vertexCount
vertexCount * stride      pos f32[3] ... normal f32[3] ... uv f32[2]
u32  (zero)
u32  indexCount
indexCount * u16          triangle list
```

Found by locating the index buffer — a long run of bounded, locally-coherent
u16s — and reading backwards from it. Validated by a property that cannot occur
by chance: **the normals must be unit length**. On the reference file the true
block gives mean |n| = 1.0000 with std 0.0000 and every other offset gives
garbage, which makes the detector effectively false-positive free and makes it
safe to *scan* for mesh blocks rather than parse the node tree.

**There is no single vertex record**, because the engine builds a D3D9 vertex
declaration per material out of whatever its shaders ask for. The `VSINPUT`
recovered from `wic.exe` is

```hlsl
float4 myPos : POSITION;   float4 myWeights : BLENDWEIGHT;
float4 myBoneIndices : BLENDINDICES;   float3 myNormal : NORMAL;
float4 myDiffuse : COLOR0; float4 myTanspace : COLOR1;
float%d myTexcoord%d : TEXCOORD%d;
```

with the trailing texcoords variable in count *and* width, so strides of 32
through 60+ all occur. Two confirmed by hand: **36** (`pos f32[3], normal
f32[3], colour u8[4], uv f32[2]` — props) and **60** (`pos f32[4], skinning 12 B,
normal f32[3], tanspace u8[4], uv f32[2], uv2 f32[2]` — skinned units). Rather
than enumerate the rest, the reader *discovers* the normal offset per block (it
is whichever float3 is unit length) and picks the UV pair by range.

### The bug that made units look encrypted

Worth recording because it cost hours and the symptom was completely
misleading. Unit meshes appeared to have **no float32 normals at any stride or
alignment**, which pointed at a packed or quantised format — and the evidence
seemed to agree: byte autocorrelation gave a clean stride-60 peak, and probes
for int16/int8/ubyte-normalised normals found nothing periodic.

The data was ordinary float32 the whole time. **Mesh blocks are not 4-byte
aligned** — one unit buffer starts at `0x000cca`, which is even but not a
multiple of four. A numpy float view taken from offset 0 only ever sees 4-aligned
words, so it is *structurally incapable* of seeing those fields, and the
conclusion "there are no float normals here" was an artefact of the tool rather
than a fact about the file. Scanning all four byte phases fixed it and took
coverage from 367 files to 2,687.

The lesson generalises: when a probe returns "this pattern does not exist
anywhere", check that the probe could have seen it.

### Result

| | |
|---|---:|
| converted | **2,691 of 3,308 files** (81%) |
| geometry | **14.5 M verts, 11.4 M tris** |
| no mesh found | 617 — but **531 of those are under 4 KB** (median 767 bytes) and hold no geometry at all, so genuine misses are ~86 files (2.6%) |
| errors | 0 |

Validated against reality, not just against itself: the exported Leopard 2A4
measures **3.76 m wide × 3.41 m tall × 9.63 m long** against a real 3.70 × 3.03
× 9.97 m, and renders as an unmistakable Leopard in profile — hull, turret, gun,
cupola, antennas, side skirts, road wheels.

Files contain multiple blocks (LOD chain plus sub-objects); each becomes an OBJ
`g` group, with the top LOD first.

What is **not** recovered: per-submesh material and texture assignment (the
strings are in the file, but their binding lives in the node tree), skinning
weights, and animation. Output is OBJ, not FBX — FBX needs the Autodesk SDK.

**`.gety` is the exception worth knowing about**: 223 wreck models ship as
literal LightWave `FORM…LWO2` files, a documented public format that Blender
and Noesis open directly with no work at all.

## Why this install is worth reading

Unusually among the libraries on this list, **the valuable part is text**. WiC
ships 130 `.sur` surface files containing inline HLSL *with Massive's comments
intact*, 2,187 particle effects as plain key–value files, its per-GPU quality
database as a commented `.txt`, and its entire gameplay layer as Python
bytecode that decompiles cleanly — 375 modules, 7.7 MB of source, zero
failures. The engine describes itself.

## Three traps, all of which produced wrong data first

These are documented at length in the scripts because each one fails *silently*.

**Payloads are a sequence of independent zlib streams, not one.** A single
`zlib.decompress()` raises on the trailing bytes, and the obvious fallback — a
raw-deflate (`-15`) decompressor — does not raise, it returns **garbage of
plausible length**. A 10 MB texture silently became a 6.8 MB one and looked
entirely fine. `wic_sdf.py` consumes stream after stream and refuses to return a
short buffer.

**Archive order is not alphabetical.** `wic1`–`wic5` are the shipped content;
`wic20` … `wic65` are patches that *re-issue* files from the earlier archives.
Extraction is last-writer-wins and must run in release order — sorted by name,
`wic20` lands before `wic3` and a 2007 file quietly overwrites its 2009
replacement. This is also why resumability lives at archive granularity (a
`_done/` marker per archive per kind) rather than per file: a per-file "already
exists" skip would keep superseded versions.

**Textures have no header and no format record anywhere.** Every `.dds`
decompresses to exactly its declared size minus 128 — the size of a D3D9 DDS
header — and the table of contents contains no `DDS ` magic, no dimensions and
no format field. The header is dropped at pack time and rebuilt by the engine
from knowledge held in the executable, which we do not have.

So `wic_tex.py` **searches**: it enumerates every `(format, width, height,
mips)` whose size model matches the payload, then decodes each candidate and
scores it. Size alone is genuinely ambiguous — DXT1 512×512, A8L8 256×256 and
R5G6B5 256×256 are all 174,760 bytes with the same mip chain. Two signals
separate them:

- **mip agreement** — level 1 should be level 0 at half size. A wrong triple
  almost never yields a consistent pyramid. This is the strong one.
- **smoothness** — a correct reading is a smooth image; a wrong one reads
  adjacent texels out of unrelated blocks and looks like noise.

Every result carries a confidence margin, and true readings win by 2–200×
while false ones win by under 1.35 — which is where the threshold comes from.
Anything below it goes to `_lowconfidence.txt` rather than being silently
trusted. DXT3 and DXT5 share an identical colour block and are reported as one
class.

**And where better evidence exists, the search is helped rather than replaced.**
Particle flipbooks are its hard case, and the `.pe` files that reference them
declare `PARTICLEWIDTH`, `PARTICLEHEIGHT` and `NUMTEXTURES` outright — so the
frame geometry never has to be guessed.

**What the declaration does not say is how the frames are packed, and that cost a
round of wrong output.** A stack of N frames each carrying its own small mip
chain and a single grid image carrying one mip are *the same number of bytes* —
`128 × (128·128·4 + 64·64·4)` and `2048·1024·4 + 1024·512·4` are both 10,485,760
— so neither the size model nor the mip-agreement test can separate them, and a
stack of smoke frames decodes smoothly enough to look right in a scorer. The
first implementation chose the stack. WiC actually ships **grid atlases**: the
six-point smoke pair is one 2048×1024 image, 16×8 frames of 128×128, A8R8G8B8
plus one mip for the positive triple and R8G8B8 for the negative.

Read as a stack, every frame comes out as a comb of interleaved rows, because a
128-wide view of a 2048-wide image walks a sixteenth of a row at a time. **Only
looking at the decoded image caught this** — which is why `to_png` now writes the
whole grid rather than frame 0, and why `wic_nuke_sheets.py` exists. Two further
fixes went in alongside: `pe_hints` no longer reads the `TEXTURE3` slot (it is
`surfaces/dummy.dds` in nearly every effect, which declared the shared dummy to
be a 128-frame flipbook), and a hint that two effects disagree about is now
dropped rather than taken first-writer-wins — that disagreement was silently
mis-sizing the very atlases the hint path existed to get right.

The lesson is the `.mrb` one from the other direction: **a size model can only
rank hypotheses you already had, and "it decoded without raising" is not
evidence.**

## Formats

| Ext | Files | What it is |
|---|---:|---|
| `.mp3` / `.wav` | 12,014 / 908 | audio; `sound/feedback` alone is 9,668 files |
| `.dds` | 10,029 | textures, headerless (above) |
| `.mrb` | 3,367 | render meshes, magic `MRB` |
| `.ice` | 2,228 | object/entity definitions, magic `ice0010` |
| `.pe` | 2,187 | particle effects, **plain text** |
| `.sdw` | 1,738 | shadow meshes, magic `SDW` — stencil volume casters |
| `.tga` | 1,457 | uncompressed textures |
| `.pyo` | 467 | Python 2.3 bytecode — the gameplay layer |
| `.slot` | 242 | text attachment points (`AddNullObject Door`) |
| `.gety` | 223 | LightWave `FORM…LWO2` objects — wreck geometry |
| `.sur` | 130 | surfaces — **plain text with inline HLSL** |
| `.mot` / `.mmb` | 54 / 52 | scripted motion paths and animation |

Container format transcribed from aluigi's QuickBMS script
`world_in_conflict.bms`, re-implemented in Python so the archives can be listed
and selectively extracted rather than unpacked whole.
