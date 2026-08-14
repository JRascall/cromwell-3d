"""
ba_bank.py - decrypt Broken Arrow's FMOD Studio banks and pull the audio out.

Broken Arrow ships its whole sound library as FMOD Studio banks (.bank) in
BrokenArrow_Data/StreamingAssets. Three GB of them, and none of the usual tools
open them, because the banks are built with FMOD's optional encryption.

WHAT "ENCRYPTED" ACTUALLY MEANS HERE, because it is narrower than it sounds.
A .bank is a RIFF container: 'FMT ', a big 'LIST' of project metadata, then one
'SND ' chunk holding an FSB5 sample archive. Only the FSB5 is enciphered. The
RIFF skeleton, the bus layout, the event and sample names - all plaintext. So
the container parses with no key at all, and the key is only needed for the
audio itself. vgmstream sees this and reports "couldn't load bank 0 at 1ee80
(encrypted?)", which is the accurate diagnosis and the end of its help.

THE CIPHER is the FSB scheme vgmstream implements in fsb_encrypted_streamfile.h
and it is not AES despite what half the internet says about FMOD banks:

    plain[i] = reverse_bits(cipher[i]) ^ key[i % len(key)]

with i counted from the first byte of the FSB5, not from the start of the file.
(vgmstream also knows an "alt" ordering, reverse_bits(cipher ^ key); Broken
Arrow uses the standard one. The check is one line and this script does it, so
a future bank built the other way is diagnosed rather than silently garbled.)

HOW THE KEY WAS FOUND, since the obvious route is a dead end. The key is a
string the game hands to FMOD at startup, so it lives in global-metadata.dat -
but IL2CPP compiles method bodies to native code, so nothing in a metadata dump
says WHICH literal is the key, and there are tens of thousands of them. The
cipher does not need any of that. It is a repeating XOR over a header whose
first bytes are known: an FSB5 begins with the magic 'FSB5' and a version field
of 1, which hands over eight key bytes for free:

    key[0..3] = reverse_bits(cipher[0..3]) ^ b"FSB5"
    key[4..7] = reverse_bits(cipher[4..7]) ^ b"\x01\x00\x00\x00"

That yields "jU5n9Ce2" - printable ASCII, which is the tell that the derivation
is right and not eight bytes of noise. The length falls out of the ciphertext:
the run of zero-valued header fields makes the ciphertext visibly repeat with
period 12, so the key is 12 bytes, and the last four come from the zeroed field
at 0x20 (cipher = reverse_bits(key) wherever the plaintext is zero).

    KEY = "jU5n9Ce2ng5T"

It is CONFIRMED rather than assumed, which matters because a wrong key produces
plausible-looking bytes rather than an obvious failure: decrypting the header
gives sampleHeadersSize + nameTableSize + dataSize that add up, with the 0x3C
header, to exactly the bytes remaining in the 'SND ' chunk. Four independent
size fields agreeing to the byte is not something a wrong key does.

WHAT COMES OUT. The decrypted FSB5 carries a name table, so every subsong has
the sound designer's own name on it - UI_CUSTOMIZE_AIR_Layer-001 and so on -
and vgmstream writes them out under those names. The codec is Vorbis (FSB5 mode
15) at 44.1 kHz.

Usage:

    py -3 tools/ba/ba_bank.py <bank-or-dir> --out DIR              # -> .fsb
    py -3 tools/ba/ba_bank.py <bank-or-dir> --out DIR --wav        # -> .wav too
    py -3 tools/ba/ba_bank.py <bank-or-dir> --list                 # inspect only

Resumable at file granularity: an output that already exists is skipped, so an
interrupted run over 3 GB of banks costs nothing to restart. --force overrides.
"""

import argparse
import os
import struct
import subprocess
import sys

# The 12-byte key, derived and verified as described above. Exposed as a
# default rather than hardcoded at the call site so a future patch that rebuilds
# the banks with a new key can be handled with --key instead of an edit.
DEFAULT_KEY = b"jU5n9Ce2ng5T"

