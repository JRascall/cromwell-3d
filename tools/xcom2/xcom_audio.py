"""xcom_audio.py - decode XCOM 2's Wwise audio to WAV, with real names.

XCOM 2's sound is NOT in the .upk packages. The SoundNodeWave objects there
are stubs: `batchexport ... SoundNodeWave WAV` reports success and writes
nothing. The actual audio is Wwise-encoded in Content/WwiseAudio: ~24,800
loose .wem files (Wwise Vorbis, codec 0xFFFF) named by numeric ID, plus .bnk
banks and one .txt manifest per bank.

Those .txt manifests are the useful part - they carry tab-separated sections:

    Streamed Audio   ID  Name  Audio source file  ...
    In Memory Audio  ID  Name  Audio source file  ...  Data Size
    Event            ID  Name  ...

so 1001493842.wem can be recovered as SF00_en_au_StunTarget_01.wav rather than
staying a number. Decoding needs vgmstream (an external decoder); this script
only builds the map and drives it.

    py -3 tools/xcom2/xcom_audio.py map   --wwise <dir> [--out map.csv]
    py -3 tools/xcom2/xcom_audio.py plan  --wwise <dir> --dest <dir> [--slice i --of n]
"""

from __future__ import annotations

import argparse
import csv
import re
import struct
from collections import Counter
from pathlib import Path


