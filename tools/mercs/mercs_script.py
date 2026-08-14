"""mercs_script.py - unpack Mercenaries' mission scripts to .lua source.

200 scr_ chunks in ASSETS.DSK. They are LZ-compressed with the same codec as
CSEG geometry (mercs_lzss.py), just without the {decSize, compSize} header that
CSEG puts in front of it - the stream is self-terminating, so none is needed.
That is why they look like binary with English words scattered through it if you
read the BODY directly: the words are the literal runs.

198 of the 200 unpack to plain Lua source, 638 KB compressed to 2.03 MB. The
two that do not are not text and are left alone rather than written out with a
misleading .lua extension.

WHAT IS IN THEM. Mission and gameflow logic, with comments and original
formatting intact - the shipped disc has the source, not bytecode. The engine
API surface they call is the interesting part for study: Actor_*, Mission_*,
Faction_*, Traffic_*, Utility_* and so on, which names the whole scripting
interface the designers worked against.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_lzss import decompress          # noqa: E402
from mercs_mesh import children, cstr      # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    ok = binary = 0
    total_in = total_out = 0
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        seen = set()
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'scr_' or h in seen:
                continue
            seen.add(h)
            d = dict(children(blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]))
            name = cstr(d.get('NAME', b'')) or '%08X' % h
            body = d.get('BODY', b'')
            if not body:
                continue
            try:
                out = decompress(body)
            except Exception:
                binary += 1
                continue
            head = out[:4000]
            printable = sum(1 for c in head if 9 <= c < 127) / max(1, len(head))
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in name)
            if printable < 0.95:
                # Not text. Keep it, but do not call it .lua.
                with open(os.path.join(args.out, safe + '.bin'), 'wb') as f:
                    f.write(out)
                binary += 1
                continue
            with open(os.path.join(args.out, safe + '.lua'), 'wb') as f:
                f.write(out)
            ok += 1
            total_in += len(body)
            total_out += len(out)

    print('  %d scripts -> %s' % (ok, args.out))
    print('  %s bytes compressed -> %s bytes of source'
          % (format(total_in, ','), format(total_out, ',')))
    if binary:
        print('  %d chunk(s) were not text, written as .bin' % binary)
    return 0


if __name__ == '__main__':
    sys.exit(main())
