"""mercs_elf.py - pull what the PS2 executable still says about itself.

There is no engine source on the disc; SLUS_209.32 is a compiled binary and its
symbol table was stripped, so there are no function names. Three things did
survive, and all three are worth having for study.

VU MICROCODE. The PS2's Graphics Synthesizer is fixed function - there are no
programmable pixel shaders anywhere in this game, and no shader chunk in any
archive. The vertex-side equivalent is VU1 microcode, and it is not hidden: the
ELF keeps it in named sections. `.vutext` is 18,912 bytes, and a dozen
`.DVP.overlay.*` sections hold the paged-in programs. That is the whole of the
game's programmable graphics work, and this dumps each section verbatim.

SOURCE TREE. Assert and log strings carry original paths and line numbers, so
104 source file names are recoverable - RsTrafficManager.cpp, ps2RedTerrain.cpp,
RedWaterOcean.cpp, RsHavokObstacleManager.cpp and so on. Not source, but an
accurate map of how the engine was divided up, and the line numbers say how big
those files were.

STRINGS. Everything printable, with the file:line references separated out.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import collections
import csv
import os
import re
import struct
import sys

SRC_RE = re.compile(r'([A-Za-z0-9_./\\-]+\.(?:cpp|c|h|hpp|inl))')
LINE_RE = re.compile(r'([A-Za-z0-9_./\\-]+\.(?:cpp|c|h|hpp|inl))\s*\[(\d+)\]')


def sections(data):
    e_shoff = struct.unpack_from('<I', data, 32)[0]
    e_shentsize = struct.unpack_from('<H', data, 46)[0]
    e_shnum = struct.unpack_from('<H', data, 48)[0]
    e_shstrndx = struct.unpack_from('<H', data, 50)[0]
    if not (e_shoff and e_shnum):
        return []
    def hdr(i):
        return struct.unpack_from('<10I', data, e_shoff + i * e_shentsize)
    strtab = hdr(e_shstrndx)[4]
    out = []
    for i in range(e_shnum):
        h = hdr(i)
        end = data.index(b'\0', strtab + h[0])
        name = data[strtab + h[0]:end].decode('ascii', 'replace')
        out.append({'name': name, 'type': h[1], 'addr': h[3],
                    'offset': h[4], 'size': h[5]})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elf')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    data = open(args.elf, 'rb').read()

    secs = sections(data)
    with open(os.path.join(args.out, 'sections.csv'), 'w', encoding='utf-8',
              newline='') as f:
        w = csv.DictWriter(f, fieldnames=['name', 'type', 'addr', 'offset', 'size'])
        w.writeheader()
        w.writerows(secs)

    # VU microcode: .vutext, .vudata and every DVP overlay.
    vu_dir = os.path.join(args.out, 'vu')
    os.makedirs(vu_dir, exist_ok=True)
    vu_total = vu_n = 0
    for s in secs:
        if not s['size']:
            continue
        if s['name'].startswith('.vu') or '.DVP.' in s['name']:
            safe = s['name'].strip('.').replace('.', '_') or 'section'
            with open(os.path.join(vu_dir, '%s.bin' % safe), 'wb') as f:
                f.write(data[s['offset']:s['offset'] + s['size']])
            vu_total += s['size']
            vu_n += 1

    strings = [m.decode('ascii') for m in re.findall(rb'[ -~]{4,}', data)]
    with open(os.path.join(args.out, 'strings.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(strings))

    # Source files, with the highest line number seen for each - a rough lower
    # bound on how long the file was.
    files = collections.Counter()
    maxline = collections.defaultdict(int)
    for s in strings:
        for m in SRC_RE.finditer(s):
            files[m.group(1).replace('\\', '/')] += 1
        for m in LINE_RE.finditer(s):
            p = m.group(1).replace('\\', '/')
            maxline[p] = max(maxline[p], int(m.group(2)))
    with open(os.path.join(args.out, 'source_files.csv'), 'w', encoding='utf-8',
              newline='') as f:
        w = csv.writer(f)
        w.writerow(['file', 'references', 'highest_line_seen'])
        for p, n in files.most_common():
            w.writerow([p, n, maxline.get(p, '')])

    print('  %d sections -> sections.csv' % len(secs))
    print('  %d VU microcode section(s), %s bytes -> vu/' % (vu_n, format(vu_total, ',')))
    print('  %s strings -> strings.txt' % format(len(strings), ','))
    print('  %d source files named -> source_files.csv' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
