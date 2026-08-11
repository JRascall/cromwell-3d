#!/usr/bin/env python3
"""bake_msdf.py - bake Inter into MSDF atlases for world-space text.

WHAT THIS IS FOR. Screen-space UI text is rasterised at exactly the size it
will be drawn at (ui/paint/UiFontSet.hpp), which is the sharpest thing possible
and requires knowing the size in advance. World-space text has no such size: a
nameplate's on-screen height changes every frame as the camera moves. A
multi-channel distance field is one atlas that stays crisp at any of them.

See study/distance_fields.md for why MSDF rather than SDF (corners), why the
antialiasing must be screen-space derived, and why baking is expensive while
drawing is not.

WHY A CONVERSION STEP AT ALL, when msdf-atlas-gen already writes JSON. Because
the engine has no JSON parser and does not need one for this. The generator's
output is rich, nested and aimed at tools; what a renderer wants is a flat
table it can read with a stream and no allocation per glyph. Doing that
translation here keeps the messy half in Python - exactly the split
tools/gen_fa_icons.py already uses for the icon vocabulary.

USAGE
    python tools/bake_msdf.py

Reads src/cromwell/assets/fonts/Inter/, writes .bmp + .cwfont pairs into
src/cromwell/assets/fonts/msdf/. Both are generated, both are gitignored, and
both are one command away. Requires the baker, which the normal build produces:

    cmake --build builds/_cmake-win --target msdf-atlas-gen-standalone --config Release
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INTER_DIR = ROOT / "src" / "cromwell" / "assets" / "fonts" / "Inter"
OUT_DIR = ROOT / "src" / "cromwell" / "assets" / "fonts" / "msdf"
BAKER = ROOT / "builds" / "_cmake-win" / "tools" / "msdf-atlas-gen.exe"

# Must match cromwell::sdf::kTextPxRange and what the shader is handed. It is
# written into the .cwfont as well, so the engine reads the value actually
# baked rather than trusting this constant to still be true.
PX_RANGE = 4

# Em size of each glyph cell in the atlas. This is NOT the size text renders at
# - the whole point is that any size works - it is how much resolution the
# field gets. 48 keeps Inter's thinner stems well above the point where a
# distance field stops being able to represent them, and still packs the ASCII
# set into a single 332x332 texture.
EM_SIZE = 48

# Y DOWN, matching every other 2D coordinate in this engine: raylib's screen
# space, the UI kit's rects, and the atlas rows themselves. Getting this wrong
# renders text upside down, which is at least an unambiguous symptom.
Y_ORIGIN = "top"

WEIGHTS = ["Regular", "Medium", "SemiBold", "Bold", "ExtraBold"]


def bake_one(source: Path, out_image: Path, out_json: Path) -> bool:
    """Runs the generator. Its stdout is the only progress report there is, so
    failures are surfaced rather than swallowed."""
    command = [
        str(BAKER),
        "-font", str(source),
        "-type", "msdf",
        # BMP because the build disables libpng - see the note in CMakeLists.
        # raylib loads BMP natively and the file never ships.
        "-format", "bmp",
        "-size", str(EM_SIZE),
        "-pxrange", str(PX_RANGE),
        "-yorigin", Y_ORIGIN,
        "-imageout", str(out_image),
        "-json", str(out_json),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAILED: {result.stdout.strip()} {result.stderr.strip()}",
              file=sys.stderr)
        return False
    return True


def write_cwfont(data: dict, destination: Path, image_name: str) -> int:
    """Flattens the generator's JSON into the engine's table.

    THE FORMAT IS TEXT, ON PURPOSE. It is a few kilobytes read once at load,
    so parsing speed is irrelevant, and being able to diff two bakes or eyeball
    a suspicious advance is worth more than the bytes. One record per line, all
    numbers, no nesting - a stream and a token at a time on the C++ side.

    Every unit stated once here so no reader has to infer it:
      - plane bounds are in EM, relative to the origin on the baseline, Y down.
        Multiply by the size you want and you have the quad.
      - atlas bounds are in TEXELS. Divide by the atlas size for UVs.
      - advance and the line metrics are in EM.
    """
    atlas = data["atlas"]
    metrics = data["metrics"]
    glyphs = data["glyphs"]
    kerning = data.get("kerning", [])

    lines = [
        "cwfont 1",
        f"image {image_name}",
        # The range the bake actually used, so the shader is never handed a
        # number that was typed twice and drifted once.
        f"atlas {atlas['width']} {atlas['height']} {atlas['distanceRange']}",
        f"metrics {metrics['lineHeight']} {metrics['ascender']} {metrics['descender']}",
        f"glyphs {len(glyphs)}",
    ]

    for glyph in glyphs:
        plane = glyph.get("planeBounds")
        rect = glyph.get("atlasBounds")
        if plane is None or rect is None:
            # A space has an advance and no geometry. Emitted with zeroed
            # bounds rather than skipped, so the loader's table is dense and a
            # missing glyph and a blank one stay distinguishable.
            lines.append(f"g {glyph['unicode']} {glyph['advance']} 0 0 0 0 0 0 0 0")
            continue
        lines.append(
            "g {} {} {} {} {} {} {} {} {} {}".format(
                glyph["unicode"], glyph["advance"],
                plane["left"], plane["top"], plane["right"], plane["bottom"],
                rect["left"], rect["top"], rect["right"], rect["bottom"]))

    # Kerning matters more here than in the UI, because world text is often set
    # large - the gap in "AV" or "To" that is invisible at 12 px is glaring at
    # 120 - so the format carries it.
    #
    # FOR INTER IT COMES OUT EMPTY, AND THAT IS CORRECT. msdf-atlas-gen reads
    # kerning through FT_Get_Kerning, which only sees the legacy `kern` table.
    # Inter has no `kern`; its pairs live in GPOS, which needs HarfBuzz to
    # read. Checked, not assumed - Inter-Regular.ttf's table directory is
    # GDEF/GPOS/GSUB/cmap/glyf/... with no `kern` anywhere.
    #
    # Left as-is rather than pulling HarfBuzz in: an unkerned Inter is barely
    # distinguishable at nameplate sizes because it is a modern sans with even
    # sidebearings, and the section stays in the format so that adding a source
    # of pairs later needs no change on the engine side.
    lines.append(f"kerning {len(kerning)}")
    for pair in kerning:
        lines.append(f"k {pair['unicode1']} {pair['unicode2']} {pair['advance']}")

    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(glyphs)


def main() -> int:
    if not BAKER.exists():
        print(f"baker not built: {BAKER}", file=sys.stderr)
        print("cmake --build builds/_cmake-win --target msdf-atlas-gen-standalone "
              "--config Release", file=sys.stderr)
        return 1

    sources = [(w, INTER_DIR / f"Inter-{w}.ttf") for w in WEIGHTS]
    missing = [w for w, path in sources if not path.exists()]
    if len(missing) == len(sources):
        print(f"no Inter faces in {INTER_DIR}", file=sys.stderr)
        print("The font pack is licensed and not in git - see "
              "src/cromwell/assets/fonts/README.md.", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    baked = 0

    with tempfile.TemporaryDirectory() as scratch:
        scratch_json = Path(scratch) / "atlas.json"
        for weight, source in sources:
            if not source.exists():
                print(f"  skipped {weight}: not installed")
                continue

            image = OUT_DIR / f"Inter-{weight}.bmp"
            print(f"  baking {weight} ...")
            if not bake_one(source, image, scratch_json):
                continue

            data = json.loads(scratch_json.read_text(encoding="utf-8"))
            count = write_cwfont(data, OUT_DIR / f"Inter-{weight}.cwfont", image.name)
            size = data["atlas"]
            print(f"    {count} glyphs, {size['width']}x{size['height']}, "
                  f"range {size['distanceRange']}")
            baked += 1

    if baked == 0:
        print("nothing baked", file=sys.stderr)
        return 1

    print(f"{baked} weight(s) -> {OUT_DIR.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
