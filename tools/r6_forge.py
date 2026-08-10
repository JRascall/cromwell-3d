"""Rainbow Six: Siege .forge (scimitar) archive reader.

Reads the container layer only: what an archive holds, where each asset sits, and
what type it is. It does not convert meshes or textures -- that is DumpTool's job
(see tools/README.md). This exists because the index has to answer "what is in
these 307 archives" without decoding 44 GB of payloads, and because the container
format needed documenting somewhere executable.

The awkward parts of the format, all verified against the shipped build:

*Two header layouts.* v34 inserts a u32 at 0x1E that v33 does not have, shifting
every field below it by four bytes. Read a v34 archive with the v33 layout and
nothing complains: numEntries comes out plausible and firstTablePosition comes out
as garbage, and the failure surfaces much later as a wild seek. Two invariants pin
it, and are asserted rather than assumed: sizeOfFat == numEntries + 2 (the hash
and descriptor entries), and firstTablePosition == the header size.

*The v34 file allocation table is encrypted.* The 20-byte stride survives (the
table region is exactly sizeOfFat * 20 bytes) but the fields are not plaintext.
No constant XOR or subtraction recovers offsets that land on real containers. It
does not need to be read: containers are 4096-aligned and self-describing, so the
archive is enumerated by scanning for the container magic instead. On every
archive checked this recovers exactly numEntries - 2 assets.

*Two kinds of archive.* Most hold containers. The *_soundmedia archives hold no
containers at all -- just Wwise .wem laid end to end on page boundaries -- and
contain zero occurrences of the container magic. They are detected separately, and
the two hit lists are kept apart because compressed payloads throw up occasional
pages that happen to begin "RIFF....WAVE", and letting one of those displace a
real container costs whole assets.

Needs Python 3 and oo2core_8_win64.dll for Oodle (see OODLE_DLL below); the game
does not ship it loose, it is statically linked into RainbowSix.exe.
"""
import ctypes
import os
import struct

CONTAINER_MAGIC = 0x1015FA9957FBAA37
RIFF = 0x46464952
WAVE = 0x45564157
PAGE = 4096

# Oodle is not redistributable and is not vendored here. RainbowForge3 ships a copy
# and any UE4/UE5 install has a compatible newer one (oo2core_9 decodes oo2core_8
# streams); point this at whichever you have.
OODLE_DLL = os.environ.get(
    'R6_OODLE_DLL',
    r'e:\Game Development\xcom-c\workbench\r6\RainbowForge3\RainbowForge3-master'
    r'\DumpTool\bin\Release\net6.0\oo2core_8_win64.dll')

_oodle = None


def _lib():
    global _oodle
    if _oodle is None:
        if not os.path.exists(OODLE_DLL):
            raise FileNotFoundError(
                f'Oodle not found at {OODLE_DLL}; set R6_OODLE_DLL')
        _oodle = ctypes.WinDLL(OODLE_DLL)
        _oodle.OodleLZ_Decompress.restype = ctypes.c_int
    return _oodle


def oodle_decompress(src, out_len):
    """Decompress one Oodle chunk. Raises if the decoder is short, which is the
    only signal that the chunk table was misread -- Oodle will happily produce
    partial output otherwise."""
    out = ctypes.create_string_buffer(out_len)
    n = _lib().OodleLZ_Decompress(
        ctypes.c_char_p(src), ctypes.c_longlong(len(src)),
        out, ctypes.c_longlong(out_len),
        0, 0, 0,                              # fuzzSafe, checkCRC, verbosity
        None, ctypes.c_longlong(0), None, None,
        None, ctypes.c_longlong(0), 3)        # threadPhase: unthreaded
    if n != out_len:
        raise ValueError(f'oodle produced {n} bytes, expected {out_len}')
    return out.raw[:out_len]


def read_header(path):
    """Parse an archive header, asserting the layout rather than trusting it."""
    with open(path, 'rb') as f:
        hdr = f.read(96)

    if hdr[:9] != b'scimitar\x00':
        return None

    version, = struct.unpack_from('<I', hdr, 9)
    o = 30
    x1e = None
    if version >= 34:
        x1e, = struct.unpack_from('<I', hdr, o)
        o += 4

    num_entries, num_dirs, _unk2, _unk3, _unk3b = struct.unpack_from('<IIIII', hdr, o)
    o += 20
    _free_file, _free_dir, size_of_fat, num_tables = struct.unpack_from('<IIII', hdr, o)
    o += 16
    first_table, = struct.unpack_from('<Q', hdr, o)
    o += 8

    if size_of_fat != num_entries + 2:
        raise ValueError(f'{os.path.basename(path)}: sizeOfFat {size_of_fat} != '
                         f'numEntries+2 {num_entries + 2}; header layout changed')
    if first_table != o:
        raise ValueError(f'{os.path.basename(path)}: firstTablePosition 0x{first_table:x} '
                         f'!= header size 0x{o:x}; header layout changed')

    return dict(path=path, version=version, x1e=x1e, num_entries=num_entries,
                num_dirs=num_dirs, size_of_fat=size_of_fat, num_tables=num_tables,
                first_table=first_table)


