"""xcom_anim.py - animation metadata and notify events from the SDK.

umodel gives you the bone tracks (.psa: BONENAMES / ANIMINFO / ANIMKEYS) but
ActorX carries NO notify events, so a .psa cannot tell you when a footstep
lands or a shot fires. That information is only in the SDK, and unlike the
track data it IS reachable: `batchexport <pkg> AnimSequence T3D` dumps every
sequence's properties.

The two are complementary - run both.

What a dumped sequence looks like:

    Begin Object Class=AnimNotify_AkEvent Name=AnimNotify_AkEvent_27
       AkEvent=AkEvent'SoundX2CharacterFX.XCom_and_Advent_Soldier_Foley_VeryShort'
    End Object
    SequenceName="HL_CarryBodyStartMF"
    SequenceLength=2.200000
    NumFrames=67
    Notifies(2)=(Time=0.650590,Notify=AnimNotify_AkEvent'AnimNotify_AkEvent_27')

so a clip yields its name, duration, frame count, and a timeline of typed
events. The AkEvent payload names a Wwise event, which cross-references into
the WAVs xcom_audio.ps1 extracted.

COVERAGE, and why there are two sources:

  * CLIPS come from the .psa files umodel produced - their ANIMINFO chunk is a
    complete, authoritative table (name, frame count, rate, bone count).
  * NOTIFIES come from the T3D dump, which is PARTIAL. BatchExport names each
    file after the object, and most AnimSequences are anonymous
    (`AnimSequence_0`, `AnimSequence_1`, ...) with the numbering restarting per
    AnimSet, so files overwrite each other: Soldier_ANIM reports 866 sequences
    exported but leaves 252 files. There is no BatchExport option to
    disambiguate, so notify coverage is roughly a third of clips.

Subcommands:
    clips     --anim DIR --out CSV            complete clip table from .psa
    scan      --pkg NAME --dump DIR --out TSV  parse one package's T3D dump
    build     --mats DIR --out CSV            join every scan into notify CSVs
"""

from __future__ import annotations

import argparse
import csv
import re
import struct
from collections import Counter
from pathlib import Path


def read_psa(path: Path):
    """Yield one dict per sequence from an ActorX .psa ANIMINFO chunk.

    Chunks are a 32-byte header (20-byte tag, flags, entry size, count)
    followed by count*size bytes. ANIMINFO entries are VAnimInfoBinary:
    name[64], group[64], then ints/floats where the useful ones are the bone
    count, TrackTime, AnimRate and NumRawFrames.

    Note TrackTime is expressed in FRAMES, not seconds - duration is
    NumRawFrames / AnimRate, which is why a "76.00" track is 2.5s at 30fps.
    """
    with path.open('rb') as f:
        while True:
            head = f.read(32)
            if len(head) < 32:
                return
            tag = head[0:20].split(b'\0')[0].decode('ascii', 'replace')
            _, size, count = struct.unpack('<iii', head[20:32])
            if tag != 'ANIMINFO':
                f.seek(size * count, 1)
                continue
            for _ in range(count):
                b = f.read(size)
                if len(b) < 168:
                    return
                name = b[0:64].split(b'\0')[0].decode('ascii', 'replace')
                group = b[64:128].split(b'\0')[0].decode('ascii', 'replace')
                bones = struct.unpack('<i', b[128:132])[0]
                rate = struct.unpack('<f', b[152:156])[0]
                frames = struct.unpack('<i', b[164:168])[0]
                yield {
                    'clip': name, 'group': group, 'bones': bones,
                    'frames': frames, 'fps': round(rate, 3),
                    'length_s': round(frames / rate, 4) if rate else '',
                }


def clips(anim: Path, out: Path):
    rows = []
    for psa in sorted(anim.rglob('*.psa')):
        # xcom_extracted/anim/<Package>/AnimSet/<Set>.psa
        try:
            pkg = psa.relative_to(anim).parts[0]
        except ValueError:
            pkg = psa.parent.name
        for r in read_psa(psa):
            rows.append({'package': pkg, 'anim_set': psa.stem, **r})
    if not rows:
        print(f"No .psa found under {anim}")
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    cols = ['package', 'anim_set', 'clip', 'group', 'length_s', 'frames', 'fps', 'bones']
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader(); w.writerows(rows)
    print(f"{len(rows)} clips from {len({r['package'] for r in rows})} packages "
          f"/ {len({r['anim_set'] for r in rows})} anim sets -> {out}")
    tot = sum(float(r['length_s']) for r in rows if r['length_s'] != '')
    print(f"  total animation: {tot/60:.0f} minutes")
    return 0

