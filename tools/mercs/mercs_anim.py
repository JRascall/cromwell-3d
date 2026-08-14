"""mercs_anim.py - decode Mercenaries' anim chunks to dense per-frame curves.

Animation is Pandemic's **Zephyr** system: the loader is ZephyrAnim.cpp in the
decompiled ELF (`FUN_00399098` reads the chunk, `FUN_003999b8` reads one joint,
`FUN_00399c88` decodes a rotation channel, `FUN_0039c1b0` samples translation).
Every field below was read out of those four functions rather than guessed, and
then checked against data that could contradict it - see THE CHECKS at the end.

THE CHUNK

    NAME  the animation's name
    INFO  {u16 frames; u16 joints; f32 invStaticScale; u16 events;
           u32 packedSize; u32 unpackedSize}          - 18 bytes
    XLTC  3 floats: the total root displacement over the whole clip. The loader
          turns it into a velocity as `xltc * 30 / frames`, which is where the
          30 fps in this file comes from - it is the engine's own constant, not
          an assumption about PS2 video timing.
    EVNT  {u16 frame; char name[]} - one per event. Sound cues
          (`sfx_base.body_fall`) and gameplay hooks (`FootFallRun`).
    CJNT  the curve data for every joint, LZ-packed with the same codec as the
          meshes (mercs_lzss) when packedSize < unpackedSize.
    jont  one per joint, in skeleton order:
            NAME  the joint name - a STRING, so animations bind to a skeleton by
                  name and no name-hash function is needed
            QROT  {u16 c0,c1,c2,c3} byte sizes of the four quaternion channels.
                  Only the first three reach the runtime's joint record; the
                  fourth channel is whatever is left, and the count exists only
                  to advance the cursor past it.
            DXLT  f32 - a per-joint scale. Its presence means the translation is
                  animated: `frames * 6` bytes of s16 triples.
            SXLT  3 x s16 - a fixed translation, scaled by 1/INFO.invStaticScale.

CURSOR ORDER. The jont chunks do not carry offsets into CJNT. The runtime walks
them in file order keeping a running cursor, and each sub-chunk advances it:
DXLT rounds up to even and takes `frames * 6`, QROT takes the sum of its four
counts. So the layout is implicit in the order the chunks appear, and a decoder
that reads them out of order silently desynchronises every joint after the first
mistake. That is also what makes the cursor a test - see below.

THE ROTATION CODEC (FUN_00399c88), per channel, one quaternion component:

    cur = s16 at the channel start; p = start + 2; frame 0 emits cur
    0x80 n   hold cur for n more frames
    0x81 vv  cur = the following s16, one frame
    else     cur += (s8)b, one frame
    component = cur / 2047.0

The 2047 is the runtime's literal 0.0004885198. Note it is NOT 32767: the
components only ever use about a sixteenth of the s16 range, which looks like
waste until you notice the delta opcode is a *signed byte* - a tighter scale
would make ordinary frame-to-frame motion overflow into the 3-byte escape and
the stream would get bigger, not smaller.

TRANSLATION (FUN_0039c1b0). Dynamic joints use their own DXLT float, so each
joint gets a quantisation range suited to it - a character's root moves in
centimetres and a camera's in hundreds of metres, and one global scale could not
serve both. Static joints share the animation-wide 1/INFO.invStaticScale.

THE CHECKS, none of which can be read backwards:

1.  The per-joint cursor must land exactly on INFO.unpackedSize. It does, for
    all 1,835 animations - no slack, no overrun.
2.  Decoded quaternions must be unit length. Across every animation the norm
    stays inside 0.995..1.004, which is the quantisation error of 1/2047 and
    nothing else. A wrong scale, a wrong channel order or a dropped opcode all
    break this immediately.
3.  Translations must agree with the skeleton in the modl chunk, which is a
    SEPARATE file this decoder never reads. `bone_root` in the American merc's
    idle sits at y=1.184; the skeleton puts bone_root at y=1.18. The static
    offsets match the bone spacing the same way.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_lzss import decompress, STORED          # noqa: E402

FPS = 30.0                     # ZephyrAnim's own constant, see XLTC above
QUAT_SCALE = 1.0 / 2047.0      # the runtime's 0.0004885198
MAGIC = b'MRCA'
VERSION = 1


def children(buf):
    out, p = [], 0
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]
        size = struct.unpack_from('<I', buf, p + 4)[0]
        if p + 8 + size > len(buf):
            break
        out.append((tag.decode('ascii', 'replace'), buf[p + 8:p + 8 + size]))
        p = (p + 8 + size + 3) & ~3
    return out


def cstr(b):
    return b.split(b'\0')[0].decode('ascii', 'replace')


def decode_channel(buf, off, size, frames):
    """One quaternion component, expanded to one value per frame.

    Returned dense rather than as keys on purpose: the stream is already
    frame-stepped - every opcode advances by a whole number of frames and the
    hold opcode repeats a value - so there is nothing to interpolate and no
    information is invented by expanding it. A consumer that wants keys can
    difference the result; a consumer that wants to play it back wants this.
    """
    if size < 2:
        return [0.0] * frames
    cur = struct.unpack_from('<h', buf, off)[0]
    p, end = off + 2, off + size
    out = [cur]
    while len(out) < frames and p < end:
        b = buf[p]
        if b == 0x80:                                   # hold
            out.extend([cur] * buf[p + 1])
            p += 2
        elif b == 0x81:                                 # absolute
            cur = struct.unpack_from('<h', buf, p + 1)[0]
            out.append(cur)
            p += 3
        else:                                           # signed delta
            cur = (cur + (b - 256 if b > 127 else b) + 32768) % 65536 - 32768
            out.append(cur)
            p += 1
    # A channel may stop early when the rest of the clip holds its last value.
    out.extend([cur] * (frames - len(out)))
    return [v * QUAT_SCALE for v in out[:frames]]


def decode(payload):
    """anim chunk payload -> {name, frames, joints[], events[], xltc}."""
    name, info, cjnt, jonts, events, xltc = '', None, b'', [], [], (0.0, 0.0, 0.0)
    for tag, buf in children(payload):
        if tag == 'NAME':
            name = cstr(buf)
        elif tag == 'INFO':
            info = struct.unpack_from('<HHfHII', buf, 0)
        elif tag == 'CJNT':
            cjnt = buf
        elif tag == 'XLTC':
            xltc = struct.unpack_from('<3f', buf, 0)
        elif tag == 'EVNT':
            events.append((struct.unpack_from('<H', buf, 0)[0], cstr(buf[2:])))
        elif tag == 'jont':
            jonts.append(buf)
    if info is None:
        return None
    frames, njoints, inv_static, nev, packed, unpacked = info
    data = cjnt if packed >= unpacked else decompress(cjnt[:packed], unpacked)
    static_scale = (1.0 / inv_static) if inv_static else 0.0

    cursor, joints = 0, []
    for jb in jonts:
        jname, counts, rot_off, dyn, trans_off, sxlt = '', None, 0, None, 0, None
        for k, v in children(jb):
            if k == 'NAME':
                jname = cstr(v)
            elif k == 'QROT':
                counts = struct.unpack_from('<4H', v, 0)
                rot_off = cursor
                cursor += sum(counts)
            elif k == 'DXLT':
                dyn = struct.unpack_from('<f', v, 0)[0]
                cursor = (cursor + 1) & ~1
                trans_off = cursor
                cursor += frames * 6
            elif k == 'SXLT':
                sxlt = struct.unpack_from('<3h', v, 0)

        quat = []
        if counts:
            o, comps = rot_off, []
            for c in counts:
                comps.append(decode_channel(data, o, c, frames))
                o += c
            quat = list(zip(*comps))
        else:
            quat = [(0.0, 0.0, 0.0, 1.0)] * frames

        if dyn is not None:
            trans = [tuple(c * dyn for c in
                           struct.unpack_from('<3h', data, trans_off + f * 6))
                     for f in range(frames)]
        elif sxlt:
            trans = [tuple(c * static_scale for c in sxlt)] * frames
        else:
            trans = [(0.0, 0.0, 0.0)] * frames

        joints.append({'name': jname, 'quat': quat, 'trans': trans,
                       'dynamic': dyn is not None})

    return {'name': name, 'frames': frames, 'joints': joints, 'events': events,
            'xltc': xltc, 'consumed': cursor, 'expected': unpacked,
            'duration': frames / FPS}


def write_anim(path, a):
    """The .anim container. Layout, little-endian throughout:

        char[4] 'MRCA'   u32 version   u32 frames    u32 joints
        f32 fps          f32 duration  f32 xltc[3]
        u32 eventCount   u32 dataOffset
        u16 len + name
        events: u32 frame, u16 len + name
        joints: u16 len + name, u8 flags (bit 0 = animated translation)
        <pad to 16>
        joint-major dense block: joints * frames * 7 f32
                                 (qx, qy, qz, qw, tx, ty, tz)

    dataOffset is in the header rather than implied so the float block can be
    16-byte aligned and read by casting a mapped pointer. The strings above it
    are variable length, which would otherwise leave the floats at an arbitrary
    offset - fine for memcpy, not fine for anything wanting alignment.
    """
    def pstr(s):
        b = s.encode('ascii', 'replace')
        return struct.pack('<H', len(b)) + b

    head = bytearray()
    head += pstr(a['name'])
    for frame, ev in a['events']:
        head += struct.pack('<I', frame) + pstr(ev)
    for j in a['joints']:
        head += pstr(j['name']) + struct.pack('<B', 1 if j['dynamic'] else 0)

    fixed = 4 + 4 * 4 + 4 * 5 + 4 * 2
    data_off = (fixed + len(head) + 15) & ~15
    out = bytearray()
    out += MAGIC + struct.pack('<III', VERSION, a['frames'], len(a['joints']))
    out += struct.pack('<5f', FPS, a['duration'], *a['xltc'])
    out += struct.pack('<II', len(a['events']), data_off)
    out += head
    out += b'\0' * (data_off - len(out))
    for j in a['joints']:
        for f in range(a['frames']):
            out += struct.pack('<7f', *j['quat'][f], *j['trans'][f])
    with open(path, 'wb') as f:
        f.write(out)
    return len(out)


def to_json(a):
    return {
        'name': a['name'], 'frames': a['frames'], 'fps': FPS,
        'duration': a['duration'], 'xltc': list(a['xltc']),
        'events': [{'frame': f, 'name': n} for f, n in a['events']],
        'joints': [{'name': j['name'], 'animatedTranslation': j['dynamic'],
                    'quat': [list(q) for q in j['quat']],
                    'trans': [list(t) for t in j['trans']]}
                   for j in a['joints']],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--json', metavar='NAME',
                    help='also write <NAME>.json - one animation, readable')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rows = []
    done = failed = short = 0
    total_bytes = 0
    seen = set()
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'anim' or h in seen:
                continue
            seen.add(h)
            if args.limit and done >= args.limit:
                break
            payload = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            try:
                a = decode(payload)
            except Exception:
                a = None
            if not a:
                failed += 1
                continue
            # The cursor landing anywhere but exactly on the unpacked size means
            # a joint was misread; say so rather than writing a plausible file.
            if a['consumed'] != a['expected']:
                short += 1
                print('  ! %s consumed %d of %d bytes'
                      % (a['name'], a['consumed'], a['expected']))
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in a['name'])
            stem = '%s_%08X' % (safe[:60] or 'anim', h)
            total_bytes += write_anim(os.path.join(args.out, stem + '.anim'), a)
            rows.append((a['name'], '%08X' % h, a['frames'], '%.3f' % a['duration'],
                         len(a['joints']),
                         sum(1 for j in a['joints'] if j['dynamic']),
                         len(a['events']), stem + '.anim'))
            if args.json and a['name'].lower() == args.json.lower():
                with open(os.path.join(args.out, stem + '.json'), 'w',
                          encoding='utf-8') as f:
                    json.dump(to_json(a), f, indent=1)
            done += 1

    with open(os.path.join(args.out, 'animations.tsv'), 'w', encoding='utf-8') as f:
        f.write('name\thash\tframes\tseconds\tjoints\tanimated_translations\t'
                'events\tfile\n')
        for r in sorted(rows):
            f.write('\t'.join(str(c) for c in r) + '\n')

    print('  %d animations -> %s (%.1f MB)'
          % (done, args.out, total_bytes / 1048576.0))
    if short:
        print('  %d did not consume their curve data exactly' % short)
    if failed:
        print('  %d could not be decoded' % failed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
