"""
hd2_dsar.py - readers for the Helldivers 2 container formats.

Helldivers 2 ships its assets in two nested layers:

    bundles.NN.nxa  (30 of them)   DSAR container: LZ4-compressed 256 KiB chunks
      -> a flat decompressed byte stream, addressed by offset
    bundles.nxa                    DSAR container holding the DSAA *index*
      -> which named file lives at which offsets in which archives

The named files inside are the ordinary Stingray bundles that every existing
Helldivers 2 extractor already understands - `<hash16>`, `<hash16>.stream`,
`<hash16>.gpu_resources`. Older installs had them loose on disk; this repack
packs them into the .nxa set, which is why stock tools find nothing.

Nothing here converts assets. This layer only rebuilds the loose files, so
filediver / Diver can do the actual mesh and texture work unchanged.

Format notes live in study/games/shooters/helldivers2/helldivers2_formats.md.
"""

import bisect
import os
import struct
from collections import OrderedDict

# The native package is ~200x faster than the fallback and matters a lot: a
# full unpack touches ~28 GB of compressed chunks. The pure-Python decoder is
# kept so the toolchain still runs on a bare interpreter.
try:
    import lz4.block as _lz4

    def lz4_block(src, dsize):
        return _lz4.decompress(src, uncompressed_size=dsize)

    HAVE_NATIVE_LZ4 = True
except ImportError:  # pragma: no cover - exercised only without the package
    HAVE_NATIVE_LZ4 = False

    def lz4_block(src, dsize):
        """Decode one raw LZ4 block (no frame header, size known up front)."""
        dst = bytearray(dsize)
        s = d = 0
        n = len(src)
        while s < n:
            token = src[s]
            s += 1
            ll = token >> 4
            if ll == 15:
                while True:
                    b = src[s]
                    s += 1
                    ll += b
                    if b != 255:
                        break
            dst[d:d + ll] = src[s:s + ll]
            s += ll
            d += ll
            if s >= n:
                break
            off = src[s] | (src[s + 1] << 8)
            s += 2
            ml = (token & 15) + 4
            if (token & 15) == 15:
                while True:
                    b = src[s]
                    s += 1
                    ml += b
                    if b != 255:
                        break
            r = d - off
            for _ in range(ml):          # may overlap; must stay byte-at-a-time
                dst[d] = dst[r]
                d += 1
                r += 1
        return bytes(dst[:d])


DSAR_MAGIC = b'DSAR'
DSAA_MAGIC = b'DSAA'
BUNDLE_MAGIC = 0xF0000011

STORED = 0        # block method 0: bytes are already raw
LZ4 = 3           # block method 3: raw LZ4 block


class Archive:
    """Random access into one DSAR container's decompressed byte stream.

    The block table is tiny (32 bytes per 256 KiB) so it is read up front;
    blocks themselves are decompressed on demand and cached, because the
    index addresses them in scattered order.
    """

    def __init__(self, path, cache_blocks=24):
        self.path = path
        self.f = open(path, 'rb')
        magic, ver, nblocks, table_end, self.size, _pad, _tag = struct.unpack(
            '<4sIIIII8s', self.f.read(32))
        if magic != DSAR_MAGIC:
            raise ValueError('%s: not a DSAR container (magic %r)' % (path, magic))
        self.version = ver
        self.blocks = []
        raw = self.f.read(nblocks * 32)
        for i in range(nblocks):
            dec_off, comp_off, dsize, csize = struct.unpack_from('<QQII', raw, i * 32)
            self.blocks.append((dec_off, comp_off, dsize, csize, raw[i * 32 + 24]))
        self._starts = [b[0] for b in self.blocks]
        self._cache = OrderedDict()
        self._cache_max = cache_blocks

    def close(self):
        self.f.close()
        self._cache.clear()

    def _block(self, i):
        hit = self._cache.get(i)
        if hit is not None:
            self._cache.move_to_end(i)
            return hit
        _dec_off, comp_off, dsize, csize, method = self.blocks[i]
        self.f.seek(comp_off)
        raw = self.f.read(csize)
        data = raw if method == STORED else lz4_block(raw, dsize)
        self._cache[i] = data
        if len(self._cache) > self._cache_max:
            self._cache.popitem(last=False)
        return data

    def read(self, offset, length):
        """Read `length` bytes at `offset` in the decompressed stream."""
        out = bytearray()
        while length > 0:
            i = bisect.bisect_right(self._starts, offset) - 1
            if i < 0 or i >= len(self.blocks):
                raise ValueError('%s: offset %d outside archive' % (self.path, offset))
            block = self._block(i)
            start = offset - self._starts[i]
            take = min(length, len(block) - start)
            if take <= 0:
                raise ValueError('%s: short block at offset %d' % (self.path, offset))
            out += block[start:start + take]
            offset += take
            length -= take
        return bytes(out)

    def read_all(self):
        """Decompress the whole container. Used for the small index archive."""
        out = bytearray(self.size)
        for dec_off, comp_off, dsize, csize, method in self.blocks:
            self.f.seek(comp_off)
            raw = self.f.read(csize)
            out[dec_off:dec_off + dsize] = raw if method == STORED else lz4_block(raw, dsize)
        return bytes(out)


