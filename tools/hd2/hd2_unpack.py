"""
hd2_unpack.py - rebuild Helldivers 2's loose bundle tree out of the .nxa repack.

Current installs pack every bundle into 30 bundles.NN.nxa archives. Every
existing extractor (filediver, Diver, Hellextractor) expects thousands of loose
`<hash16>` / `.stream` / `.gpu_resources` files in data/ instead, and finds
almost nothing here. This writes that tree back out, byte for byte.

    py -3 tools/hd2/hd2_unpack.py --list
    py -3 tools/hd2/hd2_unpack.py --out hd2_extracted/data
    py -3 tools/hd2/hd2_unpack.py --out hd2_extracted/data --filter 000d250a
    py -3 tools/hd2/hd2_unpack.py --out hd2_extracted/data --slice 0 --of 6

RESUMABLE: a file already present at its exact expected size is skipped, so an
interrupted run costs nothing. Delete a file to redo it.

Slicing is by bundle, and a bundle owns its output files, so N workers over
disjoint slices never contend. They do all read all 30 archives, so the win is
CPU-bound decompression, not I/O.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2_dsar import Index, wrap_dsar, HAVE_NATIVE_LZ4  # noqa: E402

DEFAULT_DATA = r'E:\SteamLibrary\steamapps\common\Helldivers 2\data'


def human(n):
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024 or unit == 'TB':
            return '%.1f %s' % (n, unit)
        n /= 1024.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=DEFAULT_DATA, help='Helldivers 2 data/ directory')
    ap.add_argument('--out', help='where to write the loose bundle tree')
    ap.add_argument('--filter', default='', help='only bundles whose hash starts with this')
    ap.add_argument('--kind', default='all', choices=['all', 'bundle', 'stream', 'gpu_resources'],
                    help='restrict to one file kind (bundle = the small index file)')
    ap.add_argument('--limit', type=int, default=0, help='stop after N bundles')
    ap.add_argument('--slice', type=int, default=0)
    ap.add_argument('--of', type=int, default=1)
    ap.add_argument('--wrap-dsar', action='store_true',
                    help='emit DSAR-wrapped (stored) files instead of raw bundles')
    ap.add_argument('--list', action='store_true', help='print the table of contents and exit')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    if not os.path.isdir(args.data):
        sys.exit('no such data directory: %s' % args.data)

    idx = Index(args.data)
    groups = idx.bundles()
    names = [b for b in groups if b.startswith(args.filter)]
    if args.limit:
        names = names[:args.limit]

    total_bytes = sum(e.size for b in names for e in groups[b])
    if args.list:
        print('%d archives, %d files in %d bundles, %s logical'
              % (idx.archive_count, len(idx.entries), len(groups), human(total_bytes)))
        for b in names[:40]:
            parts = ' '.join('%s=%s' % (e.kind, human(e.size)) for e in groups[b])
            print('  %-18s %s' % (b, parts))
        if len(names) > 40:
            print('  ... %d more' % (len(names) - 40))
        idx.close()
        return

    if not args.out:
        sys.exit('--out is required (or use --list)')

    if args.of > 1:
        names = names[args.slice::args.of]
    os.makedirs(args.out, exist_ok=True)

    if not args.quiet:
        if not HAVE_NATIVE_LZ4:
            print('WARNING: lz4 package not found, using the slow pure-Python decoder.')
            print('         py -3 -m pip install lz4')
        print('Unpacking %d bundles (%s logical) -> %s'
              % (len(names), human(total_bytes), args.out))

    started = time.time()
    written = skipped = failed = 0
    wrote_bytes = 0

    for n, bundle in enumerate(names, 1):
        for entry in groups[bundle]:
            if args.kind != 'all' and entry.kind != args.kind:
                continue
            dest = os.path.join(args.out, entry.name)
            # Resume check. Size is the cheap discriminator; a torn file from a
            # kill mid-write is caught because we stage and rename.
            if not args.wrap_dsar and os.path.exists(dest) \
                    and os.path.getsize(dest) == entry.size:
                skipped += 1
                continue
            try:
                data = idx.extract(entry)
                if len(data) != entry.size:
                    raise ValueError('got %d bytes, index says %d' % (len(data), entry.size))
                if args.wrap_dsar:
                    data = wrap_dsar(data)
                staging = dest + '.partial'
                with open(staging, 'wb') as f:
                    f.write(data)
                os.replace(staging, dest)
                written += 1
                wrote_bytes += len(data)
            except Exception as exc:                     # noqa: BLE001
                failed += 1
                print('  FAILED %s: %s' % (entry.name, exc), file=sys.stderr)

        if not args.quiet and (n % 25 == 0 or n == len(names)):
            elapsed = time.time() - started
            rate = wrote_bytes / elapsed if elapsed else 0
            pct = 100.0 * n / len(names)
            sys.stdout.write('\r  %5.1f%%  %d/%d bundles  %s written  %s/s   '
                             % (pct, n, len(names), human(wrote_bytes), human(rate)))
            sys.stdout.flush()

    idx.close()
    if not args.quiet:
        took = time.time() - started
        print('\nWrote %d files (%s), skipped %d, failed %d in %d:%02d'
              % (written, human(wrote_bytes), skipped, failed, took // 60, took % 60))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
