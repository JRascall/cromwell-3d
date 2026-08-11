# -*- coding: utf-8 -*-
"""Reader for Eugen Systems 'EUG0/CNDF' binary NDF files as shipped in R.U.S.E.

Container:  'EUG0' 0 'CNDF' then a small header; the payload is one zlib stream
whose decompressed length is the uint32 at 0x28.  Inside, a 'TOC0' section
directory sits near the end and names nine sections, each (name, pad, uint64
offset, uint64 size).  Offsets are relative to the file, and the payload we
decompress starts 40 bytes in, so subtract 40.

    OBJE  object records
    TOPO  uint32 offset into OBJE, one per object
    CHNK  chunk table
    CLAS  class names            (length-prefixed strings)
    PROP  property names         (length-prefixed strings)
    STRG  string literals        (length-prefixed strings)
    TRAN  qualified-name tokens  (length-prefixed strings)
    IMPR  import tree            (tran index, parent, children offsets)
    EXPR  export tree            ditto

An object is  classIdx(4)  then property records until 0xABABABAB.  A property
record is propIdx(4) typeIdx(4) then a value whose width depends on the type.

Written for ../study/ruse.md; Python 2, see ruse_edat.py for why.

KNOWN INCOMPLETE: the type table below was recovered by inspection and several
codes (notably 0x14, and some packed 8-byte handles) are still wrong or missing.
objects() therefore stops decoding an object when it meets one and resyncs on the
next 0xABABABAB rather than silently returning garbage.  The four string tables
-- CLAS, PROP, STRG, TRAN -- do not go through the value decoder at all, so class
and property names are trustworthy even where values are not.
"""
import struct
import zlib

TYPE_NAMES = {
    0: 'bool', 1: 'int8', 2: 'int32', 3: 'uint32', 4: 'time64', 5: 'float',
    6: 'double', 7: 'string', 8: 'widestring', 9: 'ref', 0x0B: 'vec3',
    0x0C: 'vec4', 0x0D: 'colour4b', 0x0E: 'int3', 0x0F: 'vec2',
    0x11: 'array', 0x12: 'pair', 0x13: 'guid', 0x14: 'blob',
    0x18: 'zero', 0x19: 'uint8', 0x1A: 'int16', 0x1B: 'uint16',
    0x1C: 'int64', 0x1D: 'uint64', 0x1E: 'hash', 0x1F: 'vec2f',
    0x21: 'localised', 0x22: 'vec2i16', 0x25: 'path',
}


def _strings(buf, trailer=0):
    """Length-prefixed strings.  PROP entries carry a trailing uint32 (the
    owning class index); CLAS, STRG and TRAN do not."""
    out, pos, n = [], 0, len(buf)
    while pos + 4 <= n:
        ln = struct.unpack_from('<I', buf, pos)[0]
        pos += 4
        if ln > n - pos:
            break
        out.append(buf[pos:pos + ln])
        pos += ln + trailer
    return out