class Entry:
    """One named file (a bundle, its .stream, or its .gpu_resources)."""

    __slots__ = ('name', 'size', 'chunks')

    def __init__(self, name, size, chunks):
        self.name = name
        self.size = size
        self.chunks = chunks      # list of (file_offset, archive_offset, archive_index)

    @property
    def kind(self):
        for suffix in ('.stream', '.gpu_resources'):
            if self.name.endswith(suffix):
                return suffix[1:]
        return 'bundle'

    @property
    def bundle(self):
        """The base hash, shared by all three files of a bundle."""
        return self.name.split('.', 1)[0]


class Index:
    """The DSAA table of contents, read out of bundles.nxa."""

    def __init__(self, data_dir):
        self.data_dir = data_dir
        idx = Archive(os.path.join(data_dir, 'bundles.nxa'))
        blob = idx.read_all()
        idx.close()

        magic, ver, total, self.archive_count, count, _filler = struct.unpack_from(
            '<4sIIIII', blob, 0)
        if magic != DSAA_MAGIC:
            raise ValueError('bundles.nxa did not decompress to a DSAA index')
        if total != len(blob):
            raise ValueError('DSAA size mismatch: header %d, got %d' % (total, len(blob)))
        self.version = ver

        self.entries = []
        for i in range(count):
            size, name_off, nchunks, chunk_off = struct.unpack_from('<QIIQ', blob, 24 + i * 24)
            end = blob.index(b'\0', name_off)
            name = blob[name_off:end].decode('ascii')
            chunks = []
            for j in range(nchunks):
                o = chunk_off + j * 16
                file_off, arc_off = struct.unpack_from('<QI', blob, o)
                chunks.append((file_off, arc_off, blob[o + 15]))
            self.entries.append(Entry(name, size, chunks))

        self._archives = {}

    # -- archive pool ------------------------------------------------------
    def archive(self, i):
        a = self._archives.get(i)
        if a is None:
            a = Archive(os.path.join(self.data_dir, 'bundles.%02d.nxa' % i))
            self._archives[i] = a
        return a

    def close(self):
        for a in self._archives.values():
            a.close()
        self._archives.clear()

    # -- extraction --------------------------------------------------------
    def extract(self, entry):
        """Rebuild one named file. Chunks are deduplicated across archives, so
        a single file is generally stitched from dozens of scattered reads."""
        out = bytearray(entry.size)
        chunks = entry.chunks
        for j, (file_off, arc_off, arc_i) in enumerate(chunks):
            end = chunks[j + 1][0] if j + 1 < len(chunks) else entry.size
            out[file_off:end] = self.archive(arc_i).read(arc_off, end - file_off)
        return bytes(out)

    def bundles(self):
        """Entry lists grouped by bundle hash, in index order."""
        groups = OrderedDict()
        for e in self.entries:
            groups.setdefault(e.bundle, []).append(e)
        return groups


def wrap_dsar(payload, chunk=0x40000):
    """Re-wrap raw bytes as a DSAR container using stored (method 0) blocks.

    Method 0 means "these bytes are already raw", so this produces a valid,
    uncompressed DSAR file. Only needed if a downstream tool insists on the
    DSAR envelope rather than a bare Stingray bundle.
    """
    n = (len(payload) + chunk - 1) // chunk or 1
    table_end = 32 + n * 32
    head = struct.pack('<4sIIIII8s', DSAR_MAGIC, 0x00010003, n, table_end,
                       len(payload), 0, b'PADDING*')
    table = bytearray()
    comp_off = table_end
    for i in range(n):
        dec_off = i * chunk
        size = min(chunk, len(payload) - dec_off)
        table += struct.pack('<QQII', dec_off, comp_off, size, size)
        table += bytes([STORED, 2]) + b'\x54\x55\x55\x55\x55\x55'
        comp_off += size
    return head + bytes(table) + payload


def read_bundle_types(data):
    """(type_hash, count) pairs from a raw Stingray bundle's type table.

    Header is 80 bytes; each type entry is 32 bytes beginning with the murmur64a
    hash of the type name and its file count. The counts sum to the header's
    file count, which is what makes this layout verifiable.
    """
    magic, ntypes, nfiles = struct.unpack_from('<III', data, 0)
    if magic != BUNDLE_MAGIC:
        return None, None, None
    types = []
    for i in range(ntypes):
        thash, tcount = struct.unpack_from('<QQ', data, 80 + i * 32)
        types.append((thash, tcount))
    return ntypes, nfiles, types