# Nested notify definitions come before the sequence body that references them.
OBJ_RE = re.compile(r"Begin Object Class=(\S+) Name=(\S+)(.*?)^   End Object", re.S | re.M)
SEQNAME_RE = re.compile(r'^\s*SequenceName="([^"]*)"', re.M)
SEQLEN_RE = re.compile(r'^\s*SequenceLength=([\d.eE+-]+)', re.M)
FRAMES_RE = re.compile(r'^\s*NumFrames=(\d+)', re.M)
RATESCALE_RE = re.compile(r'^\s*RateScale=([\d.eE+-]+)', re.M)
# Time is optional - a notify with no Time fires at 0.
NOTIFY_RE = re.compile(r"^\s*Notifies\(\d+\)=\((?:Time=([\d.eE+-]+),)?Notify=(\w+)'([^']+)'", re.M)
# Whatever payload the notify carries: AkEvent name, footstep foot, etc.
PAYLOAD_RE = re.compile(r"^\s*(\w+)=(?:\w+')?([^'\r\n]+)'?", re.M)
PAYLOAD_SKIP = {'Name', 'ObjectArchetype'}


def scan(dump: Path, pkg: str, out: Path):
    clips, events = [], []
    for f in sorted(dump.glob('*.T3D')):
        txt = f.read_text(errors='replace')

        # Index the notify objects declared in this file.
        defs: dict[str, tuple[str, str]] = {}
        for cls, name, body in OBJ_RE.findall(txt):
            if 'Notify' not in cls:
                continue
            payload = ''
            for k, v in PAYLOAD_RE.findall(body):
                if k not in PAYLOAD_SKIP and not v.startswith('Default__'):
                    payload = v.strip().rstrip("'")
                    break
            defs[name] = (cls, payload)

        for seq in SEQNAME_RE.finditer(txt):
            name = seq.group(1)
            # Properties belong to the sequence that follows them, so slice
            # from this SequenceName to the next one.
            nxt = SEQNAME_RE.search(txt, seq.end())
            body = txt[seq.end(): nxt.start() if nxt else len(txt)]
            head = txt[max(0, seq.start() - 4000): seq.start()] + body

            length = SEQLEN_RE.search(head)
            frames = FRAMES_RE.search(head)
            rate = RATESCALE_RE.search(head)
            notifies = NOTIFY_RE.findall(body)
            clips.append({
                'package': pkg, 'clip': name,
                'length_s': float(length.group(1)) if length else '',
                'frames': int(frames.group(1)) if frames else '',
                'rate_scale': float(rate.group(1)) if rate else 1.0,
                'notify_count': len(notifies),
            })
            for time, cls, ref in notifies:
                cls2, payload = defs.get(ref, (cls, ''))
                events.append({
                    'package': pkg, 'clip': name,
                    'time_s': float(time) if time else 0.0,
                    'notify': cls2, 'payload': payload,
                })

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh, delimiter='\t')
        for c in clips:
            w.writerow(['C', c['package'], c['clip'], c['length_s'], c['frames'],
                        c['rate_scale'], c['notify_count']])
        for e in events:
            w.writerow(['E', e['package'], e['clip'], e['time_s'], e['notify'], e['payload']])
    print(f"{pkg}: {len(clips)} clips, {len(events)} notify events")
    return 0


def build(mats: Path, out: Path):
    clips, events = [], []
    for tsv in sorted(mats.glob('*.tsv')):
        for row in csv.reader(tsv.open(encoding='utf-8'), delimiter='\t'):
            if not row:
                continue
            if row[0] == 'C' and len(row) >= 7:
                clips.append(dict(zip(
                    ['package', 'clip', 'length_s', 'frames', 'rate_scale', 'notify_count'],
                    row[1:7])))
            elif row[0] == 'E' and len(row) >= 6:
                events.append(dict(zip(
                    ['package', 'clip', 'time_s', 'notify', 'payload'], row[1:6])))

    if not clips:
        print(f"No scans found in {mats}")
        return 1

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['package', 'clip', 'length_s', 'frames',
                                           'rate_scale', 'notify_count'])
        w.writeheader(); w.writerows(clips)
    ev = out.with_name(out.stem + '_events.csv')
    with ev.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['package', 'clip', 'time_s', 'notify', 'payload'])
        w.writeheader(); w.writerows(events)

    print(f"{len(clips)} clips from {len({c['package'] for c in clips})} packages -> {out}")
    print(f"{len(events)} notify events -> {ev}")
    print("  by notify type: " + ", ".join(
        f"{k}={v}" for k, v in Counter(e['notify'] for e in events).most_common(6)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('clips')
    c.add_argument('--anim', required=True, type=Path)
    c.add_argument('--out', required=True, type=Path)
    s = sub.add_parser('scan')
    s.add_argument('--pkg', required=True)
    s.add_argument('--dump', required=True, type=Path)
    s.add_argument('--out', required=True, type=Path)
    b = sub.add_parser('build')
    b.add_argument('--mats', required=True, type=Path)
    b.add_argument('--out', required=True, type=Path)
    a = ap.parse_args(argv)
    if a.cmd == 'clips':
        return clips(a.anim, a.out)
    if a.cmd == 'scan':
        return scan(a.dump, a.pkg, a.out)
    return build(a.mats, a.out)


if __name__ == '__main__':
    raise SystemExit(main())
