# -*- coding: utf-8 -*-
"""Read R.U.S.E.'s '.xyz' files: an 'XYZ0' header, the uncompressed length at
offset 6, a hash, then a zlib stream holding a marshalled Python 2 code object.
Python 2.7's marshal reads Eugen's 2.5-era objects unchanged.

Prints module-level constant assignments (the enum/define modules are almost
entirely these) plus every class and function the module defines.

Written for ../study/games/strategy/ruse.md.  Usage:

    python ruse_python.py <some .ipk or IA_Common.dat> <path suffix to match>

Note that .ipk files are themselves edat packages, so they can be pulled out of
ZZ_Win.dat with ruse_edat.py first and then opened directly.
"""
import dis
import marshal
import struct
import sys
import zlib

from ruse_edat import Edat


def load(data):
    return marshal.loads(zlib.decompress(data[data.index(b'\x78\xda'):]))


def consts(code, indent=''):
    """Walk the bytecode pairing LOAD_CONST with the STORE_NAME after it."""
    out = []
    last = None
    i, n = 0, len(code.co_code)
    while i < n:
        op = ord(code.co_code[i])
        arg = None
        if op >= dis.HAVE_ARGUMENT:
            arg = ord(code.co_code[i + 1]) + (ord(code.co_code[i + 2]) << 8)
            i += 3
        else:
            i += 1
        name = dis.opname[op]
        if name == 'LOAD_CONST':
            last = code.co_consts[arg]
        elif name in ('STORE_NAME', 'STORE_GLOBAL'):
            if not isinstance(last, type(code)):
                out.append((code.co_names[arg], last))
            last = None
        elif name.startswith('LOAD') or name.startswith('BUILD'):
            pass
        else:
            last = None
    return out


def walk(code, depth=0):
    pad = '    ' * depth
    for name, value in consts(code):
        print('%s%-44s = %r' % (pad, name, value))
    for c in code.co_consts:
        if isinstance(c, type(code)) and c.co_name != '<module>':
            args = ', '.join(c.co_varnames[:c.co_argcount])
            print('%sdef/class %s(%s)' % (pad, c.co_name, args))
            if depth < 1:
                walk(c, depth + 1)


if __name__ == '__main__':
    pkg, suffix = sys.argv[1], sys.argv[2]
    p = Edat(pkg)
    for e in p.entries:
        if not e.path.replace('\\', '/').endswith(suffix):
            continue
        code = load(p.read(e))
        print('=' * 78)
        print('# %s' % code.co_filename)
        walk(code)