def _entries_from_fat(f, h):
    """v33: the table is plaintext. Entries are (offset i64, uid u64, size u32)."""
    f.seek(h['first_table'])
    tbl = f.read(48)
    max_file, = struct.unpack_from('<i', tbl, 0)
    pos_fat, = struct.unpack_from('<q', tbl, 8)

    f.seek(pos_fat)
    raw = f.read(20 * max_file)
    out = []
    for i in range(max_file):
        off, uid, size = struct.unpack_from('<qQI', raw, 20 * i)
        out.append((uid, off, size))
    return out


def _entries_by_scan(f, size, max_entries=None):
    """v34: the table is encrypted, so find the containers directly.

    Walked sequentially in large buffers rather than seeking per page -- a 12 GB
    archive holds three million pages and the seeks cost far more than the read.

    max_entries stops early. That is for surveying only: a partial scan sees the
    front of the archive, which is enough to sample what an archive holds but is
    not a complete listing, so never index with it set.
    """
    container_hits = []
    riff_hits = []
    skip_until = 0

    f.seek(0)
    base = 0
    buf_size = 8 << 20
    while base < size:
        if max_entries is not None and len(container_hits) + len(riff_hits) > max_entries:
            break
        buf = f.read(buf_size)
        if not buf:
            break
        for p in range(0, len(buf) - 12, PAGE):
            at = base + p
            if struct.unpack_from('<Q', buf, p)[0] == CONTAINER_MAGIC:
                container_hits.append(at)
                continue
            if at < skip_until:
                continue
            if (struct.unpack_from('<I', buf, p)[0] == RIFF
                    and struct.unpack_from('<I', buf, p + 8)[0] == WAVE):
                riff_hits.append(at)
                skip_until = at + struct.unpack_from('<I', buf, p + 4)[0] + 8
        base += len(buf)

    hits = container_hits if container_hits else riff_hits
    out = []
    for i, start in enumerate(hits):
        end = hits[i + 1] if i + 1 < len(hits) else size
        # No real uid survives -- the table that held it is encrypted -- so the
        # offset stands in. Unique within the archive and stable across runs.
        out.append((start, start, end - start))
    return out


def entries(path, max_entries=None):
    """[(uid, offset, size)] for every asset in an archive."""
    h = read_header(path)
    if h is None:
        return [], None
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        if h['version'] >= 34:
            return _entries_by_scan(f, size, max_entries), h
        ents = _entries_from_fat(f, h)
        return ents[:max_entries] if max_entries else ents, h


def _read_block(buf, pos, want_bytes=None):
    """Decompress one asset block. want_bytes stops after enough chunks to cover
    that many bytes, which is the difference between reading a 160-byte header and
    inflating a 40 MB mesh."""
    _x, deser = struct.unpack_from('<HH', buf, pos)
    pos += 4 + 1 + 2                       # x, deserializerType, y, z

    if deser == 7:
        raise NotImplementedError('flat data block')
    if deser not in (3, 13, 15):
        raise NotImplementedError(f'deserializer type {deser}')

    num_chunks, _u2 = struct.unpack_from('<HH', buf, pos)
    pos += 4
    chunks = []
    for _ in range(num_chunks):
        unpacked, packed = struct.unpack_from('<II', buf, pos)
        pos += 8
        chunks.append((unpacked, packed))

    out = bytearray()
    for unpacked, packed in chunks:
        pos += 4                            # per-chunk hash
        data = buf[pos:pos + packed]
        pos += packed
        out += oodle_decompress(data, unpacked) if unpacked > packed else data
        if want_bytes is not None and len(out) >= want_bytes:
            break
    return bytes(out), pos


def read_container(buf, want_bytes=None):
    """(meta_block, asset_block). The asset block is the one that carries the
    payload; the meta block in front of it is skipped past without inflating."""
    magic, = struct.unpack_from('<Q', buf, 0)
    if magic != CONTAINER_MAGIC:
        raise ValueError(f'bad container magic 0x{magic:016X}')

    pos = 8
    meta, pos = _read_block(buf, pos, want_bytes=0)
    if pos + 8 > len(buf):
        return meta, None
    pos += 8                                # assetBlockMagic
    asset, pos = _read_block(buf, pos, want_bytes=want_bytes)
    return meta, asset


def asset_meta(payload):
    """FileMetaData at the head of an asset payload -> (fileType, name_blob, uid).

    The name is not decodable on current builds: the FAT's copy is zeroed and this
    one is obfuscated with a key that no longer follows the published derivation.
    The blob is returned raw so callers can record its length, but it is not a
    name. See study/rainbow_six_formats.md.
    """
    if len(payload) < 16:
        return None
    nlen, _var1 = struct.unpack_from('<HH', payload, 0)
    if nlen < 1 or 8 + nlen + 12 > len(payload):
        return None
    blob = payload[8:8 + nlen]
    file_type, = struct.unpack_from('<I', payload, 8 + nlen)
    uid, = struct.unpack_from('<Q', payload, 12 + nlen)
    return file_type, blob, uid
