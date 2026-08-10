# Helldivers 2 container formats

Reverse-engineered from `E:\SteamLibrary\steamapps\common\Helldivers 2` against
a build current as of 2026-08-09. Everything below was verified against the
shipped data, not inferred from documentation — the checks are noted per
section so a future build can be re-validated cheaply.

Implemented in [`tools/hd2_dsar.py`](../tools/hd2_dsar.py).

## Why the public tools find nothing

Helldivers 2 runs on Autodesk Stingray (ex-Bitsquid). Historically its `data/`
directory held thousands of loose files named by a 16-hex-digit hash, in
triples: `<hash>`, `<hash>.stream`, `<hash>.gpu_resources`. Every extractor —
[filediver](https://github.com/xypwn/filediver), Diver, Hellextractor — is
built around that layout.

This install has **10 loose files and 30 `bundles.NN.nxa` archives**. The
bundles didn't change format; they were repacked into a new outer container.
So the tools are not broken, they are pointed at a directory that no longer
holds what they expect. Unpack the archives and they work unchanged.

Three layers, outermost first:

```
bundles.NN.nxa        DSAR   LZ4-compressed chunk container      (28 GB)
  bundles.nxa         DSAR   -> decompresses to the DSAA index
    DSAA index               name -> chunk list, per file        (11.5 MB)
      <hash16>        raw    Stingray bundle, magic 0xF0000011   (127 GB)
```

## DSAR — the compression container

Every `.nxa` **and** each of the 10 loose files in `data/` is a DSAR container.
All integers little-endian.

```
struct DsarHeader {          // 32 bytes
    char     magic[4];       // "DSAR"
    uint32_t version;        // 0x00010003
    uint32_t block_count;
    uint32_t table_end;      // 32 + block_count*32; first block starts later
    uint32_t decompressed_size;
    uint32_t pad;            // 0
    char     tag[8];         // "PADDING*"
};

struct DsarBlock {           // 32 bytes, block_count of them
    uint64_t dec_offset;     // offset in the decompressed stream
    uint64_t comp_offset;    // offset in this file
    uint32_t dec_size;       // 0x40000 (256 KiB) except the last
    uint32_t comp_size;
    uint8_t  method;         // 0 = stored, 3 = raw LZ4 block
    uint8_t  flags;          // 2,3,4,6,7 observed; not needed to decode
    uint8_t  filler[6];      // 54 55 55 55 55 55
};
```

`method 3` is a **raw LZ4 block** — no frame header, no magic; the decompressed
size comes from the table. `method 0` means the bytes are already raw, used for
incompressible payloads (already-compressed textures and audio). One block in
`bundles.08.nxa` is a single stored run of 898 MB.

**Verification.** For every container tested, the last block satisfies both
`dec_offset + dec_size == header.decompressed_size` and
`comp_offset + comp_size == filesize` exactly. Two independent invariants over
the whole table; if a future build changes the layout these break immediately.

The codec was identified rather than guessed: `game.dll` carries LZ4 strings,
and the first block of `bundles.nxa` begins `f0 0d 44 53 41 41 …` — an LZ4
token of `0xf0` (15 literals) plus extension byte `0x0d` gives 28 literal
bytes, which decode to the `DSAA` header including a length field matching the
container header. It is not Oodle (no `oo2core` ships) and not GDeflate,
despite DirectStorage being present in `bin/`.

## DSAA — the index

`bundles.nxa` decompresses to an 11.5 MB `DSAA` blob: the table of contents for
all 30 content archives.

```
struct DsaaHeader {          // 24 bytes
    char     magic[4];       // "DSAA"
    uint32_t version;        // 0x00010001
    uint32_t total_size;     // == length of the decompressed blob
    uint32_t archive_count;  // 30
    uint32_t file_count;     // 8815
    uint32_t filler;         // 0x33333333
};

struct DsaaRecord {          // 24 bytes, file_count of them, from offset 24
    uint64_t file_size;
    uint32_t name_offset;    // -> NUL-terminated ASCII in the name blob
    uint32_t chunk_count;
    uint64_t chunk_offset;   // -> chunk_count * DsaaChunk
};

struct DsaaChunk {           // 16 bytes
    uint64_t file_offset;    // where this chunk lands in the rebuilt file
    uint32_t archive_offset; // offset in that archive's decompressed stream
    uint8_t  pad[3];
    uint8_t  archive_index;  // which bundles.NN.nxa
};
```

A chunk's length is the next chunk's `file_offset` minus its own; the last runs
to `file_size`. The name blob follows the record array (records end at
`24 + 8815*24 = 211584`, names start at 211704), then the chunk arrays.

**Deduplication is the whole point of the repack.** Identical chunks are stored
once and referenced from many files, which is why 8815 files totalling 127 GB
fit in 28 GB of archives, and why a file is stitched from dozens of scattered
reads (one 553 KB bundle uses 105 chunks across 14 archives, several of them
4-byte runs shared with other bundles). Sequential extraction therefore thrashes
the block cache; that is inherent to the format, not a tooling problem.

**Verification.** Reconstructed files match their recorded `file_size` exactly
and begin with the correct Stingray magic. The 8815 files resolve to 3254
bundles: 3254 bundle files, 2911 `.gpu_resources`, 2650 `.stream`.

## Stingray bundles — the payload

The rebuilt `<hash16>` files are ordinary Stingray bundles, which is the point:
existing extractors take them as-is.

```
uint32 magic       = 0xF0000011
uint32 type_count
uint32 file_count
uint32 pad
...                                  // 80-byte header total
struct TypeEntry { uint64 type_hash; uint64 count; ... };  // 32 bytes each, at offset 80
```

`type_hash` is **murmur64a (seed 0) of the type name**, Stingray's universal
naming scheme. Confirmed: `murmur64a("texture") == 0xcd4238c6a0c69e32`, and the
per-type counts sum exactly to `file_count`.

Resource types seen in a 20-bundle sample: `material`, `texture`, `unit`,
`physics`, `particles`, `animation`, `prefab`, `bones`, `state_machine`,
`level`, `lua`, `render_config`, `mouse_cursor`, `wwise_bank`, `wwise_dep`,
`wwise_stream`. Two hashes did not match any candidate name and are reported as
raw hex rather than guessed: `0x46bc82aae9ae0565` (common — 296 of 2253
resources) and `0x5ee65304478f8db5`.

The 80-byte per-file entry that follows the type table is **not** decoded here;
`hd2_index.py` only needs the type table. Reading individual resources out of a
bundle is filediver's job.

## `data/game/` — encrypted, not free

`data/game/` holds `game.dll` and 58 `generated_*.dl_bin` files whose names
promise exactly the tuning data a study project would want — `damage_settings`,
`destruction_settings`, `weather_settings`, `sky_settings`, `planet_data`, a
42 MB `entities` — plus `dl_library.dl_typelib`, which in the open-source
[datalibrary](https://github.com/wc-duck/datalibrary) format would describe
every one of their schemas.

They are loose on disk but **not readable**: all 256 byte values occur in the
first 64 KB and the longest ASCII runs are noise. Encrypted, or compressed with
an unidentified scheme. The open-source DL magic is absent. Not pursued.

## Caveats

* Read-only, offline work on a local install. `bin/GameGuard` is anti-cheat —
  don't run extraction while the game is running.
* Assets are Arrowhead's. This is for reading and studying; nothing extracted
  can ship in anything.
* The layout will change again. The DSAR invariants above are the cheapest
  place to check whether it has.
