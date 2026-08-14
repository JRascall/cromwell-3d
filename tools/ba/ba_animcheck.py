"""Sweep a whole bake and report how much of it actually carries motion.

    py -3 tools/ba/ba_animcheck.py D:/ba_extracted/anim

WHY A SWEEP AND NOT A SPOT CHECK. Every way this pipeline has failed produced
well-formed files - right bone names, right frame counts, no errors - holding a
rest pose. A spot check on one rig proves that rig. It does not prove the other
421, and the failure mode is invisible to file counts, sizes and exit codes:
4,890 empty clips weigh the same as 4,890 good ones.

So this reads every track, measures the largest rotation any bone makes over
the clip, and reports the distribution. What you want to see is a small,
*explicable* tail of near-static clips - poses, folded states, held idles - and
nothing else. A large static population means a silent regression.

Static is not automatically wrong. `Prone_idle` legitimately moves 3 bones by
2.6 degrees, and several hundred clips here are single-pose states with names
like `FoldedState` and `blank`. That is why this prints the names of the static
ones rather than just a count: the question is never "are any static" but "are
the static ones the ones that should be".
"""
import math
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ba_track import Reader

try:
    import numpy as np
except ImportError:
    np = None


def scan(path):
    """Header plus the largest per-bone rotation over the clip, in degrees."""
    r = Reader(open(path, 'rb').read())
    if r.raw(4) != b'BATK':
        return None
    r.i32()
    name = r.s()
    fps = r.f32()
    frames = r.i32()
    human = bool(r.i32())
    nbones = r.i32()
    for _ in range(nbones):
        r.s()

    n = nbones * frames * 10
    if np is not None:
        vals = np.frombuffer(r.d, dtype='<f4', count=n, offset=r.p)
        q = vals.reshape(frames, nbones, 10)[:, :, 3:7]
        # Angle between each frame's rotation and frame 0, per bone. abs() on
        # the dot because q and -q are the same rotation - without it every
        # sign flip reads as a 180 degree swing.
        dot = np.abs(np.einsum('fbk,bk->fb', q, q[0]))
        ang = 2 * np.degrees(np.arccos(np.clip(dot, -1.0, 1.0)))
        worst = float(ang.max()) if ang.size else 0.0
        moving = int((ang.max(axis=0) > 1.0).sum())
    else:
        vals = struct.unpack_from('<%df' % n, r.d, r.p)
        worst, moving = 0.0, 0
        for b in range(nbones):
            q0 = vals[b * 10 + 3:b * 10 + 7]
            best = 0.0
            for f in range(1, frames):
                o = (f * nbones + b) * 10 + 3
                dot = abs(sum(x * y for x, y in zip(vals[o:o + 4], q0)))
                best = max(best, 2 * math.degrees(math.acos(min(1.0, dot))))
            if best > 1.0:
                moving += 1
            worst = max(worst, best)
    return name, fps, frames, human, nbones, moving, worst


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'D:/ba_extracted/anim'
    tracks = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.endswith('.batrk'):
                tracks.append(os.path.join(dirpath, f))
    print(f"{len(tracks)} tracks under {root}")
    if np is None:
        print("(numpy not installed - this will be slow)")

    buckets = Counter()
    static = []
    human = moving_total = 0
    for i, t in enumerate(tracks):
        res = scan(t)
        if res is None:
            buckets['unreadable'] += 1
            continue
        name, fps, frames, is_human, nbones, mov, worst = res
        human += is_human
        if worst < 1.0:
            buckets['static (<1 deg)'] += 1
            static.append((os.path.basename(os.path.dirname(t)), name))
        elif worst < 15:
            buckets['subtle (1-15 deg)'] += 1
        elif worst < 60:
            buckets['moderate (15-60 deg)'] += 1
        else:
            buckets['large (>60 deg)'] += 1
            moving_total += 1
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{len(tracks)}")

    print(f"\nhumanoid tracks: {human}   generic: {len(tracks) - human}")
    for k in ['large (>60 deg)', 'moderate (15-60 deg)', 'subtle (1-15 deg)',
              'static (<1 deg)', 'unreadable']:
        if buckets[k]:
            print(f"  {k:<22} {buckets[k]:5}  ({100.0 * buckets[k] / len(tracks):.1f}%)")

    if static:
        print(f"\nstatic clip names, most common first "
              f"(expect poses and held states, not movement verbs):")
        for (clip, n) in Counter(c for _, c in static).most_common(20):
            print(f"  {n:4}  {clip}")


if __name__ == '__main__':
    main()
