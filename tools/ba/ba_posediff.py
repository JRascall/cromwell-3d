"""Compare the frame-0 pose of two baked clips, bone by bone.

The weighted-sum fingerprint was too blunt to answer "did the retarget apply
the clip's stance". This measures the actual angle between each bone's
frame-0 rotation in clip A and clip B, which is the question.
"""
import math, sys
from ba_track import load


def quat_angle(a, b):
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return 2 * math.degrees(math.acos(min(1.0, dot)))


a = load(sys.argv[1])
b = load(sys.argv[2])
assert a['bones'] == b['bones'], "different skeletons"
nb = len(a['bones'])

diffs = []
for i in range(nb):
    qa = a['vals'][i * 10 + 3:i * 10 + 7]
    qb = b['vals'][i * 10 + 3:i * 10 + 7]
    diffs.append((quat_angle(qa, qb), a['bones'][i] or '(animator)'))

diffs.sort(reverse=True)
mean = sum(d for d, _ in diffs) / nb
print(f"{a['name']} vs {b['name']}: frame-0 pose difference")
print(f"  mean {mean:.1f} deg over {nb} bones, max {diffs[0][0]:.1f} deg")
for d, n in diffs[:8]:
    print(f"    {d:7.1f}  {n}")
