"""World in Conflict ships its gameplay layer as Python 2.3 bytecode (.pyo).

375 modules: the mission scripting API, the AI package, unit type tables, LOS,
objectives, tactical aid, and per-map scripts up to 240 KB of source each. This
turns them back into readable Python with uncompyle6, which supports 2.3.

    py -3 wic_pyo.py <raw-dir> <out-dir>

Resumable: an existing .py (or .err marker) means that module is skipped, so an
interrupted run costs nothing. Failures are recorded per file rather than
aborting the sweep -- a decompiler that cannot handle one control-flow shape
should not cost you the other 374 modules.
"""
import sys, os, glob, io, time

try:
    from uncompyle6.main import decompile_file
except ImportError:
    sys.exit("uncompyle6 not installed:  py -3 -m pip install uncompyle6")


def main(raw_dir, out_dir):
    src = sorted(glob.glob(os.path.join(raw_dir, '**', '*.pyo'), recursive=True))
    if not src:
        sys.exit('no .pyo under %s' % raw_dir)
    ok = fail = skip = 0
    t0 = time.time()
    for p in src:
        rel = os.path.relpath(p, raw_dir)
        out = os.path.join(out_dir, rel)[:-4] + '.py'
        if os.path.exists(out) or os.path.exists(out + '.err'):
            skip += 1
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        buf = io.StringIO()
        try:
            decompile_file(p, buf)
            with open(out, 'w', encoding='utf-8') as fh:
                fh.write(buf.getvalue())
            ok += 1
        except Exception as e:
            fail += 1
            with open(out + '.err', 'w', encoding='utf-8') as fh:
                fh.write('%s: %s\n' % (type(e).__name__, e))
        if (ok + fail) % 25 == 0:
            print('  %d/%d  (%.0fs)' % (ok + fail + skip, len(src), time.time() - t0))
    print('decompiled %d, failed %d, skipped %d, of %d in %.0fs'
          % (ok, fail, skip, len(src), time.time() - t0))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
