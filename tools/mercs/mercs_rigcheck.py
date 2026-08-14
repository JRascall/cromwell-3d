"""mercs_rigcheck.py - will the clips still play on a rig you edited?

The workflow this exists for: export a character with mercs_gltf.py, open it in
Blender, add the joints the PS2 rig never had - forearm twist, upper-arm twist,
kneecaps - re-weight the mesh properly, export, and carry on using all 1,835
animations. That works, and it works for one reason:

    A CLIP BINDS BY JOINT NAME AND SETS LOCAL TRS OUTRIGHT.

Which has three consequences worth being exact about, because they decide what
you may and may not do in Blender:

  * Re-weighting is FREE. No clip refers to a vertex weight. Replace the rigid
    one-bone-per-vertex binding with smooth weights and every animation is
    untouched.
  * ADDING joints is FREE. A joint no clip mentions keeps its rest pose, which
    is what a twist bone wants - it is driven procedurally off its neighbour's
    roll, in the constraint layer, not by the animation.
  * RENAMING, REPARENTING or DELETING an original joint is what breaks things,
    and it breaks them silently. Blender's glTF round-trip is exactly where that
    happens: a `.001` suffix on a duplicated bone, a bone dissolved on export, a
    limb reparented while fixing a hierarchy.

Rest transforms may move. A clip's local TRS is absolute, not an offset from
rest, so a joint whose rest pose shifted still reaches the authored pose; the
inverse bind matrices come from the edited file and stay consistent with its own
bind pose. The check reports rest movement as information, not as an error.

    py -3 tools/mercs/mercs_rigcheck.py <original.glb> <edited.glb>
                                        [--anims mercs_extracted/animations]

Exit status is 1 if any clip that used to bind no longer does, so this can gate
a pipeline rather than only inform one.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import json
import os
import struct
import sys


def gltf_json(path):
    """The JSON chunk of a .glb. Geometry is not needed to judge a rig."""
    raw = open(path, 'rb').read()
    if raw[:4] != b'glTF':
        raise ValueError('%s is not a binary glTF' % path)
    off = 12
    while off + 8 <= len(raw):
        clen, ctype = struct.unpack_from('<II', raw, off)
        if ctype == 0x4E4F534A:
            return json.loads(raw[off + 8:off + 8 + clen].decode('utf-8', 'replace'))
        off += 8 + clen + (-clen % 4)
    raise ValueError('%s has no JSON chunk' % path)


def rig_of(js):
    """{name: {'parent':, 'trs':}} for the joints of the file's first skin."""
    nodes = js.get('nodes', [])
    skins = js.get('skins') or []
    joints = skins[0].get('joints', []) if skins else list(range(len(nodes)))
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', ()):
            parent[c] = i

    def nm(i):
        return (nodes[i].get('name') or '#%d' % i)

    out = {}
    for i in joints:
        p = parent.get(i)
        out[nm(i)] = {
            'parent': nm(p) if p is not None else '',
            'trs': (tuple(nodes[i].get('translation', (0, 0, 0))),
                    tuple(nodes[i].get('rotation', (0, 0, 0, 1)))),
        }
    return out


def skin_stats(path):
    """Mean influences per vertex, which is the number the round trip is about.

    The PS2 binding is rigid - one bone per vertex at weight 1.0 - so 'how many
    weights per vertex are non-zero' says whether the re-weighting actually
    happened. Counting JOINTS_ *sets* does not: a mesh with four real
    influences and one with a single bone both have exactly one JOINTS_0, and
    an earlier version of this check reported 17 either way and looked fine.
    """
    raw = open(path, 'rb').read()
    off, js, blob = 12, None, b''
    while off + 8 <= len(raw):
        n, t = struct.unpack_from('<II', raw, off)
        d = raw[off + 8:off + 8 + n]
        if t == 0x4E4F534A:
            js = json.loads(d.decode('utf-8', 'replace'))
        elif t == 0x004E4942:
            blob = d
        off += 8 + n + (-n % 4)
    if not js:
        return 0, 0.0

    total = live = 0
    for m in js.get('meshes', []):
        for p in m.get('primitives', []):
            k = 0
            while 'WEIGHTS_%d' % k in p.get('attributes', {}):
                acc = js['accessors'][p['attributes']['WEIGHTS_%d' % k]]
                view = js['bufferViews'][acc['bufferView']]
                start = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
                count = acc['count'] * 4
                if acc['componentType'] != 5126:      # only float weights here
                    k += 1
                    continue
                w = struct.unpack_from('<%df' % count, blob, start)
                live += sum(1 for v in w if v > 1e-6)
                total += acc['count']
                k += 1
    return total, (live / total if total else 0.0)


def anim_joint_names(path):
    """Joint names of a .anim, from its header - see mercs_anim.py's layout."""
    with open(path, 'rb') as f:
        head = f.read(8192)
        if head[:4] != b'MRCA':
            return None
        need = struct.unpack_from('<I', head, 40)[0]
        if need > len(head):
            f.seek(0)
            head = f.read(need)
    njoints = struct.unpack_from('<I', head, 12)[0]
    nev = struct.unpack_from('<I', head, 36)[0]
    p = 44

    def pstr(p):
        n = struct.unpack_from('<H', head, p)[0]
        return head[p + 2:p + 2 + n].decode('ascii', 'replace'), p + 2 + n

    name, p = pstr(p)
    for _ in range(nev):
        _, p = pstr(p + 4)
    out = []
    for _ in range(njoints):
        s, p = pstr(p)
        out.append(s.lower())
        p += 1
    return name, out


