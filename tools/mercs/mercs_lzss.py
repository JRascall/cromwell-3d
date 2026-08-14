"""mercs_lzss.py - RedEngine's LZ77 variant, as used by Mercenaries on PS2.

This is a transcription of the routine at 0x003A6358 in SLUS_209.32, reached
through the wrapper at 0x003A6520 that the CSEG handler calls as
decompress(dest, src, srcLen). It is not a guess: four rounds of parameter
sweeping against known plaintext all failed, because the format has TWO match
encodings and every sweep assumed one. Reading the MIPS settled it in minutes.

THE FORMAT

    A 16-bit little-endian bit buffer, refilled every 16 bits, consumed from
    the LSB up. Each token starts with one bit:

        1            one literal byte follows
        0 0          SHORT match: two more bits give length 3..6,
                     one byte gives the offset as byte-256, i.e. -256..-1
        0 1          LONG match: two bytes b0,b1
                         offset = b0 + ((b1 & 0xF0) << 4) - 4096   -> -4096..-1
                         length = (b1 & 0x0F) + 3                  -> 4..18
                     If that low nibble is 0 the length escapes: read one more
                     byte; 0 ENDS THE STREAM, otherwise length = byte + 1.

    Offsets are negative displacements from the current output position, not
    ring-buffer indices, and the copy is byte at a time so overlapping runs
    (the RLE case) work. There is no window: a match may reach 4096 bytes back
    into anything already produced.

WHY IT LOOKED UNSOLVABLE FROM THE OUTSIDE. A stream opens `FF FF`, which reads
as an all-literal 16-bit flag word, and that forces 16 literals - after which
the next word contradicts bytes that must plainly be literals. The resolution
is that `FF FF` is not one flag word being spent on 16 literals; the bits are
spent two and three at a time on match prefixes as well, so the accounting
never lined up under a one-bit-per-token model.

CALLERS AND FRAMING. The compressor is used in two places with different
framing, which is why they had to be solved separately:

    CSEG   preceded by {u32 decompressedSize; u32 compressedSize}, where a
           compressedSize of 0xFFFFFFFF means the payload is stored verbatim.
           The engine tests this as a SIGNED `blez`, so -1 is the sentinel.
    tex_   the BODY chunk is the raw stream with no size header at all. It
           does not need one: the stream is self-terminating, and the expected
           size is width*height*bpp/8 from the tex_ INFO chunk.

Verified against all 2295 compressed CSEG chunks in STREAMED.DSK: every one
decodes to exactly its declared size and parses as a well-formed ucfb tree.
"""

STORED = 0xFFFFFFFF


def decompress(src, expected=None):
    """Unpack a RedEngine LZ stream. `expected` is an optional size assertion."""
    out = bytearray()
    p = 0
    n = len(src)
    bits = src[0] | (src[1] << 8)
    p = 2
    avail = 16

    def getbit():
        nonlocal bits, avail, p
        b = bits & 1
        avail -= 1
        bits >>= 1
        if avail == 0:
            # Refill eagerly, exactly as the original does - it reloads the
            # moment the sixteenth bit is spent, not lazily on the next read.
            bits = (src[p] | (src[p + 1] << 8)) if p + 1 < n else 0
            p += 2
            avail = 16
        return b

    while p <= n:
        if getbit():
            if p >= n:
                break
            out.append(src[p])
            p += 1
            continue

        if getbit() == 0:
            length = ((getbit() << 1) | getbit()) + 3
            if p >= n:
                break
            off = src[p] - 256
            p += 1
        else:
            if p + 1 >= n:
                break
            b0, b1 = src[p], src[p + 1]
            p += 2
            off = b0 + ((b1 & 0xF0) << 4) - 4096
            length = (b1 & 0x0F) + 3
            if length == 3:
                if p >= n:
                    break
                ext = src[p]
                p += 1
                if ext == 0:
                    break                      # end of stream
                length = ext + 1

        start = len(out) + off
        if start < 0:
            raise ValueError('back-reference before start of output')
        for i in range(length):
            out.append(out[start + i])

    if expected is not None and len(out) != expected:
        raise ValueError('unpacked %d bytes, expected %d' % (len(out), expected))
    return bytes(out)


def unpack_sized(payload):
    """A {u32 decSize; u32 compSize} block as CSEG frames it."""
    import struct
    dec, comp = struct.unpack_from('<II', payload, 0)
    body = payload[8:]
    if comp == STORED:
        return body[:dec]
    return decompress(body[:comp], expected=dec)
