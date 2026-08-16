"""Extract the real humanoid Avatars from the bundles, as JSON for Unity.

    py -3 tools/ba/ba_avatars.py --bundles <aa/PC> --out D:/ba_extracted/avatars.json

WHY. Broken Arrow's human animations are Unity humanoid clips: muscle values
that mean nothing without an Avatar to retarget them onto. AssetRipper's
reconstruction of that Avatar is not faithful - it produces a single
`AvatarAvatar` shared by all 221 humanoid rigs which resolves 22 of Unity's 55
human bones. Everything it cannot map stays frozen, so a run cycle plays with
the hips, spine, head, both shoulders, one hip, one knee and one foot dead.
That is the whole reason every human animation in the library looked wrong
while the vehicles - generic clips, no Avatar involved - were fine.

Rebuilding the mapping from bone names alone is not enough either: Unity takes
the rig's CURRENT pose as the reference T-pose, and these rigs are not in a
T-pose, so the clips retarget onto a bad reference and come out crouched.

The bundles still have the real thing. An Avatar object carries:

  m_TOS                     hash -> bone name
  m_Human.m_HumanBoneIndex  Unity human bone -> index into the human skeleton
  m_Human.m_Skeleton.m_ID   that skeleton's node hashes
  m_Human.m_SkeletonPose    the reference pose those muscles were authored against
  m_AvatarSkeletonPose      the full skeleton's pose

So the mapping AND the reference pose come out of the game's own data, which is
the same reasoning that made the material join reliable: read the link the
engine used rather than guess it from names.
"""
import argparse
import json
from pathlib import Path

# Unity's INTERNAL human bone order, which is what m_HumanBoneIndex is indexed
# by. It is not the HumanBodyBones enum order: UpperChest sits between Chest and
# Neck here, where the enum appends it at the end as bone 54.
#
# Getting that wrong shifts every bone from index 9 onward by one and the result
# still looks like a plausible mapping - Head->Neck, LeftShoulder->Head,
# LeftUpperArm->RightShoulder - which is worse than an obvious failure, because
# it builds an Avatar that maps the whole body to the wrong bones. The tell is
# the left/right swap it produces: LeftHand landing on RightForeArm.
HUMAN_BONES = [
    'Hips', 'LeftUpperLeg', 'RightUpperLeg', 'LeftLowerLeg', 'RightLowerLeg',
    'LeftFoot', 'RightFoot', 'Spine', 'Chest', 'UpperChest', 'Neck', 'Head',
    'LeftShoulder', 'RightShoulder', 'LeftUpperArm', 'RightUpperArm',
    'LeftLowerArm', 'RightLowerArm', 'LeftHand', 'RightHand',
    'LeftToes', 'RightToes', 'LeftEye', 'RightEye', 'Jaw',
]


def xform(x):
    """A Unity xform node -> {t,q,s} as plain lists."""
    def v(d, keys):
        return [float(d.get(k, 0.0)) for k in keys]
    t = x.get('t') or {}
    q = x.get('q') or {}
    s = x.get('s') or {}
    return {
        't': v(t, 'xyz'),
        'q': v(q, 'xyzw'),
        's': v(s, 'xyz') if s else [1.0, 1.0, 1.0],
    }


def read_avatars(bundle_dir, verbose=True):
    import UnityPy

    out = {}
    files = sorted(p for p in Path(bundle_dir).glob('*.bundle')
                   if 'cutscene' not in p.name.lower())
    for i, f in enumerate(files):
        try:
            env = UnityPy.load(str(f))
        except Exception as e:
            if verbose:
                print(f'  {f.name}: {e}')
            continue
        for obj in env.objects:
            if obj.type.name != 'Avatar':
                continue
            try:
                d = obj.read_typetree()
            except Exception:
                continue
            name = d.get('m_Name') or ''
            av = d.get('m_Avatar') or {}
            human = ((av.get('m_Human') or {}).get('data') or av.get('m_Human') or {})
            if not human:
                continue

            # hash -> bone name
            tos = {}
            for e in (d.get('m_TOS') or []):
                if isinstance(e, (list, tuple)) and len(e) == 2:
                    tos[int(e[0])] = e[1]

            hskel = ((human.get('m_Skeleton') or {}).get('data')
                     or human.get('m_Skeleton') or {})
            ids = hskel.get('m_ID') or []
            idx = human.get('m_HumanBoneIndex') or []

            mapping = {}
            for k, node in enumerate(idx):
                if k >= len(HUMAN_BONES) or node is None or node < 0 or node >= len(ids):
                    continue
                nm = tos.get(int(ids[node]))
                if nm:
                    # TOS stores a path; the bone is its last element.
                    mapping[HUMAN_BONES[k]] = nm.split('/')[-1]

            # The reference pose the muscles were authored against.
            hpose = ((human.get('m_SkeletonPose') or {}).get('data')
                     or human.get('m_SkeletonPose') or {})
            xs = hpose.get('m_X') or []
            pose = {}
            for k, node_hash in enumerate(ids):
                nm = tos.get(int(node_hash))
                if nm and k < len(xs):
                    pose[nm.split('/')[-1]] = xform(xs[k])

            if mapping:
                out[name] = {'human': mapping, 'pose': pose,
                             'scale': human.get('m_Scale', 1.0),
                             'armTwist': human.get('m_ArmTwist', 0.5),
                             'foreArmTwist': human.get('m_ForeArmTwist', 0.5),
                             'upperLegTwist': human.get('m_UpperLegTwist', 0.5),
                             'legTwist': human.get('m_LegTwist', 0.5),
                             'armStretch': human.get('m_ArmStretch', 0.05),
                             'legStretch': human.get('m_LegStretch', 0.05),
                             'feetSpacing': human.get('m_FeetSpacing', 0.0)}
                if verbose:
                    print(f'  {name}: {len(mapping)} human bones, {len(pose)} pose nodes',
                          flush=True)
        if verbose and (i + 1) % 10 == 0:
            print(f'  {i + 1}/{len(files)} bundles, {len(out)} avatars', flush=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', required=True)
    ap.add_argument('--out', default='D:/ba_extracted/avatars.json')
    args = ap.parse_args()
    avatars = read_avatars(args.bundles)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(avatars, f, indent=1)

    # A flat line format beside the JSON, because the consumer is C# inside
    # Unity and JsonUtility cannot express a dictionary. Trivial to parse,
    # trivial to eyeball.
    txt = Path(args.out).with_suffix('.txt')
    with open(txt, 'w', encoding='utf-8') as f:
        for name, a in avatars.items():
            f.write(f'AVATAR\t{name}\n')
            for k in ('scale', 'armTwist', 'foreArmTwist', 'upperLegTwist',
                      'legTwist', 'armStretch', 'legStretch', 'feetSpacing'):
                f.write(f'PARAM\t{k}\t{a.get(k, 0.0)}\n')
            for human, bone in sorted(a['human'].items()):
                f.write(f'HUMAN\t{human}\t{bone}\n')
            for bone, x in a['pose'].items():
                t, q, s = x['t'], x['q'], x['s']
                f.write('POSE\t{}\t{}\t{}\n'.format(
                    bone, '\t'.join('%.8g' % v for v in t + q + s), ''))
    print(f'wrote {txt}')
    print(f'wrote {args.out}: {len(avatars)} avatars')
    for n, a in list(avatars.items())[:5]:
        print(f'   {n}: {sorted(a["human"])[:8]} ...')


if __name__ == '__main__':
    main()
