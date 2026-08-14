"""Reader for the .batrk files unity/BaBake.cs writes, plus a sanity report.

    py -3 tools/ba/ba_track.py D:/ba_extracted/anim/US_Ranger/Stand_run.batrk

A track file is one clip baked onto one rig: bone paths, then local
position/rotation/scale for every bone at every frame. Bone 0 is the Animator's
own transform and carries the root motion.

The string fields use .NET BinaryWriter's format: a 7-bit-encoded length prefix
followed by UTF-8 bytes. That is the one place a naive reader goes wrong.

WHY THE REPORT EXISTS, and why it is not decoration. Every failure in this
pipeline so far produced a *well-formed* file: correct bone names, correct
frame counts, correct rates, and no error anywhere - just numbers that never
changed, or a stance that never applied. Sampling the wrong GameObject gives
you 47 perfectly named bones holding their rest pose. Dropping root motion
gives you a prone soldier at standing height. Neither throws. So the check is
never "did it run" - it is "do the numbers move, and do two clips that should
differ actually differ". That is what these two numbers answer:

  bones rotating >1deg   - is there motion at all
  frame-0 fingerprint    - did the clip's stance apply
                           (use ba_posediff.py for the real comparison)
"""
import struct, sys, math


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from('<f', self.d, self.p)[0]
        self.p += 4
        return v

    def s(self):
        n = 0
        shift = 0
        while True:
            b = self.d[self.p]
            self.p += 1
            n |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
            shift += 7
        return self.raw(n).decode('utf-8')


def load(path):
    r = Reader(open(path, 'rb').read())
    magic = r.raw(4)
    assert magic == b'BATK', magic
    ver = r.i32()
    name = r.s()
    fps = r.f32()
    frames = r.i32()
    human = r.i32()
    nbones = r.i32()
    paths = [r.s() for _ in range(nbones)]
    vals = struct.unpack_from('<%df' % (nbones * frames * 10), r.d, r.p)
    return dict(name=name, fps=fps, frames=frames, human=bool(human),
                bones=paths, vals=vals)


if __name__ == '__main__':
    for path in sys.argv[1:]:
        c = load(path)
        nb, nf = len(c['bones']), c['frames']
        print(f"{c['name']:<20} human={c['human']} fps={c['fps']:.0f} frames={nf} bones={nb}")
        # How much does each bone actually move across the clip?
        moved = 0
        maxrot = 0.0
        rootpath = None
        for b in range(nb):
            q0 = c['vals'][(0 * nb + b) * 10 + 3:(0 * nb + b) * 10 + 7]
            worst = 0.0
            for f in range(1, nf):
                q = c['vals'][(f * nb + b) * 10 + 3:(f * nb + b) * 10 + 7]
                dot = abs(sum(a * bb for a, bb in zip(q0, q)))
                worst = max(worst, 2 * math.degrees(math.acos(min(1.0, dot))))
            if worst > 1.0:
                moved += 1
            maxrot = max(maxrot, worst)
        # root translation range
        xs = [c['vals'][(f * nb + 0) * 10 + 0] for f in range(nf)]
        ys = [c['vals'][(f * nb + 0) * 10 + 1] for f in range(nf)]
        zs = [c['vals'][(f * nb + 0) * 10 + 2] for f in range(nf)]
        print(f"    bones rotating >1deg: {moved}/{nb}   max swing {maxrot:.1f} deg")
        print(f"    root[{c['bones'][0] or '(animator)'}] x {min(xs):+.3f}..{max(xs):+.3f}  "
              f"y {min(ys):+.3f}..{max(ys):+.3f}  z {min(zs):+.3f}..{max(zs):+.3f}")
        # Frame-0 pose fingerprint. Two clips that retarget to genuinely
        # different stances (prone vs standing) must differ here; if every clip
        # has the same fingerprint, the retarget is not being applied at all
        # and only the per-frame wobble is real.
        fp = sum(abs(c['vals'][(0 * nb + b) * 10 + 3 + k]) * (b + 1) * (k + 1)
                 for b in range(nb) for k in range(4))
        print(f"    frame-0 pose fingerprint: {fp:.4f}")
        print(f"    sample bones: {[p or '(animator)' for p in c['bones'][:4]]}")
