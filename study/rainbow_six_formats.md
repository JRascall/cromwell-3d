# Rainbow Six: Siege — the FORGE (scimitar) container

What the archives look like on the current build, which parts of the published
format have moved, and what is and is not recoverable. Written because all three
public toolkits fail on this build and the reasons are not obvious — each failure
looks like something else.

Source tags: `[MEASURED]` verified against the shipped build here, `[RF]` taken
from [RainbowForge](https://github.com/parzivail/RainbowForge)'s source (archived
March 2024), `[inferred]` reasoned but not proven.

The install measured: 307 `.forge` archives, 43.7 GB, **941,735 entries**.
`[MEASURED]`

## 1. Two header layouts, and why the wrong one does not crash

`[MEASURED]` The header is `"scimitar\0"`, a `u32` version, then fields whose
offsets **depend on the version**:

| | v33 | v34 |
|---|---|---|
| version | 0x09 | 0x09 |
| *(new field, always 6)* | — | **0x1E** |
| numEntries | 0x1E | 0x22 |
| numDirectories (== 2) | 0x22 | 0x26 |
| sizeOfFat | 0x3A | 0x3E |
| numTables | 0x3E | 0x42 |
| firstTablePosition | 0x42 | 0x46 |
| header size | 0x4A | 0x4E |

**v34 inserts a `u32` at 0x1E that v33 does not have**, shifting everything below
it by four bytes. 215 of the 307 archives are v34 — including *every* texture and
sound archive — and 92 are v33, including most mesh archives.

The trap is that reading a v34 archive with the v33 layout does not fail where
the mistake is. `numEntries` reads back as a plausible small number, the FAT
pointer reads back as garbage like `0x4e00000001`, and the process dies much later
on a seek past the end of the file. RainbowForge, RainbowForge3 and Prism_V2 all
do exactly this.

Two invariants pin the layout and are worth asserting rather than assuming; both
hold across all 307 archives with zero exceptions `[MEASURED]`:

* `sizeOfFat == numEntries + 2` — the two extras are the descriptor and hash
  containers.
* `firstTablePosition == the header size` — 0x4A at v33, 0x4E at v34.

## 2. The v34 file allocation table is encrypted

`[MEASURED]` At v33 the FAT is plaintext: 20 bytes per entry, `(offset i64,
uid u64, size u32)`, sorted by offset.

At v34 the 20-byte stride survives — the table region is exactly
`sizeOfFat * 20` bytes in every archive checked — but the fields are not
plaintext. What the raw dwords show:

* the offset-high dword decrypts to a **single constant `0x2cbcb973` in every
  archive**, which is what you would expect if the plaintext there is zero;
* the offset-low dword has constant low 12 bits (`0x724`), consistent with
  4096-aligned offsets;
* but no constant XOR and no constant subtraction produces offsets that land on
  real containers. Set-matching the decoded values against the true container
  offsets (recovered independently, below) fails for every candidate key.

So the FAT is encrypted with something that is not a simple additive or XOR mask.
This is almost certainly why public tooling stopped working, and it was not worth
solving, because:

**The table does not need to be read.** Containers are 4096-aligned and
self-describing, so the archive can be enumerated by scanning for the container
magic. On every archive checked this recovers exactly `numEntries - 2` assets —
the descriptor and hash being the two absent. Entry size becomes the gap to the
next container, an upper bound rather than the exact length, which is safe because
every reader takes its real lengths from the block headers.

## 3. Containers

`[RF]`, offsets `[MEASURED]`. A container opens with the 8-byte magic
**`0x1015FA9957FBAA37`**. The low byte is a format revision: RainbowForge knows
`…AA34` and `…AA36`, RainbowForge3 added `…AA37`, which is what this build uses.

Then two blocks — a metadata block and the asset block — each
`u16 x, u16 deserializerType, u8, u16` followed by a chunk table:

```
u16 numChunks, u16
numChunks × (u32 unpackedLength, u32 packedLength)
then per chunk: u32 hash, followed by packedLength bytes
```

A chunk is compressed when `unpackedLength > packedLength`. `deserializerType`
3/13/15 are chunked (15 is new on this build and is Oodle); 7 is a flat block.
Compression is **Oodle**, and the game does not ship `oo2core` loose — it is
statically linked into `RainbowSix.exe`, so a copy has to come from elsewhere.
`oo2core_9` decodes `oo2core_8` streams, so any UE4/UE5 install serves.

Because the two blocks both carry the magic, an unaligned scan finds each
container twice, 63 bytes apart `[MEASURED]`. Only page-aligned hits are starts.

## 4. Asset type — the FAT no longer carries it

`[MEASURED]` The per-entry metadata table (320 bytes per entry, holding name,
timestamp and file type) is **all zeros** in every v33 and v34 archive. Typing
assets off it makes everything `Unknown`; RainbowForge3 works around this by
forcing `AssetType.Texture`, which is right only inside a textures archive and
silently corrupts every other kind.

