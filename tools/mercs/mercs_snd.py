"""mercs_snd.py - split Mercenaries' PS2 sample banks into playable files.

Mercenaries ships two unrelated audio systems and it is worth knowing which
you are looking at before reaching for a tool:

  .MIB + .MIH   Sony's MultiStream pair - the streaming music and ambience.
                Standard, and vgmstream reads the pair directly with no help
                from us. Nothing in this file touches them; the PowerShell
                driver hands them to vgmstream as they are.

  .MSB + .MSH   RedEngine's own sample bank - all the SFX and the 177 MB of
                voice. vgmstream does not know it, which is what this file is
                for. The .MSB is nothing but PS-ADPCM samples laid end to end;
                every piece of structure lives in the tiny .MSH beside it.

THE .MSH LAYOUT, and how it was confirmed rather than guessed:

    u32 headerSize        128 for a 5-entry bank, i.e. the table is padded out
    u32 unknown           0x24 in every bank seen; version or alignment
    u32 count             number of samples
    count * {
        u32 size          bytes of ADPCM
        u32 id            a global sample id, sequential within a bank
        u32 offset        byte offset into the .MSB
        u32 sampleRate    real rates - 4000, 11025, 18000, 22050, 44100
    }

The check that makes this a fact and not a story: for AS_BB the five entry
sizes sum to 79,408, which is the .MSB length to the byte, and the offsets
form an exact partition of the file with no gaps. A wrong field assignment
does not add up like that. `--verify` re-runs that check on every bank.

WHY .VAG AND NOT STRAIGHT TO WAV. Each sample is written as a Sony .VAG,
which is a 48-byte big-endian header wrapped round the ADPCM that was already
there - a lossless repackage, not a decode. That keeps this script free of an
ADPCM decoder (a second implementation to get subtly wrong) and hands the
decode to vgmstream, which does it properly. The driver then converts the
.VAG files to .WAV in one sweep.

Read-only research: Pandemic's audio is not ours to redistribute and none of
it is committed or shipped, the same rule this repo already applies to XCOM,
Siege, UNIGINE and SimCity. See study/games/ for what is being learned.
"""
import argparse
import os
import struct
import sys

VAG_HEADER = 48


def read_msh(path):
    """Parse a .MSH into a list of (size, id, offset, rate)."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 12:
        raise ValueError('%s is too small to be a .MSH' % path)
    header_size, unknown, count = struct.unpack_from('<III', data, 0)
    entries = []
    for i in range(count):
        at = 12 + i * 16
        if at + 16 > len(data):
            raise ValueError('%s: entry %d runs past the end of the header'
                             % (path, i))
        entries.append(struct.unpack_from('<IIII', data, at))
    return header_size, unknown, entries


def vag_header(size, rate, name):
    """Sony .VAG - big-endian, 48 bytes, then the ADPCM verbatim."""
    stem = os.path.basename(name).encode('ascii', 'replace')[:15]
    return (b'VAGp'
            + struct.pack('>I', 0x20)
            + b'\0' * 4
            + struct.pack('>I', size)
            + struct.pack('>I', rate)
            + b'\0' * 12
            + stem.ljust(16, b'\0'))


def verify(msh_path, msb_path):
    """The partition check: sizes must tile the .MSB exactly."""
    _, _, entries = read_msh(msh_path)
    msb = os.path.getsize(msb_path)
    total = sum(e[0] for e in entries)
    covered = [(e[2], e[2] + e[0]) for e in sorted(entries, key=lambda e: e[2])]
    gaps = []
    cursor = 0
    for start, end in covered:
        if start != cursor:
            gaps.append((cursor, start))
        cursor = max(cursor, end)
    return {
        'entries': len(entries),
        'sum': total,
        'msb': msb,
        'exact': total == msb,
        'gaps': gaps,
        'overrun': cursor > msb,
    }


def split(msh_path, msb_path, out_dir, prefix):
    _, _, entries = read_msh(msh_path)
    os.makedirs(out_dir, exist_ok=True)
    written = 0
    with open(msb_path, 'rb') as f:
        for i, (size, sid, offset, rate) in enumerate(entries):
            if size == 0:
                continue
            f.seek(offset)
            pcm = f.read(size)
            if len(pcm) != size:
                sys.stderr.write('  short read on %s entry %d\n' % (prefix, i))
                continue
            # Rate 0 shows up on a few padding entries; 22050 is the bank
            # default per RSM.CFG and is the safe thing to stamp.
            r = rate if rate else 22050
            name = '%s_%04d_id%d_%dhz.vag' % (prefix, i, sid, r)
            with open(os.path.join(out_dir, name), 'wb') as o:
                o.write(vag_header(size, r, name))
                o.write(pcm)
            written += 1
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('msh', help='path to a .MSH (the .MSB is found beside it)')
    ap.add_argument('--out', help='output directory for .vag files')
    ap.add_argument('--verify', action='store_true',
                    help='check the sizes tile the .MSB exactly, write nothing')
    args = ap.parse_args()

    msb = os.path.splitext(args.msh)[0] + '.MSB'
    if not os.path.exists(msb):
        msb = os.path.splitext(args.msh)[0] + '.msb'
    if not os.path.exists(msb):
        sys.stderr.write('no .MSB beside %s\n' % args.msh)
        return 2

    if args.verify:
        r = verify(args.msh, msb)
        print('%-14s %5d entries  sum=%-12s msb=%-12s %s%s'
              % (os.path.basename(args.msh), r['entries'],
                 format(r['sum'], ','), format(r['msb'], ','),
                 'EXACT' if r['exact'] else 'MISMATCH',
                 '  gaps=%d' % len(r['gaps']) if r['gaps'] else ''))
        return 0 if r['exact'] else 1

    out = args.out or os.path.splitext(args.msh)[0] + '_vag'
    prefix = os.path.splitext(os.path.basename(args.msh))[0].lower()
    n = split(args.msh, msb, out, prefix)
    print('%-14s -> %d samples in %s' % (os.path.basename(args.msh), n, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
