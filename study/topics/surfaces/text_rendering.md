# Text rendering: the whole pipeline, compared

Every stage between a `.ttf` on disk and lit pixels, as five other codebases do
it, and an audit of ours against each.

Written because "the text looks blurry" was chased through six wrong answers
before landing, and most of the wrong answers were plausible. The value here is
the *stage list*: it is the thing that turns "blurry" into a question with one
answer per row.

Sources are tagged by how they were obtained. `[UE-SRC]`, `[IMGUI-SRC]`,
`[RAYLIB-SRC]` were read from source on this machine; `[SOURCE-SDK]` is headers
only (the rasteriser is closed); `[GODOT-DOC]` and `[SKIA-DOC]` are
documentation and public source, not read locally.

---

## 1. The stages

A glyph goes through all of these. Getting any one wrong reads as "blurry" or
"pixelated" and none of them announce themselves:

1. **Load** — file to face
2. **Size** — how a style size becomes a rasterisation size
3. **Hinting** — how much the outline is snapped to the grid
4. **Render mode** — greyscale, LCD subpixel, or monochrome
5. **Coverage post-processing** — gamma, contrast
6. **Atlas** — packing, padding, pixel format
7. **Sampling** — filter, mipmaps
8. **Positioning** — where the quad lands, and at what precision
9. **Quad and UVs** — the 1:1 mapping, or not
10. **Blending** — alpha mode and colour space
11. **Shaping** — kerning and ligatures
12. **Frame position** — whether anything resamples the UI afterwards

---

## 2. What each one does

### Unreal Engine 5.7 `[UE-SRC]`

Read from `Engine/Source/Runtime/SlateCore/Private/Fonts/` and
`Engine/Shaders/Private/SlateElementPixelShader.usf`.

| Stage | What it does |
|---|---|
| Size | `FT_Set_Pixel_Sizes` with an **integer** pixel size. `ComputeFontPixelSize` does the size × scale maths in 26.6/16.16 fixed point and rounds **once**, at the end (`FontCacheFreeType.cpp:119`). |
| Hinting | `EFontHinting::Default` → `FT_LOAD_TARGET_NORMAL`, i.e. **the font's own bytecode**, no `NO_HINTING`, no `FORCE_AUTOHINT`. All of UE's own UI fonts use it. Options map 1:1 to FreeType: Auto→`FORCE_AUTOHINT`, AutoLight→`TARGET_LIGHT`, Monochrome→`TARGET_MONO\|FORCE_AUTOHINT`, None→`NO_AUTOHINT\|NO_HINTING` (`SlateFontRenderer.cpp:60`). |
| AA off | Falls back to `TARGET_MONO \| FORCE_AUTOHINT` — hard pixels, no AA. |
| Atlas | Padding **1**, content type **Alpha** (A8), sampled `Texture2DSample_A8`. |
| Positioning | `FMath::RoundToInt` on the glyph offset (`SlateFontRenderer.cpp:770`). **Whole-pixel.** No subpixel positioning. |
| Gamma | **Yes**, in the Slate pixel shader — `GammaAndAlphaValues` carries a gamma curve, an inverse display gamma, and a **contrast** term. `ApplyContrast` uses a midpoint of **0.25** with a TODO about making it configurable. |
| Shaping | **HarfBuzz** (`FontCacheHarfBuzz.cpp`). Full kerning and ligatures. |
| Scalable | Ships `ST_SdfFont` and `ST_MsdfFont` shader types plus `SlateSdfGenerator.cpp`. Its SDF path forces `FT_LOAD_NO_HINTING \| FT_LOAD_NO_AUTOHINT`, because hinting would distort a scalable field. |

### Dear ImGui + FreeType `[IMGUI-SRC]`

| Stage | What it does |
|---|---|
| Hinting | `LoadFlags = 0` — **the font's own bytecode**, same as UE's Default. Flags exist for every other mode and none are set by default (`imgui_freetype.cpp:192`). |
| Render mode | `FT_RENDER_MODE_NORMAL` greyscale unless `Monochrome`. |
| Positioning | `PixelSnapH` rounds advances to integers; widget positions are integers. **Whole-pixel.** |
| Size | Since 1.92 fonts are **dynamically sized** — `PushFont(font, size)` rasterises at that size. Nothing is ever scaled. |

This is the most useful reference of the five, because it renders *the same
face in the same window* as our kit. Any difference is our integration.

### Godot 4 `[GODOT-DOC]`

Three independent settings, and its defaults:

| Setting | Options | Default |
|---|---|---|
| Antialiasing | None / **Greyscale** / LCD Subpixel | Greyscale |
| Hinting | None / **Light** / Full | Light |
| Subpixel **positioning** | Disabled / **Auto** / ½ / ¼ | Auto |

Godot is the outlier, and deliberately: it takes *precise placement* over
*hard stems*. `Auto` rasterises ¼-pixel phases at small sizes and disables them
at large ones. Costs 2–4× font cache memory (≈3 MB measured for its editor with
full Chinese localisation).

**It does not default to LCD subpixel AA**, and its docs say why: fringing,
"especially on display technologies that don't use standard RGB subpixels".

### Chromium / Skia `[SKIA-DOC]`