The real type is in the `FileMetaData` block at the head of the *decompressed*
container payload:

```
u16 nameLength, u16 (always 2), u32
byte[nameLength]      <- see §6, this is not a usable name
u32 fileType
u64 uid
```

Read it, then rewind — every parser reads that same block itself as its first
act. Verified: mesh archives type as `CompiledMeshObject`, textures as
`CompiledLowResolutionTextureMap`, meshshape as
`CompiledMeshShapeDataObject`, soundbank as `CompiledSoundMedia` +
`CompiledSoundBank`.

**Most type IDs on this build are not in RainbowForge's table.** Its ~840 class
IDs date from 2021; the map archives are dominated by `0x569859AA` and the
on-demand archives by `0xB4361608`, neither of which appears in it. Unknown IDs
should be recorded as hex, not guessed at.

## 5. Sound is stored two completely different ways

`[MEASURED]` This is the one that hides audio from every tool:

* **`*_soundbank`** (13 archives) — ordinary Oodle containers of type
  `CompiledSoundMedia`. The Wwise `.wem` is inside the decompressed payload at a
  **variable** offset, after the name blob. RainbowForge's `WemSound` walks a
  fixed field layout that has since moved, so it writes a non-RIFF file with a
  `.wem` name that no converter will touch. Searching the payload for the
  `RIFF`/`WAVE` signature and trusting the length in that header is indifferent
  to the fields ahead of it.
* **`*_soundmedia`** (14 archives) — **no containers at all.** Bare `.wem` laid
  end to end on 4096-byte boundaries. The container magic occurs *zero* times, so
  both the FAT and a container scan see an empty archive. Found by scanning for
  `RIFF`+`WAVE`; the count matches `numEntries - 2` in every archive, **46,063
  files** in total.

Two scan traps, both of which cost real assets:

1. A `.wem` big enough to span pages contains page-aligned `RIFF/WAVE` byte
   patterns *inside its own audio*. Accepting those invents entries and truncates
   the real one, since size is the gap to the next hit. Skip to the end of each
   accepted wem.
2. Compressed texture payloads occasionally produce a page beginning
   `RIFF....WAVE`. Letting one displace a container swallows real assets. Keep
   container hits and RIFF hits in separate lists and let containers win — an
   archive is one kind or the other, never both.

## 6. Asset names are gone — what was ruled out

`[MEASURED]` Names are **not recoverable**, and this is the single biggest
limitation. Recording the evidence so it is not re-derived:

* The FAT's name field is zeroed everywhere (§4).
* The in-payload name blob does not decode. RainbowForge's `DecodeName` — key
  `BASE + uid + dataOffset + fileType + (fileType<<32)`, stepping per 8-byte
  block — yields garbage. Searched exhaustively for a replacement: every additive
  combination of `uid`, `var2`, `fileType`, entry offset and entry uid, then again
  at shifts of 0/8/16/24/32/40/48, solved byte-by-byte from the LSB (exact,
  because low bytes of an addition do not depend on high bytes) against 3,000
  assets at 90% printable-ASCII tolerance. Nothing survived the first byte.
* No plaintext names anywhere else: descriptors hold only the build path
  `//r6s-data/livecert/`; the bootstrap archive holds UI/localisation templates
  (`!!Placeholder`, `!!_TPL__HDG_2D`) and 18,859 `LocalizedString` records; the 48
  `.depgraphbin` files decompress cleanly but contain **zero strings**.

What the blob actually looks like, over 54,731 mesh assets `[MEASURED]`:

* Every single one ends in a `0x00` byte; 86% end in two. Never three.
* The last four bytes are a low-entropy `u32`, always even and under 2²⁴, shared
  across thousands of assets (`0x6ac` alone covers 9,757). That trailing field is
  what makes the last byte always zero — not a null terminator.
* Everything before roughly the last 13 bytes has full 8-bit entropy.
* Among random same-length pairs, most agree at the 1/256 chance baseline — but
  ~3% agree on **40–80% of their bytes**, with differences confined to bytes 3
  and 6 of *every* 8-byte block.

That last pattern cuts both ways `[inferred]`. It proves a stepping-key XOR is
still in use and that duplicate content exists. But differences landing on fixed
positions *within each 8-byte record*, rather than at scattered character
positions, is the signature of a binary record array rather than text. Combined
with the ASCII search failing at byte 0 under every constructible key, the likely
reading is that this field no longer holds a readable name at all.

The only reliable route left is disassembling `RainbowSix.exe`'s deserializer.

## 7. Particles

`[MEASURED]` **Particle system definitions are not extractable, and there is no
FX archive.** The type table knows `FX = 0x824A23BA`, `EffectData`,
`DebrisParticleSystemSetting`, `RuntimeVFXSettings` and `PostEffects`, but:

