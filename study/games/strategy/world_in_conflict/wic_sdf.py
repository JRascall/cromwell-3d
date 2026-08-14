"""Reader for Massive Entertainment 'RYS' .sdf archives (World in Conflict).

Format transcribed from aluigi's QuickBMS script world_in_conflict.bms (0.3),
re-implemented in Python so we can list and selectively extract without
unpacking 6 GB.

Two container generations:
  type <= 7   XOR-obfuscated recursive directory tree ("EXTRACT1")
  type >  7   single LZMA-compressed table of contents ("EXTRACT2")
"""
import sys, os, struct, zlib, lzma, fnmatch

M32 = 0xFFFFFFFF


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v


def cstr(buf, off):
    end = buf.index(b'\0', off)
    return buf[off:end].decode('latin-1'), end + 1


def lzma_raw(raw, xsize):
    """quickbms comtype lzma: 5-byte props then the stream, no size field.

    decompress() honours max_length and buffers the remainder, so a single
    call silently truncates anything past the first chunk -- loop until the
    declared size is reached.
    """
    props = lzma._decode_filter_properties(lzma.FILTER_LZMA1, raw[:5])
    d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[props])
    out = bytearray()
    chunk = d.decompress(raw[5:], max_length=xsize)
    out += chunk
    while len(out) < xsize and not d.eof:
        chunk = d.decompress(b'', max_length=xsize - len(out))
        if not chunk:
            break
        out += chunk
    return bytes(out)


def inflate(raw, xsize):
    """Big payloads are a *sequence* of independent zlib streams, not one.

    A single zlib.decompress() therefore raises on the trailing bytes, and a
    raw-deflate (-15) fallback happily returns garbage rather than failing --
    which is how a 10 MB texture silently became a 6.8 MB one. Consume stream
    after stream until the declared uncompressed size is reached, and refuse
    to return a short buffer.
    """
    if raw[:1] == b'\x5d':
        return lzma_raw(raw, xsize)
    out = bytearray()
    rest = raw
    while rest and len(out) < xsize:
        d = zlib.decompressobj()
        out += d.decompress(rest)
        out += d.flush()
        if not d.eof:
            break
        rest = d.unused_data
    # Every .dds in the archive decodes 128 bytes short of its declared size,
    # which is exactly the size of a D3D9 DDS header: the packer stores the
    # texel payload headerless and the TOC still records the authored file
    # size. Anything else short is a real decode failure.
    if len(out) == xsize - 128:
        return bytes(out)
    if len(out) != xsize:
        raise ValueError('short inflate: %d of %d' % (len(out), xsize))
    return bytes(out)


class Sdf:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        head = self.f.read(4)
        assert head[:3] == b'RYS', 'not a RYS archive: %r' % head[:3]
        self.type_raw = head[3]
        self.key3 = {0x00: 0x40, 0x40: 0x08,
                     0x80: 0x80, 0xC0: 0x88}.get(self.type_raw & 0xC0, 0)
        self.type = self.type_raw & 0x3F
        if self.type == 0x5:
            self.key1, self.key2 = 0x81E7C3A1, 0x7E183C5A
        elif self.type == 0x7:
            self.key1 = self.key2 = 0x6D3AE31B
            self.archive_size = struct.unpack('<I', self.f.read(4))[0]
        self.entries = []          # (name, offset, size, zsize_or_None)

    # ---- generation 1 -----------------------------------------------------
    def _key1(self):
        return ((~4) ^ self.key1) & M32 if self.type == 0x7 else self.key1

    def _read_dir(self, offset, count, path):
        f = self.f
        f.seek(offset)
        key = self._key1()
        if count < 0:
            count = struct.unpack('<I', f.read(4))[0] ^ key
            key = self._key1()
        size = struct.unpack('<I', f.read(4))[0] ^ key
        blob = bytearray(f.read(size))
        key = ((~size) & M32) ^ self.key2
        x, s2 = 0, size
        while s2 > 3:
            struct.pack_into('<I', blob, x,
                             struct.unpack_from('<I', blob, x)[0] ^ key)
            x += 4
            key = ror(key, key & 0x1F) ^ (s2 & M32)
            s2 -= 4
        for i in range(count):
            p = struct.unpack_from('<I', blob, i * 4)[0]
            e_size, e_off = struct.unpack_from('<II', blob, p)
            is_folder = blob[p + 8]
            name, _ = cstr(blob, p + 9)
            if is_folder:
                self._read_dir(e_off, e_size, path + name + '/')
                f.seek(0)
            else:
                self.entries.append((path + name, e_off, e_size, None))

    # ---- generation 2 -----------------------------------------------------
    def _read_toc(self):
        f = self.f
        f.seek(4)
        toc_off = struct.unpack('<I', f.read(4))[0]
        f.seek(toc_off)
        size, zsize = struct.unpack('<II', f.read(8))
        flags = zsize >> 30
        zsize &= 0x3FFFFFFF
        raw = f.read(zsize if flags else size)
        toc = inflate(raw, size) if flags else raw
        self.toc = toc
        n_files = struct.unpack_from('<I', toc, 0)[0]
        info = []
        for i in range(n_files):
            _dummy, off = struct.unpack_from('<II', toc, 4 + i * 8)
            info.append(off)
        for off in info:
            _dummy, size, zsize, data_off, path_off = struct.unpack_from(
                '<IIIII', toc, off)
            name, _ = cstr(toc, off + 20)
            folder, _ = cstr(toc, path_off)
            self.entries.append(((folder + '/' + name).replace('\\', '/'),
                                 data_off, size, zsize))

    def walk(self):
        if self.type > 7:
            self._read_toc()
        else:
            self._read_dir(4 + (4 if self.type == 0x7 else 0), -1, '')
        return self.entries

    # ---- payload ----------------------------------------------------------
    def read(self, entry):
        _name, off, size, zsize = entry
        self.f.seek(off)
        if zsize is None:
            if self.key3 & 0x80:
                xsize = struct.unpack('<I', self.f.read(4))[0]
                return inflate(self.f.read(size - 4), xsize)
            return self.f.read(size)
        flags = zsize >> 30
        z = zsize & 0x3FFFFFFF
        if flags:
            return inflate(self.f.read(z), size)
        return self.f.read(size)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = [a for a in sys.argv[1:] if a.startswith('--')]
    a = Sdf(args[0])
    ents = a.walk()
    sys.stderr.write('%s: type=0x%02x key3=0x%02x  %d files\n'
                     % (os.path.basename(args[0]), a.type, a.key3, len(ents)))
    listing = next((o.split('=', 1)[1] for o in opts if o.startswith('--list=')), None)
    if listing:
        with open(listing, 'w', encoding='utf-8') as fh:
            for n, o, s, z in ents:
                fh.write('%10d %s\n' % (s, n))
    pat = next((o.split('=', 1)[1] for o in opts if o.startswith('--glob=')), None)
    dest = next((o.split('=', 1)[1] for o in opts if o.startswith('--out=')), None)
    if pat and dest:
        n = 0
        for e in ents:
            if fnmatch.fnmatch(e[0].lower(), pat.lower()):
                p = os.path.join(dest, e[0].lstrip('/'))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                try:
                    open(p, 'wb').write(a.read(e))
                    n += 1
                except Exception as ex:
                    sys.stderr.write('FAIL %s: %s\n' % (e[0], ex))
        sys.stderr.write('extracted %d\n' % n)
    if not listing and not pat:
        for n, o, s, z in ents[:60]:
            print('%10d %s' % (s, n))


if __name__ == '__main__':
    main()