# Bit-reversal LUT. The cipher reverses the bit order of every byte before the
# XOR; doing that arithmetically per byte over three gigabytes is the kind of
# thing that turns a 40-second job into a 10-minute one, and a 256-entry table
# costs nothing.
REVERSE_BITS = bytes(int(format(i, "08b")[::-1], 2) for i in range(256))

FSB5_MAGIC = b"FSB5"
# An FSB5 v1 header is 0x3C bytes: magic, version, then five u32 counts/sizes,
# a mode, flags, and a 16-byte hash. Everything after it is
# sampleHeaders + nameTable + data, and those three plus this are the file.
FSB5_HEADER_SIZE = 0x3C


def decrypt(buf, key, base=0, alt=False):
    """Apply the FSB stream cipher to buf.

    base is the offset of buf[0] within the FSB5, because the key position is
    absolute: decrypting a slice out of the middle of a file with base=0 gives
    bytes that look fine and are wrong.
    """
    if alt:
        return bytes(REVERSE_BITS[b ^ key[(base + i) % len(key)]] for i, b in enumerate(buf))
    return bytes(REVERSE_BITS[b] ^ key[(base + i) % len(key)] for i, b in enumerate(buf))


def riff_chunks(data):
    """Yield (id, offset_of_body, size) for the top-level chunks of a RIFF.

    Deliberately shallow. The nested LIST tree in a bank is thousands of chunks
    deep in places and none of it is needed to find the audio - the 'SND ' chunk
    is always at the top level beside 'FMT ' and 'LIST'.
    """
    if data[:4] != b"RIFF":
        return
    pos = 12  # 'RIFF' + size + 'FEV '
    end = len(data)
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        yield cid, pos + 8, size
        pos += 8 + size + (size & 1)  # RIFF chunks are word-aligned


def find_fsbs(data, key):
    """Locate every FSB5 inside a bank's 'SND ' chunk.

    Returns a list of (offset, size, alt) - alt recording which of the two
    cipher orderings matched, so a bank built the other way is reported rather
    than mis-decrypted.

    The search exists because the FSB does not start at the 'SND ' body: the
    banks carry a run of plaintext zero padding first, and the length of it
    VARIES - 10 bytes in Voice_Command, 16 in UI, 22 in Music_Campaign, 26 in
    Master. It is not chunk-relative padding at all: the FSB is placed at a
    32-byte-aligned offset in the FILE, so the padding is whatever it takes to
    get there from wherever the metadata happened to end.

    Which is why this steps one byte at a time rather than four. Stepping by the
    word found the banks whose padding happens to be a multiple of four and
    silently reported "no FSB5" for the other half - a failure that reads as
    "wrong key" and sends you back to the binary for no reason. Encryption makes
    this a search instead of a scan for a magic number, but it is a cheap one:
    a few hundred four-byte trial decryptions before the first hit.
    """
    found = []
    for cid, body, size in riff_chunks(data):
        if cid != b"SND ":
            continue
        limit = min(body + 0x400, body + size)
        pos = body
        while pos < limit:
            for alt in (False, True):
                if decrypt(data[pos:pos + 4], key, 0, alt) == FSB5_MAGIC:
                    # Trust the header's own arithmetic for the length rather
                    # than the chunk size: a bank may hold several FSBs
                    # back to back, and only the header says where one ends.
                    head = decrypt(data[pos:pos + FSB5_HEADER_SIZE], key, 0, alt)
                    shdr, names, dsize = struct.unpack_from("<3I", head, 0x0C)
                    total = FSB5_HEADER_SIZE + shdr + names + dsize
                    found.append((pos, total, alt))
                    break
            if found and found[-1][0] == pos:
                pos += found[-1][1]
                limit = body + size  # keep going: there may be another
                continue
            pos += 1
    return found