* none of them appears as a top-level type in any of the 307 archives (sampled
  across all of them), and
* none appears embedded in the decompressed payloads of the bootstrap, on-demand
  or map archives.

Those class IDs are from 2021 and have almost certainly been renumbered. Whatever
describes emitters now lives inside the map archives' `0x569859AA` world blobs —
80 assets per map, up to 18 MB each — which no public parser decodes.

What *is* available for particle work is the art, not the simulation:

* **FX textures** extract normally, but cannot be isolated by type. The texture
  category enum is `Diffuse / Normal / Specular / Misc / Displacement /
  Translucence / ColorMask`, and RainbowForge's own note on `Misc` is that it
  "can be Diffuse, GUI, Normal, Emission, Mask, Distortion, Cubemap or pretty
  much anything" `[RF]`. Flipbooks and noise land in `Misc`, alongside much else,
  and with no names they have to be found by eye.

For emitter behaviour this project is better served by `study/helldivers2_vfx.md`,
where the shaders were readable.

## 8. Map archives

`[MEASURED]` Worth knowing they exist: **40 archives, one per map** —
`pvp01_house`, `pvp02_oregon`, `pvp04_clubhouse`, `pvp09_kanal`, `pvp13_border`,
`pvp16_ibiza`, `pvp19_tower`, and so on, plus `onb00_shootingrange` and
`tdm01_mtown`. Each holds only ~80 entries, nearly all the undecoded `0x569859AA`
world type, at 1–24 MB. The geometry they reference lives in the shared mesh
archives.

These are the analogue of XCOM's parcels, and the same prize — real level layouts
at a known scale. Decoding `0x569859AA` is the single highest-value piece of
unfinished work here.

## 9. CompiledMeshShapeDataObject is not a flat archive

`[MEASURED]` `MagicHelper` routes `CompiledMeshShapeDataObject` (0x9231EE0F) to
`AssetType.FlatArchive`. On this build that is wrong, and expensively so: the
payload is a **single object** behind one `FileMetaData` header, with no second
record anywhere in it.

The flat archive parser derives each record's length from `FileMetaData`'s
`ContainerType`, which in the pre-v31 layout was the `u32` sitting after the name
and genuinely was a length. In the v32+ layout that field is the `u16` before the
name, and it is **always 2** — so the length comes out as `2 - 8 = -6`, the reader
seeks *backwards*, and misparses until it overruns.

The symptom is total: all 38 `*_meshshape` archives produced **zero files and
18,878 exceptions each**. That matters more than it sounds, because
`CompiledMeshShapeDataObject` is the **single largest asset class in the game** —
268,265 objects, more than `CompiledMeshObject`'s 172,782.

Leaving the type unmapped drops it to the raw dump, which recovers all 268,265.
Worth pairing with a general rule: when a typed parser throws, rewind and write
the decompressed payload rather than losing it. Several of these parsers walk
layouts that have shifted since 2021.

## 10. What a full extraction yields

`[MEASURED]` Run against the then-current build, four workers, into
`r6_extracted/`:

| kind | files | size |
|---|---|---|
| `texture` | 238,350 | 97.7 GB |
| `mesh` | 172,743 | 21.5 GB |
| `guitexture` | 28,719 | 6.2 GB |
| `other` | 129,315 | 3.9 GB |
| `meshshape` | 268,265 | 3.4 GB |
| `gidata` | 287 | 2.1 GB |
| `soundmedia` | 46,063 | 2.1 GB |
| `world` | 1,996 | 1.6 GB |
| `soundbank` | 54,372 | 0.9 GB |
| **total** | **940,110** | **139.4 GB** |

Against 941,094 assets catalogued by `r6_index.py`, that is a **0.13% loss**
(1,195 hard errors, mostly `EndOfStreamException` on unknown types). 43.7 GB of
archives expands to 139.4 GB — a bit over 3x, and textures are 70% of it.

Budget: about 100 minutes of wall time across four parallel workers, and the
single 11.4 GB `merged_bnk_textures3` dominates one worker's slice regardless of
how the rest is split.

## 11. Known limitations

* **No names** (§6), so the library is navigable by type, size, archive and
  dependency only.
* **No particle definitions** (§7).
* **World/entity data is opaque** (§8).
* **A residual 0.13% is lost** (§10): 689 `EndOfStreamException`, 184 "container
  is not asset", and a handful of unknown mesh vertex layouts. Concentrated in the
  unmapped types, not in meshes, textures or sound.
* **Meshes come out as OBJ**: positions, normals, UVs and vertex colours, no
  skinning. Siege's rigged characters need more than the OBJ writer emits.
* **Texture output is DDS**, not PNG — DXT1/DXT5/BC5U with mips intact, which is
  the right form for a renderer but needs a converter for eyeballing.
