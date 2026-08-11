# UI fonts

The widget kit (`src/cromwell/ui/`) resolves a file per weight through
`UiFontSet`. A weight with no file falls back to Regular; a set with no files at
all falls back to raylib's built-in bitmap font, which is 10px and looks it —
the UI still draws, it just stops being representative.

## The typeface

**Inter**, five weights (Regular, Medium, SemiBold, Bold, ExtraBold), SIL Open
Font License — `Inter-LICENSE.txt` is the licence as shipped with the faces.

This is not a placeholder. The letter-spacing values in the widget specs
(`1.0`–`2.4` px at 10–16 px) are Inter's 100–150 thousandths-of-an-em converted
to pixels, so they are only correct against Inter. Swapping the family means
re-tuning them.

## Where the binaries come from, and why they are gitignored

Copied from the PO project's own pack:
`E:\Game Development\PO\Content\PlanetsOnline\UI\Fonts\Inter\`. Font binaries
are kept out of code repositories (see `.gitignore`) and copied in from there —
same handling as every other licensed pack. **The build needs them present**; a
fresh clone gets the raylib fallback until they are copied.

Icons, when the UI wants them, come from the same place — the Font Awesome Pro
pack beside Inter in PO, whose glyphs are ordinary text runs to this kit (see
the note on icons in `ui/control/BorderButton.hpp`).

## Naming

`UiFontSet::loadWeight` takes an explicit path per weight, so the file names are
whatever the caller says. The game's gallery looks for `<Family>-<Weight>.ttf`
with the family named in one place — see `src/game/render/ui/WidgetGallery.cpp`.

## Atlas size

Rasterised once at 48px and scaled at draw time (see `UiFontSet.hpp`). Text drawn
*larger* than 48px will soften; if a screen needs display-sized type, load that
weight a second time at a larger atlas size rather than scaling this one up.

## Glyph coverage

`LoadFontEx` is asked for raylib's default codepoint set, which is ASCII. The
stepper's default chevrons (`‹` `›`, U+2039/203A) are outside it and will
rasterise as missing-glyph boxes — either override them per control, or widen
the codepoint set in `UiFontSet::loadWeight` if a screen needs a lot of
non-ASCII.
