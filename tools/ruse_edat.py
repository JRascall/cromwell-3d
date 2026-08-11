# -*- coding: utf-8 -*-
"""Minimal reader for Eugen Systems 'edat' packages (version 1 = R.U.S.E.,
version 2 = Wargame and later).

Written for ../study/ruse.md.  Python 2 -- the sibling ruse_python.py needs
Python 2's marshal to read the game's code objects, so all three readers stay
on 2.7 rather than splitting the toolchain.  Usage:

    python ruse_edat.py <path to some .dat>          # list the contents

Header layout was recovered by arithmetic against R.U.S.E.'s IA_Common.dat:
    0x00 magic 'edat'
    0x04 uint32 version
    0x08 16 bytes dictionary MD5 (zero in R.U.S.E.)
    0x18 1 byte
    0x19 uint32 dict offset
    0x1D uint32 dict length
    0x21 uint32 file-content offset   (== dict offset + dict length)
    0x25 uint32 file-content length
Dictionary walk follows enohka/moddingSuite's EdataManager.
"""
import struct
import sys


def _cstr(buf, pos):
    end = buf.index(b'\x00', pos)
    return buf[pos:end].decode('latin-1'), end + 1


class Entry(object):
    def __init__(self, path, offset, size):
        self.path = path
        self.offset = offset
        self.size = size

    def __repr__(self):
        return '<%s %d bytes @%d>' % (self.path, self.size, self.offset)


class Edat(object):
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as fh:
            head = fh.read(0x40)
            assert head[:4] == b'edat', 'not an edat package'
            self.version = struct.unpack_from('<I', head, 4)[0]
            self.dict_offset = struct.unpack_from('<I', head, 0x19)[0]
            self.dict_length = struct.unpack_from('<I', head, 0x1D)[0]
            self.file_offset = struct.unpack_from('<I', head, 0x21)[0]
            self.file_length = struct.unpack_from('<I', head, 0x25)[0]
            fh.seek(self.dict_offset)
            self.dict_buf = fh.read(self.dict_length)
        self.entries = self._walk()

    def _walk(self):
        buf = self.dict_buf
        pos = 0
        end = len(buf)
        dirs = []
        endings = []
        out = []
        while pos < end - 8:
            group = struct.unpack_from('<i', buf, pos)[0]
            pos += 4
            if group == 0:
                entry_size = struct.unpack_from('<i', buf, pos)[0]
                pos += 4
                if self.version == 1:
                    off, size = struct.unpack_from('<II', buf, pos)
                    pos += 8
                    pos += 1                       # unused byte, not a checksum
                else:
                    off, size = struct.unpack_from('<qq', buf, pos)
                    pos += 16
                    pos += 16                      # MD5
                name, pos = _cstr(buf, pos)
                pos += self._pad(name, is_dir=False)
                out.append(Entry(''.join(dirs) + name, off, size))
                while endings and pos == endings[-1]:
                    dirs.pop()
                    endings.pop()
            elif group > 0:
                entry_size = struct.unpack_from('<i', buf, pos)[0]
                pos += 4
                if entry_size != 0:
                    endings.append(entry_size + pos - 8)
                elif endings:
                    endings.append(endings[-1])
                name, pos = _cstr(buf, pos)
                pos += self._pad(name, is_dir=True)
                dirs.append(name)
            else:
                break
        return out

    def _pad(self, name, is_dir):
        """Records are 2-byte aligned, but the two versions disagree on which
        parity gets the filler byte -- and getting it wrong desyncs the walk a
        few entries in rather than failing, so it reads as a short archive.

        v1 (R.U.S.E.):  files pad on an odd name length, directories on even.
        v2 (Wargame on): both pad on an even name length.
        """
        if self.version == 1:
            return 1 if (len(name) % 2 == (0 if is_dir else 1)) else 0
        return 1 if len(name) % 2 == 0 else 0

    def read(self, entry):
        with open(self.path, 'rb') as fh:
            fh.seek(self.file_offset + entry.offset)
            return fh.read(entry.size)


if __name__ == '__main__':
    pkg = Edat(sys.argv[1])
    print('version=%d  files=%d  dict=%d+%d  content=%d+%d'
          % (pkg.version, len(pkg.entries), pkg.dict_offset, pkg.dict_length,
             pkg.file_offset, pkg.file_length))
    for e in pkg.entries:
        print('%10d  %s' % (e.size, e.path))