class Ndf(object):
    def __init__(self, raw):
        assert raw[:4] == b'EUG0' and raw[8:12] == b'CNDF', 'not a CNDF file'
        # The uint32 at 0x0C flags compression: 0x80 means the payload is one
        # zlib stream from 0x2C, 0 means it is stored raw.  Either way section
        # offsets are file-relative and the payload starts 40 bytes in.
        if raw[0x2C:0x2D] == b'\x78':
            self.buf = zlib.decompressobj().decompress(raw[0x2C:])
        else:
            self.buf = raw[0x28:]
        toc = self.buf.index(b'TOC0')
        count = struct.unpack_from('<I', self.buf, toc + 4)[0]
        self.sections = {}
        for i in range(count):
            base = toc + 8 + i * 24
            name = self.buf[base:base + 4]
            off, size = struct.unpack_from('<QQ', self.buf, base + 8)
            self.sections[name] = (off - 40, size)
        self.classes = _strings(self._sec(b'CLAS'))
        self.props = _strings(self._sec(b'PROP'), trailer=4)
        self.strings = _strings(self._sec(b'STRG'))
        self.trans = _strings(self._sec(b'TRAN'))
        # TOPO is a permutation of object indices (the topological order in
        # which objects must be constructed), not a table of byte offsets --
        # the values are all smaller than the object count.  Objects sit
        # back-to-back in OBJE and are walked sequentially.
        topo = self._sec(b'TOPO')
        self.topo = list(struct.unpack('<%dI' % (len(topo) // 4), topo))

    def _sec(self, name):
        off, size = self.sections[name]
        return self.buf[off:off + size]

    # -- value decoding -----------------------------------------------------
    def _value(self, buf, pos, t):
        """Return (python value, new position). Raises on an unknown type."""
        if t == 0:
            return bool(ord(buf[pos])), pos + 1
        if t in (1, 0x19):
            return ord(buf[pos]), pos + 1
        if t == 2:
            return struct.unpack_from('<i', buf, pos)[0], pos + 4
        if t == 3:
            return struct.unpack_from('<I', buf, pos)[0], pos + 4
        if t == 5:
            return struct.unpack_from('<f', buf, pos)[0], pos + 4
        if t == 6:
            return struct.unpack_from('<d', buf, pos)[0], pos + 8
        if t in (4, 0x1C, 0x1D):
            return struct.unpack_from('<q', buf, pos)[0], pos + 8
        if t in (0x1A, 0x1B, 0x22):
            return struct.unpack_from('<h', buf, pos)[0], pos + 2
        if t == 7:
            i = struct.unpack_from('<I', buf, pos)[0]
            return self.strings[i] if i < len(self.strings) else '<str%d>' % i, pos + 4
        if t == 8:
            ln = struct.unpack_from('<I', buf, pos)[0]
            return buf[pos + 4:pos + 4 + ln].decode('utf-16le', 'replace'), pos + 4 + ln
        if t == 9:
            tag = struct.unpack_from('<I', buf, pos)[0]
            idx = struct.unpack_from('<I', buf, pos + 4)[0]
            kind = {0xAAAAAAAA: 'tran', 0xBBBBBBBB: 'obj'}.get(tag, '%08x' % tag)
            return ('<%s %d>' % (kind, idx)), pos + 8
        if t == 0x0B:
            return struct.unpack_from('<3f', buf, pos), pos + 12
        if t == 0x0C:
            return struct.unpack_from('<4f', buf, pos), pos + 16
        if t == 0x0D:
            return struct.unpack_from('<4B', buf, pos), pos + 4
        if t == 0x0E:
            return struct.unpack_from('<3i', buf, pos), pos + 12
        if t == 0x0F:
            return struct.unpack_from('<2f', buf, pos), pos + 8
        if t == 0x11:
            n = struct.unpack_from('<I', buf, pos)[0]
            pos += 4
            items = []
            for _ in range(n):
                et = struct.unpack_from('<I', buf, pos)[0]
                pos += 4
                v, pos = self._value(buf, pos, et)
                items.append(v)
            return items, pos
        if t == 0x12:
            a_t = struct.unpack_from('<I', buf, pos)[0]
            a, pos = self._value(buf, pos + 4, a_t)
            b_t = struct.unpack_from('<I', buf, pos)[0]
            b, pos = self._value(buf, pos + 4, b_t)
            return (a, b), pos
        if t == 0x13:
            return buf[pos:pos + 16].encode('hex'), pos + 16
        if t == 0x18:
            return None, pos
        if t == 0x1E:
            return buf[pos:pos + 16].encode('hex'), pos + 16
        if t == 0x21:
            return buf[pos:pos + 8].encode('hex'), pos + 8
        if t == 0x25:
            ln = struct.unpack_from('<I', buf, pos)[0]
            return buf[pos + 4:pos + 4 + ln], pos + 4 + ln
        raise ValueError('unknown NDF type 0x%02x at %d' % (t, pos))

    def objects(self):
        obje = self._sec(b'OBJE')
        end = len(obje)
        pos, n = 0, 0
        while pos + 8 <= end:
            cls = struct.unpack_from('<I', obje, pos)[0]
            pos += 4
            props, broken = [], False
            while pos + 4 <= end:
                head = struct.unpack_from('<I', obje, pos)[0]
                if head == 0xABABABAB:
                    pos += 4
                    break
                t = struct.unpack_from('<I', obje, pos + 4)[0]
                pos += 8
                try:
                    v, pos = self._value(obje, pos, t)
                except (ValueError, struct.error) as ex:
                    props.append(('<stopped>', str(ex)))
                    broken = True
                    break
                name = self.props[head] if head < len(self.props) else '<prop%d>' % head
                props.append((name, v))
            yield n, (self.classes[cls] if cls < len(self.classes) else '<cls%d>' % cls), props
            n += 1
            if broken:
                nxt = obje.find(b'\xab\xab\xab\xab', pos)
                if nxt < 0:
                    return
                pos = nxt + 4