def binds(clip_joints, rig_names, threshold=0.75):
    """The rule the viewer and the exporter both use, kept in one place.

    Overlap against the SMALLER set: a clip may animate more joints than a model
    has, and an edited rig has MORE joints than the clip - measuring against
    either one alone rejects a perfectly good pairing in one direction or the
    other.
    """
    hit = len(set(clip_joints) & rig_names)
    return hit >= max(1, int(min(len(clip_joints), len(rig_names)) * threshold))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('original')
    ap.add_argument('edited')
    ap.add_argument('--anims', help='directory of .anim to re-check against')
    ap.add_argument('--move-tolerance', type=float, default=1e-4,
                    help='metres of rest movement before it is worth reporting')
    a = ap.parse_args()

    old = rig_of(gltf_json(a.original))
    new = rig_of(gltf_json(a.edited))
    lo = {k.lower(): k for k in old}
    ln = {k.lower(): k for k in new}

    kept = sorted(set(lo) & set(ln))
    lost = sorted(set(lo) - set(ln))
    added = sorted(set(ln) - set(lo))

    print('original: %d joints   edited: %d joints' % (len(old), len(new)))
    print('  kept   %d' % len(kept))
    print('  added  %d%s' % (len(added),
                             ('  ' + ', '.join(ln[x] for x in added[:8])
                              + (' ...' if len(added) > 8 else '')) if added else ''))
    print('  LOST   %d%s' % (len(lost),
                             ('  ' + ', '.join(lo[x] for x in lost[:8])
                              + (' ...' if len(lost) > 8 else '')) if lost else ''))

    # A rename shows up as one lost joint and one added joint whose rest
    # position is the same. Worth naming explicitly - "bone_l_calf became
    # bone_l_calf.001" is a fixable mistake, "the calf is gone" is alarming.
    suspects = []
    for l in lost:
        lt = old[lo[l]]['trs'][0]
        for x in added:
            nt = new[ln[x]]['trs'][0]
            if sum((lt[i] - nt[i]) ** 2 for i in range(3)) < 1e-8:
                suspects.append((lo[l], ln[x]))
    if suspects:
        print('\n  probably RENAMED (same rest position):')
        for o, n in suspects[:12]:
            print('    %-22s -> %s' % (o, n))

    reparented, moved = [], []
    for k in kept:
        po, pn = old[lo[k]]['parent'].lower(), new[ln[k]]['parent'].lower()
        if po != pn:
            reparented.append((lo[k], old[lo[k]]['parent'], new[ln[k]]['parent']))
        ot, nt = old[lo[k]]['trs'][0], new[ln[k]]['trs'][0]
        d = sum((ot[i] - nt[i]) ** 2 for i in range(3)) ** 0.5
        if d > a.move_tolerance:
            moved.append((lo[k], d))
    if reparented:
        print('\n  REPARENTED %d - this changes what a local transform means:'
              % len(reparented))
        for n, o, p in reparented[:12]:
            print('    %-22s %s -> %s' % (n, o or '(root)', p or '(root)'))
    if moved:
        moved.sort(key=lambda kv: -kv[1])
        print('\n  rest pose moved on %d joints (allowed - clips set local TRS '
              'absolutely):' % len(moved))
        for n, d in moved[:6]:
            print('    %-22s %.4f m' % (n, d))

    ov, oinf = skin_stats(a.original)
    nv, ninf = skin_stats(a.edited)
    print('\nskinning: %d verts at %.2f influences each -> %d verts at %.2f'
          % (ov, oinf, nv, ninf))
    if oinf < 1.05 <= ninf:
        print('  rigid binding replaced with smooth weights - the actual point '
              'of the round trip, and what stops knees and elbows creasing '
              'under IK')
    elif ninf <= 1.05:
        print('  still one bone per vertex: the mesh will crease at every joint '
              'no matter how good the IK is')

    status = 0
    if a.anims and os.path.isdir(a.anims):
        names_old = set(lo)
        names_new = set(ln)
        before = after = total = 0
        unbound = 0                      # joint tracks that no longer resolve
        hurt = []
        for f in sorted(os.listdir(a.anims)):
            if not f.endswith('.anim'):
                continue
            h = anim_joint_names(os.path.join(a.anims, f))
            if not h:
                continue
            total += 1
            cname, cj = h
            b0, b1 = binds(cj, names_old), binds(cj, names_new)
            before += b0
            after += b1
            # TRACKS, not clips. Whether a clip still "binds" is far too coarse
            # to notice a rename: losing 1 joint of 36 still clears 75%, so the
            # count stays at 1,639 while every walk quietly stops driving a
            # knee. What matters is how many joint tracks find a target.
            lost_tracks = len(set(cj) & names_old) - len(set(cj) & names_new)
            if lost_tracks > 0:
                unbound += lost_tracks
                hurt.append((cname, lost_tracks))
        print('\nclips in %s: %d' % (a.anims, total))
        print('  bound to the original rig : %d' % before)
        print('  bound to the edited rig   : %d' % after)
        if hurt:
            status = 1
            print('  CLIPS THAT LOST A TRACK   : %d (%d joint tracks in total)'
                  % (len(hurt), unbound))
            for c, k in hurt[:8]:
                print('    %-46s -%d' % (c[:44], k))
            print('  These still "play" - they just stop driving those joints, '
                  'which reads as a limb going stiff rather than as an error.')
        else:
            print('  every joint track still resolves.')

    if lost or reparented:
        status = 1
        print('\nVERDICT: the edit changed joints the clips address by name. '
              'Restore the original names and parents, or every clip that used '
              'them silently plays a partial pose.')
    elif status == 0:
        print('\nVERDICT: compatible. Added joints keep their rest pose under '
              'every clip, which is what a twist or helper bone should do - '
              'drive them from the constraint layer, not from the animation.')
    return status


if __name__ == '__main__':
    sys.exit(main())
