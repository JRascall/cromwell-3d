"""mercs_fx.py - decode Mercenaries' audio event and sound cue tables.

WHAT THESE CHUNKS ARE, AND WHY THEY MATTER MORE THAN THEIR SIZE SUGGESTS

The audio extraction already produces every sample on the disc: 168 streams and
5,388 bank samples, 1.84 GB of WAV. What it does not produce is any idea which
one is which. These chunks are the other half - the table that says a given
game event plays a given bank entry, how often it may retrigger, how far it
carries, and which of two detail variants to use at range.

They are also the callee half of a contract the Lua already shows the caller
side of. The mission scripts call Audio_PlayVoiceover 218 times and
Radio_QueueMessage 201 times against string ids; those ids resolve here. With
the scripts alone you have a game that knows it wants to say something and no
way to find out what.

And some of it is not really about audio at all. The vehicle cues carry
engine_loop, engine_start, engine_acc, rev_threshold, rev_cutoff_threshold,
max_speed and max_reverse_speed - an RPM-driven engine model with its tuning
constants sitting in the sound table because that is who needed them.

THE FORMAT IS NOT A REVERSE ENGINEERING PROBLEM

Unlike every other chunk in this game, these are self-describing: a ucfb tree
whose leaves are null-separated ASCII key/value pairs. A float is the TEXT
"0.400000". Nothing is packed, quantised or compressed. The only work is
walking the tree.

    fx3_    396 audio events        name, category, sound_a/b/c, cooldown,
                                    priority, range, low_detail, high_detail
    fxtb    the lookup table        contact-event name -> effect name
    xcl_    sound cues              bank, positional, and an SSET sound set
    xch_    music cues              as xcl_, plus a TTBL transition table
    xsh_    cue shorthands          same shape
    xsl_    cue lists               same shape

    Leaves      INFO FLOT STRG REC_    key\0value\0key\0value...
    Containers  SSET (-> SNDS -> REC_) weighted alternatives for one cue
                TTBL (-> TRNS -> REC_) fromsound/tosound/flags for music

Both leaf kinds are read the same way, so the FLOT/STRG split is only a hint
about the intended type and is deliberately not used to coerce anything. A
value stays the text the game shipped.

WHAT IS INFERRED RATHER THAN READ

`priority` and `cooldown` each appear on exactly 186 events, and the seven
chatter_event_* categories total exactly 186. That is why those two fields are
described above as belonging to the banter system - it is a count matching a
count, not something read out of the executable, and it is the sort of thing
that would be quietly wrong if a category were miscounted.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import collections
import csv
import json
import os
import struct
import sys

KINDS = ('fx3_', 'fxtb', 'xcl_', 'xch_', 'xsh_', 'xsl_')

# LEAF OR CONTAINER IS A STRUCTURAL QUESTION, NOT A NAME ONE.
#
# The obvious implementation whitelists the leaf tags - INFO, FLOT, STRG, REC_
# - and recurses into everything else. It is wrong, and wrong in a way that
# looks like it works: REC_ is a LEAF under SNDS, holding a sound name and its
# weight, and a CONTAINER under TRKS, holding a PLAY node. Treating the second
# as key/value pairs turns 4,920 track names into 4,920 CSV columns, which is
# how this was caught.
#
# So a chunk is a container when it actually parses as one, and a leaf
# otherwise. That cannot drift when a new tag turns up.


def read_table(data):
    """Return [(size, nameHash, groupHash, offset), ...] for a .DSK image."""
    count = struct.unpack_from('<I', data, 0)[0]
    entries, offset = [], 8 + count * 12
    for i in range(count):
        size, name_hash, group_hash = struct.unpack_from('<III', data, 8 + i * 12)
        entries.append((size, name_hash, group_hash, offset))
        offset += size
    return entries


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


def kv(buf):
    """A leaf's null-separated key\\0value\\0... run.

    An odd count means the last key has no value, which has not been seen in
    this data but would silently drop a field if zip() were left to truncate
    it. Kept as an empty value instead so it shows up in the CSV.
    """
    parts = [s.decode('ascii', 'replace') for s in buf.split(b'\0')]
    while parts and parts[-1] == '':
        parts.pop()
    if len(parts) % 2:
        parts.append('')
    return list(zip(parts[0::2], parts[1::2]))


def is_container(buf):
    """Does this chunk parse as sub-chunks that use up all of it?

    Both halves matter. Printable four-letter tags alone are too weak - a run
    of ASCII text can fake one - so the walk must also land exactly on the end
    of the buffer, which random text will not.
    """
    kids, p = 0, 0
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]
        if not all(0x20 <= c < 0x7F for c in tag):
            return False
        size = struct.unpack_from('<I', buf, p + 4)[0]
        if p + 8 + size > len(buf):
            return False
        p = (p + 8 + size + 3) & ~3
        kids += 1
    return kids > 0 and p >= len(buf)


def parse(tag, buf):
    """One record -> {fields: {...}, <container tag>: [ ...nested... ]}."""
    node = {'_tag': tag, 'fields': collections.OrderedDict()}
    for child_tag, child in children(buf):
        if is_container(child):
            node.setdefault(child_tag, []).append(parse(child_tag, child))
        else:
            for k, v in kv(child):
                node['fields'][k] = v
    return node


def records(data, kinds):
    """Every top-level record in every DSK entry of the given kinds."""
    for size, name_hash, group_hash, offset in read_table(data):
        blob = data[offset:offset + size]
        if blob[:4] != b'ucfb' or blob[8:12].decode('ascii', 'replace') not in kinds:
            continue
        total = struct.unpack_from('<I', blob, 4)[0]
        for tag, body in children(blob[8:8 + total]):
            yield tag, parse(tag, body)


def label(node):
    """The most useful one-line name for a nested record."""
    f = node['fields']
    for key in ('name', 'tosound', 'fromsound'):
        if key in f:
            rest = ' '.join('%s=%s' % (k, v) for k, v in f.items() if k != key)
            return '%s%s' % (f[key], ' [%s]' % rest if rest else '')
    return ' '.join('%s=%s' % kv for kv in f.items())


def flatten(node, prefix=''):
    """One row per record. Nested collections are SUMMARISED, not exploded.

    A cue's sound set is a list of weighted alternatives; a music cue's
    transition table is a list of from/to rules. Giving each element its own
    column produces a header as long as the file. Each nested collection
    instead gets a count and a semicolon-joined summary, and the JSON keeps the
    full tree for anything that needs to walk it properly.
    """
    row = collections.OrderedDict(node['fields'])
    for tag, kids in node.items():
        if tag in ('_tag', 'fields'):
            continue
        name = prefix + tag.strip('_').lower()
        leaves = []
        stack = list(kids)
        while stack:
            n = stack.pop(0)
            nested = [c for t, cs in n.items()
                      if t not in ('_tag', 'fields') for c in cs]
            if nested:
                stack = nested + stack
            elif n['fields']:
                leaves.append(label(n))
        row[name + '_count'] = len(leaves)
        row[name] = '; '.join(leaves)
    return [row]


def write_csv(path, rows):
    """Union of every key seen, name first so the file is scannable."""
    keys = []
    for row in rows:
        for k in row:
            if k not in keys:
                keys.append(k)
    keys.sort(key=lambda k: (k != 'name', k.count('.'), k))
    with open(path, 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction='ignore')
        w.writeheader()
        for row in rows:
            w.writerow(row)
    return len(rows), len(keys)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    events, cues, table = [], [], []
    trees = collections.defaultdict(list)
    for path in args.dsk:
        data = open(path, 'rb').read()
        for tag, node in records(data, KINDS):
            trees[tag].append(node)
            if tag == 'fx3_':
                events.extend(flatten(node))
            elif tag == 'fxtb':
                # TABL is a flat run of strings read as pairs. num_entries says
                # how many there should be; report a mismatch rather than
                # trusting the pairing, because a stray empty string would
                # shift every row after it by one.
                pairs = list(node['fields'].items())
                head = dict(pairs[:3])
                want = int(head.get('num_entries', '0') or 0)
                body = pairs[3:]
                if want and len(body) != want:
                    print('  fxtb: %d pairs but num_entries says %d - pairing '
                          'is suspect, check TABL by hand' % (len(body), want))
                table.extend({'event': k, 'effect': v} for k, v in body)
            else:
                cues.extend(flatten(node))

    if events:
        n, k = write_csv(os.path.join(args.out, 'fx_events.csv'), events)
        print('  fx_events.csv     %5d rows, %d columns' % (n, k))
    if table:
        write_csv(os.path.join(args.out, 'fx_table.csv'), table)
        print('  fx_table.csv      %5d rows' % len(table))
    if cues:
        n, k = write_csv(os.path.join(args.out, 'sound_cues.csv'), cues)
        print('  sound_cues.csv    %5d rows, %d columns' % (n, k))
    with open(os.path.join(args.out, 'audio_tree.json'), 'w', encoding='utf-8') as f:
        json.dump(trees, f, indent=1)
    print('  audio_tree.json   %s records in %d kinds'
          % (format(sum(len(v) for v in trees.values()), ','), len(trees)))

    cats = collections.Counter(e.get('category', '?') for e in events)
    if cats:
        print('  categories: %s' % ', '.join('%s=%d' % kv for kv in cats.most_common(8)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