def bank_entries(path: Path):
    """Yield (wem_id, blob) for audio embedded in a Wwise .bnk.

    A bank is a flat chunk list: 4-byte tag, 4-byte LE size, body. DIDX is an
    index of 12-byte (id, offset, size) records and DATA is the blob they point
    into. This matters because only ~24.8k .wem sit loose on disk while 288k
    more live inside banks - every weapon, footstep and environment sound among
    them.
    """
    d = path.read_bytes()
    i = 0
    index: list[tuple[int, int, int]] = []
    data_off = None
    while i + 8 <= len(d):
        tag = d[i:i + 4]
        size = struct.unpack('<I', d[i + 4:i + 8])[0]
        body = i + 8
        if tag == b'DIDX':
            index = [struct.unpack('<III', d[body + k * 12: body + k * 12 + 12])
                     for k in range(size // 12)]
        elif tag == b'DATA':
            data_off = body
        i = body + size
        if size < 0 or i <= body - 8:      # malformed; stop rather than spin
            break
    if data_off is None:
        return
    for wid, off, size in index:
        start = data_off + off
        if 0 <= start and start + size <= len(d):
            yield wid, d[start:start + size]

# Rows are tab separated but the columns are ragged - blank trailing fields
# and empty "Notes" cells are common, so split and filter rather than index.
SECTION_RE = re.compile(r'^(\S[^\t]*)\tID\t')
BAD_CHARS = re.compile(r'[^A-Za-z0-9_.-]')


def parse_manifests(wwise: Path) -> dict[str, dict]:
    """id -> {name, bank, section} from every .txt beside the banks."""
    out: dict[str, dict] = {}
    for txt in wwise.rglob('*.txt'):
        bank = txt.stem
        section = None
        for line in txt.read_text(encoding='utf-8', errors='replace').splitlines():
            m = SECTION_RE.match(line)
            if m:
                section = m.group(1).strip()
                continue
            if not section or not line.strip():
                continue
            cells = [c for c in line.split('\t') if c.strip()]
            if len(cells) < 2:
                continue
            ident, name = cells[0].strip(), cells[1].strip()
            if not ident.isdigit():
                continue
            # Streamed Audio wins over In Memory: the loose .wem files on disk
            # are the streamed ones, so prefer that naming when both appear.
            if ident in out and out[ident]['section'] == 'Streamed Audio':
                continue
            out[ident] = {'name': name, 'bank': bank, 'section': section}
    return out


def safe(name: str) -> str:
    return BAD_CHARS.sub('_', name)[:120] or 'unnamed'


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('banks', help='extract audio embedded in .bnk and plan its conversion')
    b.add_argument('--wwise', required=True, type=Path)
    b.add_argument('--stage', required=True, type=Path)
    b.add_argument('--dest', required=True, type=Path)
    b.add_argument('--out', required=True, type=Path)
    b.add_argument('--slice', type=int, default=0)
    b.add_argument('--of', type=int, default=1)
    b.add_argument('--language', default=None,
                   help="only this localised VO folder, e.g. 'English(US)'. "
                        "Non-localised audio (_common) is always included.")
    for c in ('map', 'plan'):
        p = sub.add_parser(c)
        p.add_argument('--wwise', required=True, type=Path)
        p.add_argument('--out', type=Path)
        if c == 'plan':
            p.add_argument('--dest', required=True, type=Path)
            p.add_argument('--slice', type=int, default=0)
            p.add_argument('--of', type=int, default=1)
            p.add_argument('--language', default=None,
                           help="only this localised VO folder, e.g. 'English(US)'. "
                                "Non-localised audio is always included.")
    a = ap.parse_args(argv)

    ids = parse_manifests(a.wwise)

    if a.cmd == 'banks':
        def lang_of(p: Path) -> str:
            parts = p.parent.relative_to(a.wwise).parts
            return safe(parts[1]) if len(parts) > 1 else '_common'

        # First pass: decide a destination for every unique embedded id, so
        # collisions are resolved globally rather than per worker.
        seen: dict[int, tuple[Path, str, str]] = {}
        for bnk in sorted(a.wwise.rglob('*.bnk')):
            for wid, _ in bank_entries(bnk):
                if wid not in seen:
                    seen[wid] = (bnk, lang_of(bnk), safe(bnk.stem))
        keyed = {}
        for wid, (bnk, lang, bank) in seen.items():
            e = ids.get(str(wid))
            keyed[wid] = (lang, bank, safe(e['name']) if e else str(wid))
        dup = Counter(keyed.values())

        # Second pass: this worker's slice only, writing the blob out once.
        lines = []
        n = 0
        for bnk in sorted(a.wwise.rglob('*.bnk')):
            for wid, blob in bank_entries(bnk):
                if seen[wid][0] != bnk:
                    continue                      # another bank owns this id
                n += 1
                if n % a.of != a.slice:
                    continue
                lang, bank, name = keyed[wid]
                # Skip other locales BEFORE writing the blob out, so unwanted
                # VO costs neither conversion time nor disk.
                if a.language and lang != '_common' and lang != safe(a.language):
                    continue
                stem = name if dup[keyed[wid]] == 1 else f"{name}_{wid}"
                wem = a.stage / bank / f"{wid}.wem"
                wem.parent.mkdir(parents=True, exist_ok=True)
                if not wem.exists():
                    wem.write_bytes(blob)
                lines.append(f"{wem}\t{a.dest / lang / bank / (stem + '.wav')}")
        a.out.write_text("\n".join(lines) + "\n", encoding='utf-8')
        print(f"unique embedded ids: {len(seen)}")
        print(f"staged this slice  : {len(lines)} -> {a.out}")
        return 0
    wems = sorted(a.wwise.rglob('*.wem'))
    hit = sum(1 for w in wems if w.stem in ids)
    print(f"manifest entries : {len(ids)}")
    print(f".wem files       : {len(wems)}")
    print(f"named            : {hit} ({100*hit/max(len(wems),1):.1f}%)")
    print("  by section     : " + ", ".join(
        f"{k}={v}" for k, v in Counter(v['section'] for v in ids.values()).most_common()))

    if a.cmd == 'map':
        out = a.out or (a.wwise.parent / 'wem_names.csv')
        with out.open('w', newline='', encoding='utf-8') as fh:
            w = csv.writer(fh)
            w.writerow(['wem_id', 'name', 'bank', 'section', 'file'])
            for wem in wems:
                e = ids.get(wem.stem)
                w.writerow([wem.stem, e['name'] if e else '', e['bank'] if e else '',
                            e['section'] if e else '', wem.name])
        print(f"-> {out}")
        return 0

    # plan: emit "<src>\t<dst>" lines for the shell to feed to vgmstream.
    # Grouping by bank keeps 24k files out of one directory and matches how
    # the game organises them (one bank per voice pack / area / weapon set).
    #
    # Names are NOT unique: the same bank+name is reused across many wem IDs
    # (24827 sources collapse to 5273 names, 3900 of them colliding). Writing
    # them naively means every collision after the first is skipped or
    # overwritten, silently losing ~79% of the audio. So count first, and
    # suffix the wem ID only where a name is actually ambiguous - unique names
    # stay clean.
    # The same wem ID appears once PER LANGUAGE (Windows/German/123.wem and
    # Windows/French(France)/123.wem are the same line in different VO), so the
    # language folder has to be part of the path or 7684 files overwrite each
    # other. Non-localised audio sits in Windows/ directly and goes to _common.
    def lang_of(wem: Path) -> str:
        parts = wem.parent.relative_to(a.wwise).parts
        return safe(parts[1]) if len(parts) > 1 else '_common'

    keys = []
    for wem in wems:
        e = ids.get(wem.stem)
        keys.append((lang_of(wem), safe(e['bank']), safe(e['name'])) if e
                    else (lang_of(wem), '_unnamed', wem.stem))
    dup = Counter(keys)

    lines = []
    for i, (wem, k) in enumerate(zip(wems, keys)):
        if i % a.of != a.slice:
            continue
        if a.language and k[0] not in ('_common',) and k[0] != safe(a.language):
            continue
        # Suffix the ID only where a name is still ambiguous inside its
        # language; unique names stay clean.
        stem = k[2] if dup[k] == 1 else f"{k[2]}_{wem.stem}"
        lines.append(f"{wem}\t{a.dest / k[0] / k[1] / (stem + '.wav')}")
    out = a.out or Path(f'plan_{a.slice}.tsv')
    out.write_text("\n".join(lines) + "\n", encoding='utf-8')
    print(f"planned {len(lines)} conversions -> {out}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