Not readable locally — **CEF's binary distribution ships no font source at
all**, only `include/` and the `libcef_dll` marshalling wrappers. Skia is
compiled inside `libcef.dll`. Read `google/skia` instead.

The relevant part: Skia **pre-blends gamma-corrected glyph masks and stores the
corrected values in the mask** (`SkMaskGamma` / `PreBlend` in
`SkScalerContext`). Same technique *and* same implementation point as ours —
baked at rasterisation, not applied in a shader.

### Source / TF2 `[SOURCE-SDK]`

Only `ISurface.h` is public; the rasteriser is closed. The font flag vocabulary
is the whole story: `ANTIALIAS`, `GAUSSIANBLUR`, `DROPSHADOW`, `ADDITIVE`,
`OUTLINE`, `CUSTOM`, `BITMAP`.

**There is no LCD/subpixel flag.** AA is opt-in per font, so some Source UI text
is deliberately aliased. TF2 buys sharpness with **pinned sizes per resolution**,
**integer positions**, and **outlines/drop shadows for contrast** — not with
edge fidelity.

---

## 3. The one axis they disagree on

Everything above agrees except positioning, and the disagreement is forced:

> **Native hinting and subpixel positioning are mutually exclusive.** Hinting
> snaps stems onto the pixel grid; shifting a hinted outline by a quarter pixel
> throws that away again.

So there are exactly two coherent designs:

| | Hinting | Positioning | Buys | Costs |
|---|---|---|---|---|
| **ImGui, Unreal, Source** | native, hard | integer | crisp stems | letter spacing quantises |
| **Godot** | none/light | ¼ pixel | exact spacing | softer stems |

Three of four pick the first. Committing to one silently rules out the other's
fix, which is exactly how this took six rounds: having chosen Godot's model,
native hinting was unavailable, and native hinting was the answer.

---

## 4. Audit of ours

`cromwell/ui/text/UiFontSet.cpp` and `UiPainter.cpp`.

| Stage | Ours | Verdict |
|---|---|---|
| Load | FreeType, per weight, lazily | matches all |
| Size | `FT_Set_Pixel_Sizes`, integer, rounded once via `rasterSize()` | **matches UE** |
| Hinting | `FT_LOAD_DEFAULT` — font's own bytecode | **matches UE Default and ImGui** |
| Render mode | `FT_RENDER_MODE_NORMAL`, greyscale | matches UE, Godot defaults |
| Coverage | gamma 1.45 baked into the atlas | **matches Skia's approach**; UE does it in-shader instead |
| Atlas | shelf packed, padding 1, GRAY_ALPHA (R=255, A=coverage) | **matches UE** (padding 1, A8) |
| Sampling | `TEXTURE_FILTER_POINT`, no mipmaps | correct given 1:1; UE uses Point for atlases |
| Positioning | whole-pixel, snapped **per glyph** | **matches UE's `RoundToInt` and ImGui's `PixelSnapH`** |
| Quad/UVs | quad width = rect width, scale forced to 1 | 1:1, no resample |
| Blending | straight alpha in sRGB, no `GL_FRAMEBUFFER_SRGB` anywhere | correct for text — Skia advises against linear blending for glyphs |
| Frame position | UI drawn on the backbuffer **after** `tonemap_.draw()` resolves the 2× supersampled scene | **verified** — nothing resamples the UI |

### The two real gaps

**1. No shaping, therefore no kerning.** Every reference here uses HarfBuzz
(UE) or an equivalent. We use raw FreeType advances. Inter's kerning pairs live
in **GPOS**, which `FT_Get_Kerning` cannot read — it only sees the legacy `kern`
table, which Inter does not have (checked: its table directory is
GDEF/GPOS/GSUB/cmap/glyf/… with no `kern`). So our kerning is not merely
unimplemented, it is *unreachable* without HarfBuzz. Invisible at 11px, visible
at 40px+ on pairs like `Ha`, `gl`, `ov`.

**2. No contrast control.** UE has a `DisplayContrast` term applied around a
0.25 midpoint, separate from gamma. We have gamma only. This is the dial that
makes small light-on-dark text hold its weight, and it is a genuinely different
knob from the gamma curve.

### Not gaps, but worth knowing

- **Sizes.** The kit's captions are 11px and body 12–13px. Every reference here
  runs larger by default. At 11px there are ~7 pixels of x-height; no
  rasteriser makes that look like 20px type.
- **Contrast of the content itself.** Grey-on-grey captions are low contrast by
  design, and contrast dominates perceived sharpness. This is exactly what
  Source answers with `FONTFLAG_OUTLINE` rather than by chasing edges.

---

## 5. How to diagnose this next time

1. **Size ladder** — the same string at 10→56px, in the gallery. A broken
   pipeline is wrong at *every* size; too few pixels is only wrong at small
   ones. This one test would have saved most of the six rounds.
2. **Compare against the ImGui panel in the same window** — same FreeType, same
   face, same resolution. Any difference is our integration, nothing else.
3. **Do not judge from a zoomed crop.** `NEAREST` zoom is faithful to pixels and
   wildly exaggerates both fringing and stepping. One subpixel of fringe becomes
   a four-pixel block the eye cannot fuse. Judge at 1:1, on the monitor.
4. **Check the artefact is fresh.** A build that fails to link because the
   previous instance still holds the exe leaves a stale screenshot that looks
   like a real result. Happened twice.