def describe(path, key):
    """Print what is in a bank without writing anything. The --list path."""
    data = open(path, "rb").read()
    name = os.path.basename(path)
    chunks = [(cid.decode("ascii", "replace"), size) for cid, _, size in riff_chunks(data)]
    if not chunks:
        print(f"{name}: not a RIFF bank")
        return
    fsbs = find_fsbs(data, key)
    print(f"{name}: {len(data) / 1e6:.1f} MB  chunks={[c for c, _ in chunks]}")
    if not fsbs:
        # Master.strings.bank is the normal case here: it is a string table of
        # event paths with no audio at all, and its absence of an FSB is not a
        # failure.
        print("    no FSB5 - metadata-only bank, or the key does not fit")
        return
    for off, size, alt in fsbs:
        head = decrypt(data[off:off + FSB5_HEADER_SIZE], key, 0, alt)
        ver, nsamples, shdr, names, dsize, mode = struct.unpack_from("<6I", head, 4)
        order = "alt" if alt else "std"
        print(f"    FSB5 v{ver} @0x{off:x} {size} bytes  {nsamples} samples  "
              f"codec={mode}  names={names}B  ({order})")


def extract(path, out_dir, key, want_wav, vgmstream, force):
    data = open(path, "rb").read()
    stem = os.path.splitext(os.path.basename(path))[0]
    fsbs = find_fsbs(data, key)
    if not fsbs:
        return 0

    written = 0
    for n, (off, size, alt) in enumerate(fsbs):
        suffix = "" if len(fsbs) == 1 else f"_{n}"
        fsb_path = os.path.join(out_dir, f"{stem}{suffix}.fsb")
        if force or not os.path.exists(fsb_path):
            os.makedirs(out_dir, exist_ok=True)
            with open(fsb_path, "wb") as f:
                f.write(decrypt(data[off:off + size], key, 0, alt))
            print(f"  {os.path.basename(fsb_path)}  {size / 1e6:.1f} MB")
        written += 1

        if not want_wav:
            continue

        wav_dir = os.path.join(out_dir, "wav", stem + suffix)
        # Resume at bank granularity for the wav step. Per-subsong resume is not
        # possible: vgmstream names the outputs from the FSB's internal name
        # table, so what a given subsong will be called is not known until it
        # has been written.
        if os.path.isdir(wav_dir) and os.listdir(wav_dir) and not force:
            print(f"  wav/{stem}{suffix} already done")
            continue
        os.makedirs(wav_dir, exist_ok=True)
        # -S 0 means "every subsong"; ?n in the output pattern is the stream
        # name from the FSB name table, which is what makes the output readable
        # instead of 4,000 files called 1.wav.
        subprocess.run(
            [vgmstream, "-S", "0", "-o", os.path.join(wav_dir, "?n.wav"), fsb_path],
            check=False, stdout=subprocess.DEVNULL,
        )
        print(f"  wav/{stem}{suffix}  {len(os.listdir(wav_dir))} files")
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("input", help="a .bank file, or a directory of them")
    ap.add_argument("--out", help="output directory (required unless --list)")
    ap.add_argument("--key", default=None, help="override the bank key")
    ap.add_argument("--wav", action="store_true", help="also decode to wav via vgmstream")
    ap.add_argument("--vgmstream", default="vgmstream-cli.exe", help="path to vgmstream-cli")
    ap.add_argument("--list", action="store_true", help="report contents, write nothing")
    ap.add_argument("--force", action="store_true", help="redo work already done")
    args = ap.parse_args()

    key = args.key.encode("utf-8") if args.key else DEFAULT_KEY

    if os.path.isdir(args.input):
        banks = sorted(
            os.path.join(args.input, f)
            for f in os.listdir(args.input)
            if f.lower().endswith(".bank")
        )
    else:
        banks = [args.input]
    if not banks:
        sys.exit(f"no .bank files under {args.input}")

    if args.list:
        for b in banks:
            describe(b, key)
        return

    if not args.out:
        sys.exit("--out is required unless --list is given")

    total = 0
    for b in banks:
        print(os.path.basename(b))
        total += extract(b, args.out, key, args.wav, args.vgmstream, args.force)
    print(f"\n{total} FSB archives from {len(banks)} banks -> {args.out}")


if __name__ == "__main__":
    main()
