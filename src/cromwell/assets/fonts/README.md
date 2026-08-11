# cromwell/assets/fonts

The engine's typeface. **The font files themselves are not in git** — they are
licensed binaries, and `.gitignore` explains which licence stops which file.
This README is what a fresh checkout gets instead, so it has to be enough to
put the directory back.

## What belongs here

```
Inter/
    Inter-Regular.ttf        400
    Inter-Medium.ttf         500
    Inter-SemiBold.ttf       600
    Inter-Bold.ttf           700
    Inter-ExtraBold.ttf      800
    LICENSE.txt              SIL OFL 1.1
FontAwesome/
    FontAwesome-ProSolid.otf     the classic set, all 4349 icons
    FontAwesome-Brands.otf       company logos — Steam, Discord, GitHub
    LICENSE.txt                  Free faces: fonts OFL 1.1, icons CC BY 4.0
    LICENSE-Pro.txt              the paid terms
    CHEATSHEET_FULL.txt          name → codepoint, and the generator's input
```

The five Inter weights are not a taste call — they are exactly
`cromwell::ui::FontWeight` in `ui/core/UiText.hpp`, and the widget kit promises
that a weight a font set has no file for falls back to Regular rather than
vanishing. Adding a sixth file here without adding it to that enum gives the
widget kit no way to ask for it.

Two of Font Awesome Pro's thirty-seven faces, on the other hand, is a taste
call. Each face is atlas source that has to be rasterised and kept resident,
and Sharp, Duotone and the display families (Chisel, Jelly, Notdog, …) are a
styling choice a particular screen makes, not something the engine should carry
by default. They are licensed and available in the PO pack when a screen wants
one.

## Where to get them

Both packs are already in the PO project, which is where this set was taken
from:

```
PO/Content/PlanetsOnline/UI/Fonts/Inter/
PO/Content/PlanetsOnline/UI/Fonts/FontAwesome/
```

Copy the files named above across, then run the generator:

```
python tools/fonts/gen_fa_icons.py
```

That reads `CHEATSHEET_FULL.txt` and the two OTF cmap tables and writes both
halves of the icon vocabulary — `src/cromwell/ui/FontAwesomeIcons.hpp` for C++
and `src/cromwell/assets/web/cromwell_ui.css` for the browser surface. Running
it is only needed when the pack is updated; the header is committed, so a
checkout without the fonts still compiles.

## Licensing, stated plainly

**Inter** — SIL OFL 1.1. Ships in a build, redistributable, no attribution
required in-product (the licence file must travel with the font).

**Font Awesome Free** — fonts under OFL 1.1, icons under CC BY 4.0. Attribution
belongs in the credits.

**Font Awesome Pro 7.3.1** — a paid licence, bought for this developer.
Embedding the faces inside a shipped build is permitted. Redistributing the OTF
files on their own is not, and a public git remote is redistribution. That is
the entire reason this directory is ignored rather than committed.

## What happens without them

Nothing breaks. `UiFontAssets::installed()` returns false, `DevFonts::setup`
falls through to `rlImGuiSetup`, and the dev panel renders in ImGui's built-in
ProggyClean with no icons — smaller, uglier, completely usable. The log says so
on startup. No call site needs a guard, and no screenshot path needs a
different branch.
