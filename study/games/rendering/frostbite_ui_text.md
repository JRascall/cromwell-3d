# Frostbite UI and text rendering — Battlefield 6

How BF6 draws its menus, HUD and — the part this project actually needs — its
**text**. The third note in the Frostbite set, after
[`frostbite_rendering.md`](frostbite_rendering.md) and
[`battlefield6_netcode.md`](../shooters/battlefield6_netcode.md), and read from
the same evidence: the shipped executable.

The short answer, and the reason this note exists: **BF6 runs three separate UI
stacks, and renders text at least two different ways.** One of them is a
GPU-generated signed-distance-field pipeline with a subpixel option, which is the
*opposite* branch from the one this project took for crisp text — see §5.4, which
is the section worth reading if you read only one.

| tag | source |
|---|---|
| **[BIN]** | **Our own read of the shipped `bf6.exe`** — 195,618,664 bytes, file-stamped **2025-08-11**, retail Steam install. Plain string dump: no injection, no code run, no game data decrypted. Categorised UI extract (2,842 lines, nine sections) in [`frostbite_bf6_ui_strings.txt`](frostbite_bf6_ui_strings.txt). Limits in §0.1. |
| **[inferred]** | Our reading, not EA's. Used heavily. Always marked. |

**EA appear to have published nothing at all about Frostbite's UI or text
rendering.** Nothing in the SIGGRAPH Advances course 2010–2026, nothing on
ea.com/frostbite or ea.com/technology, no GDC session located. See §7.

---

## 0. Scope and the traps

### 0.1 What [BIN] supports

Same rules as [`frostbite_rendering.md`](frostbite_rendering.md) §0.2. A symbol
name proves a thing was **compiled into this build** — never that it runs, never
its value, never its buffer format. **No frame was captured**, so there are no
atlas dimensions, no glyph cache sizes and no costs in this document. Every
number-shaped claim below is a *setting name*, not a setting value.

### 0.2 Two traps

**`IR_VERSION` is not the `IR_*` UI shaders.** The binary contains
`IR_VERSION_2017_10_10`, `IR_VERSION_2021_7_30` and similar — these are **ONNX
Runtime** intermediate-representation version constants and have nothing to do
with the `ShaderProgram_IR_*` family. A naive grep for `IR_` merges an ML runtime
with a UI renderer. The companion strings file excludes them.

**Unicode script names are not evidence of a shaping engine.** The binary
contains `Egyptian_Hieroglyphs,Egyp`, `Anatolian_Hieroglyphs,Hluw` and the rest
of the ISO 15924 script table. That is a script-code table — it does **not** by
itself demonstrate complex-script shaping, bidi, or an ICU/HarfBuzz integration.
We found no `harfbuzz`, `icu` or `bidi` symbols. §5.5 records this as unresolved
rather than reading the table as a capability.

---

## 1. Three stacks, not one

| stack | what it is | programs | evidence [BIN] |
|---|---|---|---|
| **Twinkle** | A **web-technology UI runtime**: JavaScript on QuickJS, WebAssembly on Wasm3, HTTP caching, asset signing, remote fonts. Canvas-shaped drawing API. | 68 | `Twinkle.ScriptEngineBackend`, `Twinkle.QuickJsGcThresholdMb`, `Twinkle.Wasm3StackSizeKb`, `Twinkle.HttpCacheEnabled`, `Twinkle.EnableAssetSigning` |
| **Rime** | An **ECS-native retained-mode widget toolkit** with a vector/path renderer and analytic antialiasing. | 24 (+15 `IR_*`) | `EcsRimeLabel`, `EcsRimeCanvasLayoutNode`, `EcsRimeStackLayoutNode`, `EcsRimeUniformGridLayoutNode`, `EcsRimeMasking`, `EcsRimeBlur` |
| **`Ui_*`** | A **native purpose-built layer** for maps, compass and world-anchored markers. | 51 | `Ui_Minimap*`, `Ui_Bigmap*`, `Ui_Compass`, `Ui_AttentionTriangle`, `Ui_Textured_WorldDepth` |

Plus **`Photon`**, which is not a renderer at all but the **platform-services
bridge** exposed to Twinkle's JavaScript (§2.2), and **`BFUI`**, DICE's
game-specific UI layer built on Rime (§3.2).

### 1.1 Which stack draws what

**Rime draws the HUD — this is evidenced, not inferred.** The link is direct:
`BFUIRimeTextureManager`, `BFUIRimeTextureLoader`,
`DiceUI.BFUIRimeTextureManager::update` [BIN]. DICE's `BFUI` layer — crosshair,
vehicle HUD, HUD shake, 3D minimap — is built on Rime.

**Twinkle is [inferred] the front end.** The inference rests on what its API is
*for* rather than a naming link: its JS bindings are `Badging`, `BlockService`,
`PhotonInvite`, `OpenPlatoons`, `OpenPlayerProfile`, `Accessibility`; it has an
HTTP cache, asset signing, async response validation and server-delivered fonts.
That is a menu/store/social surface, not a 60 Hz HUD. **We did not confirm this
by observation** and the strings contain no `Twinkle.*Hud*` or `Twinkle.*Menu*`
symbol either way.

---

## 2. Twinkle — the scripted UI runtime

### 2.1 It is a browser in miniature

The engine list alone makes the point [BIN]:

```
Twinkle.ScriptEngineBackend          quickjs / QuickJsAllocatorWrapper
Twinkle.QuickJsGcThresholdMb         Twinkle.CallGcEveryNumFrames
Twinkle.QuickJsStackSizeKbClang      Twinkle.QuickJsStackSizeKbMsvc
Twinkle.Wasm3StackSizeKb
Twinkle.HttpCacheEnabled             Twinkle.HttpCacheWritesEnabled
Twinkle.EnableAssetSigning           Twinkle.ValidateAsyncResponses
```

**QuickJS for JavaScript, Wasm3 for WebAssembly, an HTTP cache, and asset
signing.** [inferred] Asset signing plus HTTP write-caching plus
`Font.useFontFromGlobalServer` says the front end is **remotely updatable** —
UI content can be delivered over the network, which is why it must be signed and
why responses must be validated. `Twinkle.ScriptEngineBackend` being a *setting*
implies more than one backend has existed.

Development affordances are unusually thorough [BIN]: `Twinkle.LogJs`,
`Twinkle.TrackJsFunctionCalls`, `Twinkle.TrackJsHeapAllocations`,
`Twinkle.UseCpuTimersInJs`, `Twinkle.ScriptAllocatorBudgetAssert`,
`Twinkle.TimeScale`, `Twinkle.DebugRender`, `Twinkle.DebugDrawCallStep`,
`Twinkle.DebugLayerStep`, `Twinkle.DebugRenderTreeZIndexOnly`.
`DebugDrawCallStep` and `DebugLayerStep` [inferred] step through the frame one
draw call or one layer at a time — the UI equivalent of a frame debugger, built
in.

### 2.2 The JavaScript API surface

Drawing is **HTML5-Canvas-shaped** [BIN]:

```
TwinkleJsBindings.Canvas.fillText          .drawImage
TwinkleJsBindings.Canvas.fontSize          .fontFamily      .fontFamilyFallbacks
TwinkleJsBindings.Texture.createDynamicAtlas   .destroyAtlas
TwinkleJsBindings.Utilities.loadFontAsync
TwinkleJsBindings.Utilities.setGlyphSamples    .setGlyphTextureSize
```

Text is a first-class object with a full editing/measurement stack [BIN]:

```
TwinkleJsBindings.Surface.createOrUpdateRichTextObject
TwinkleJsBindings.Surface.removeRichTextObject
TwinkleJsBindings.Surface.getTextMeasurements
TwinkleJsBindings.Surface.getTextSelectionRects
TwinkleJsBindings.Surface.getTextCharacterIndexAtPosition
TwinkleJsBindings.Clipboard.getText / .setText
```

`getTextCharacterIndexAtPosition` is **caret hit-testing** and
`getTextSelectionRects` is **selection highlighting** — together with the
clipboard bindings, that is a real text-input implementation, not a label
renderer. [inferred]

**Photon** is the separate platform bridge, exposing services rather than
drawing [BIN]: `Photon.UpdateFlowUrl` / `FallbackUpdateFlowUrl` /
`UpdateFlowProtocol`, `PhotonInterfacePlayerAction_AcquireMultiplayerPrivilege`,
`…JoinGame`, `…OpenPlatoons`, `PhotonJsBindings.Accessibility.narrate`,
`PhotonJsBindings.BlockService.*`, `PhotonJsBindings.Badging.*`.
`Accessibility.narrate` is a screen-reader hook. [BIN]

---

## 3. Rime — the widget toolkit

### 3.1 Layout

A conventional retained-mode tree with typed layout nodes [BIN]:

| node | modes |
|---|---|
| `EcsRimeCanvasLayoutNode` | absolute positioning |
| `EcsRimeStackLayoutNode` | `StackItemAlignment_Start` / `_Center` / `_End` |
| `EcsRimeUniformGridLayoutNode` | `UniformGridFillMode_RowsFirst` / `_ColumnsFirst`; `UniformGridCellCountMode_Fixed` / `_AddAsNeeded` |
| `EcsRimeLegacyStackLayoutNode` | the superseded stack, still shipping |

with a general axis alignment enum `RimeElementAxisAlignment_Start` / `_Center` /
`_End` / `_Stretch` / `_Invalid`, and layout as an explicit ECS phase —
`EcsRimeUpdatePasses_UiPostLayout`,
`EcsRimeUpdatePasses_UiInternal_Finalize_1_PostLayout`. [BIN]

Widgets present: `EcsRimeLabel`, `EcsRimeLine`, `EcsRimeBorder`,
`EcsRimeMasking`, `EcsRimeBlur` / `EcsRimeBlurScreen`, `EcsRimeFlipbook`,
`EcsRimeDisplay`, `EcsRimeTexture`, `RimeBulkScrollingText`,
`EcsRimeTextInputSystemData`, and a radial progress bar with its own segment
alignment enum. Screens are **editor-authored**:
`EcsRimeAuthoredScreenSystemComponent`, `EcsRimeEditorScreenSystemComponent`.
[BIN]

> **This is the family cromwell's UI kit already belongs to.** Typed layout nodes
> plus `Start`/`Center`/`End`/`Stretch` alignment plus a retained tree is the
> Slate/WPF lineage recorded in the PO-project UI reference. Rime is useful here
> less as something to copy than as confirmation that a shipped AAA HUD uses the
> same shape — and for the two things it does that a naive port would not (§3.3,
> §4).

### 3.2 BFUI — the game layer on top

DICE's own layer, and the evidence that Rime is the in-game stack [BIN]:
`BFUIRimeTextureManager`, `BFUIRimeTextureLoader`, `BFUIVehicleHUDWidgetProvider`,
`ClientBFUICrosshairHudDataProviderEntity`, `BFUI3DMinimap*`,
`BFUIHudShakeCategory_CameraShake` / `_ViewSway` / `_AimTracking` with
`HudShakeFilterType_All` / `_Exclude` / `_OnlyInclude`.

[inferred] **HUD shake is a categorised, filterable system** — the HUD can be
shaken by camera shake, view sway or aim tracking independently, and individual
elements can opt in or out. That is an accessibility and readability control as
much as a juice one.

### 3.3 The vector renderer

Rime draws **paths with analytic antialiasing**, not just quads [BIN]:

```
Rime_AntialiasedPath          Rime_AntialiasedPathV2
Rime_TexturedAntialiasedPath  Rime_TexturedAntialiasedPathV2
Rime_AntialiasedPathDevicePixels
Rime_SinglePixelLinePath
Rime_Solid  Rime_Masked  Rime_Masked_Inverted  Rime_Textured
Rime_BindlessVBCombinedFill   Rime_BindlessVBCombinedScreenSpace
Rime_TexYCrCb  Rime_TexYCrCbAlpha
Rime_CsGaussianBlurTiled  Rime_CsGaussianBlurTiledCull
Rime_CsSdrConversionTiled  Rime_CsSdrConversionTiledCull
```

with path styles from the BFUI side: `BFUIAAPathStyle_Open` / `_Closed` /
`_DrawInside` / `_DrawOutside` / `_DrawLeftSide` / `_DrawRightSide`. [BIN]

Three details worth noting [inferred]:

- **`AntialiasedPathDevicePixels`** — a variant that works in device pixels
  rather than logical units. That is the "snap to the physical grid" escape hatch
  a resolution-independent vector renderer needs for hairlines.
- **`SinglePixelLinePath`** — a dedicated one-pixel-line shader, because a
  general AA path shader cannot make a crisp 1 px line.
- **`CsSdrConversionTiled`** — the UI is composited in HDR and converted to SDR
  in a tiled compute pass, with a `Cull` variant that skips empty tiles.

An **`IR_*` family mirrors `Rime_*` almost exactly** —
`IR_AntialiasedPathV2`, `IR_TexturedAntialiasedPath`, `IR_BindlessVBCombinedFill`,
`IR_Masked`, `IR_Solid`, `IR_Textured`, `IR_SinglePixelLinePath`,
`IR_CsGaussianBlurTiled` — **plus two Rime does not have: `IR_Dashed` and
`IR_Striped`**. What `IR` stands for is **unknown and not guessed at here**.
[BIN]

---

## 4. The native `Ui_*` layer

51 programs that are neither Twinkle nor Rime, covering the things with genuinely
special requirements [BIN]:

| group | programs |
|---|---|
| **Minimap / bigmap** | `Ui_Minimap`, `_Terrain`, `_Element`, `_Overlay`, `_Shadow`, `_InteriorFeedback`, `_Post_Proc`, `_Debug`, `Ui_DetailTexturedMinimap`, `Ui_MinimapIcon`, `Ui_Bigmap`, `_Terrain`, `_Element`, `_Element_Opaque`, `_Post_Proc` |
| **World-anchored** | `Ui_Compass`, `Ui_AnchoredLine`, `Ui_AttentionCircle` / `_Square` / `_Triangle` |
| **Depth-tested UI** | `Ui_Textured_WorldDepth`, `Ui_TexturedNoPremult_WorldDepth`, `Ui_TexYCrCb_WorldDepth`, `Ui_TexYCrCbAlpha_WorldDepth` |
| **Distance-field icons** | `Ui_TexturedDistanceField`, `Ui_TexturedDistanceFieldClip`, `Ui_HardwareDfIcon`, `Ui_HardwareDfIconSingle` |
| **Primitives / effects** | `Ui_AntialiasedCircle`, `Ui_AntialiasedLine`, `Ui_Solid`, `Ui_Blur`, `Ui_Glitch`, `Ui_Mosh`, `Ui_Loader`, `Ui_LoaderPulse`, `Ui_LoadingEffect`, `Ui_Slideshow`, `Ui_Slideshow_Transition` |

**Two things here are directly reusable.**

**`_WorldDepth` variants** — UI depth-tested against the scene depth buffer, which
is how a world-anchored marker gets occluded by the wall in front of it without
the UI leaving screen space. Four variants exist because the blend and colour
space differ (premultiplied, non-premultiplied, YCrCb, YCrCb+alpha). [BIN] +
[inferred]

**`Ui_TexYCrCb*` across all three stacks** — video decodes to YCrCb and is
converted **in the UI shader**, never round-tripping through an RGB intermediate.
[inferred] For a menu background loop or a killcam replay that is one full-screen
resolve saved per frame.

---

## 5. Text

### 5.1 The engine underneath

**EAText on FreeType.** The string `EAText/FreeType` is in the binary, with
`GlyphMetricsMap`, `KerningMap`, `numFontFaces`, `FontFamilyRequest`,
`FontFamilyFallbacksRequest`, `requireFontFamilyMatch`. [BIN] Rime loads TTFs
directly (`RimeTtfFontFile`) into a dedicated allocator (`"Rime Font Arena"`).

Font **fallback chains** are first-class on both sides —
`Canvas.fontFamilyFallbacks`, `FontFamilyFallbacksRequest` — and
`Font.canTextBeRenderedInFont` [BIN] is the query that drives them: ask whether a
string is representable before committing to a face. [inferred]

### 5.2 Twinkle text is GPU-generated SDF

The whole pipeline is visible [BIN]:

| symbol | reading |
|---|---|
| `Twinkle.EnableTextSDF` | SDF text on/off |
| `Twinkle.GpuGenSDF` | distance fields generated **on the GPU** |
| `Twinkle.EnableTextSDFGen4`, `Twinkle.GpuGenSDFGen4` | a **fourth-generation** SDF generator, shipping beside the older one |
| `Twinkle_GenSdf_Inside`, `Twinkle_GenSdf_Outside` | two-sided field generation |
| `Twinkle/GlyphGenSdf_Inside`, `…_Outside` | the glyph-specific entry points |
| `Twinkle.TextEdgeOffset` | where the edge sits in the field |
| `Twinkle.TextSoftness` | edge falloff |
| `Twinkle.TextSubPixel` | subpixel antialiasing toggle |
| `Twinkle.FontQuality` | a quality tier |

Every textured shader ships in a matrix of variants — for each of six texture
slots (§6): plain, `GrayScale`, `Sdf`, `SdfGrayScale`, `SdfSubPixel`,
`SdfSubPixelGrayScale`, `WithBorder`, `WithBorderAndGradient`, `WithMask`,
`WithMaskGrayScale`. [BIN] So **grayscale vs subpixel AA, and bordered vs plain
text, are permutation choices resolved at draw time**, not runtime branches.

### 5.3 The glyph cache degrades gracefully

[BIN]:

```
TwinkleGlyphCache        Twinkle.GlyphCacheSize
MinimumGlyphCacheSize    MaximumGlyphCacheSize
Twinkle.FontQuality      Twinkle.DebugGlyph
TwinkleJsBindings.Metrics.onFontQualityLoweredBecauseGlyphCacheFull
TwinkleJsBindings.Utilities.setGlyphSamples    .setGlyphTextureSize
```

**There is a telemetry event named
`onFontQualityLoweredBecauseGlyphCacheFull`.** [inferred] That is a designed
response, not a bug: when the atlas fills, quality drops rather than glyphs
failing to draw — and DICE measure how often it happens in the field. The JS
layer can also retune the rasteriser live via `setGlyphSamples` (supersampling)
and `setGlyphTextureSize` (atlas dimensions).

Rime keeps a **separate** cache: `RimeGlyphCache`, `RimeGlyphCacheSettings`,
`RimeGlyphCacheEntity`, `EcsRimeGlyphCacheSystemState`. [BIN] Two UI stacks, two
glyph caches, two memory budgets.

Rime's glyph path also looks different — `PolygonGlyph`, `DrawGlyph`,
`DrawGlyphBrushOutline`, `DrawGlyphSmearOutline`, `SetGlyphSmooth`,
`SetGlyphMinAlpha`, `SetGlyphSamples`, `SetGlyphBrush`, `SetGlyphHSpace` /
`SetGlyphVSpace`, `RimeVerticalTextAlignment_GlyphTop` / `_GlyphCenter`. [BIN]
[inferred] `PolygonGlyph` alongside an analytic-AA path renderer (§3.3) suggests
**outline-to-polygon tessellation** rather than SDF sampling — which would make
Rime's text resolution-independent by a different route. **Not confirmed**: no
symbol states it, and `Ui_TexturedDistanceField` shows distance fields exist
elsewhere in the UI too.

### 5.4 The part that matters for this project

This is a genuine fork in the road, and Frostbite took the branch this project
did not.

| | Frostbite / Twinkle | this project's crisp-text recipe |
|---|---|---|
| glyph representation | **GPU-generated SDF** | **native-hinted raster** |
| hinting | none — SDF is inherently unhinted | **on** |
| positioning | free, subpixel | **whole-pixel placement** |
| antialiasing | subpixel or grayscale, selectable | grayscale, hinting-compatible |
| edge control | `TextEdgeOffset`, `TextSoftness` | none needed |
| scales continuously | **yes** | no — a size ladder |

**Neither is wrong; they answer different questions.** Frostbite's UI must scale
to arbitrary resolutions and DPI, animate, rotate, and be authored by designers
in JavaScript against a Canvas API — an SDF is the only representation that
survives all of that, and the subpixel/softness/edge-offset knobs are how you
claw back the sharpness SDF costs you at small sizes. This project renders a
fixed desktop UI at a known size ladder, where **hinting plus whole-pixel
placement gives strictly crisper small text than any SDF will**, and the
hinting-versus-subpixel-positioning exclusivity already recorded here is exactly
the constraint an SDF sidesteps by giving up hinting entirely.

**The transferable observation is not the technique but the trigger:** the moment
UI text needs to scale continuously or animate, the hinted-raster recipe stops
being available and something SDF-shaped becomes necessary. Frostbite is what
that costs — a four-generation SDF generator, a GPU generation pass, two AA
modes, three tuning knobs, and a quality-degradation path with its own telemetry.

### 5.5 Unresolved: complex script shaping

**We could not determine whether Frostbite does complex-script shaping.** No
`harfbuzz`, `icu`, `bidi` or `linebreak` symbol was found. The binary does carry
the ISO 15924 script-code table, but as §0.2 warns, a script table is not a
shaper. EAText historically had its own layout and shaping, so it may simply be
statically linked with stripped names — but that is speculation and is not
asserted here.

---

## 6. Batching

The **`Textured0` … `Textured5`** ladder is the tell: **one shader permutation
per number of bound textures**, so a single Twinkle draw can span up to **six
atlas pages** before it must break. [BIN] + [inferred] Supported by
`Texture.createDynamicAtlas` / `destroyAtlas` and an atlas debug viewer
(`Twinkle.DebugAtlas`, `DebugAtlasPosX` / `PosY` / `Scale`,
`DrawDebugAtlasPageIndex`).

Rime takes the other route: **bindless** — `Rime_BindlessVBCombinedFill`,
`Rime_BindlessVBCombinedScreenSpace`, `Rime_EcsBindlessVBCombinedFill` /
`…ScreenSpace` / `…YCrCb`. [BIN] [inferred] With bindless there is no
texture-count permutation problem at all, which is presumably why the `IR_`/Rime
family has no `Textured0..5` ladder.

[inferred] **Two stacks, two batching strategies, chosen by age or by
constraint** — the permutation ladder is the portable answer, bindless the modern
one. Notable that both ship.

---

## 7. What could not be found

| topic | status |
|---|---|
| **Any EA publication on Frostbite UI or text rendering** | **Appears not to exist.** Searched the SIGGRAPH Advances index 2010–2026, ea.com/frostbite and ea.com/technology news, EA SEED publications, and GDC listings. Nothing on UI, text, glyph rendering or the Twinkle/Rime systems. Consistent with the netcode absence — EA publish rendering and physics, not systems. |
| **What "Rime", "Twinkle", "Photon" and "IR" stand for** | **Unknown.** Codenames. `IR_*` is a near-clone of `Rime_*` with two extra shaders; we do not guess. |
| **Whether complex-script shaping exists** | **Unresolved** — §5.5. |
| **Every size and budget** — atlas dimensions, glyph cache sizes, `FontQuality` tiers, SDF field resolution, glyph sample counts | **Names only.** Values live in data, not the executable. |
| **Whether Rime text is tessellated outlines or SDF** | **Inferred, not confirmed** — §5.3. |
| **Twinkle-vs-Rime usage split** | **Half evidenced.** Rime→HUD is proven by `BFUIRimeTextureManager`. Twinkle→front-end is inference from its API surface only. |
| **Any cost** | **Not measured.** No capture was taken; see [`frostbite_rendering.md`](frostbite_rendering.md) §0.1 for why. |

> **Cheapest upgrade to this note**, in order: `Twinkle.DebugAtlas` and
> `Twinkle.DebugGlyph` are shipped debug views. If they can be reached from the
> retail build's console they would give atlas dimensions and glyph residency
> directly — no capture, no anti-cheat exposure. We did not attempt this and do
> not know whether the console is available in retail.

---

## 8. What is worth taking

1. **`AntialiasedPathDevicePixels` and `SinglePixelLinePath` — the two escape
   hatches.** [BIN] §3.3 A resolution-independent vector UI *still* needs a
   device-pixel path and a dedicated 1 px line shader, because general AA cannot
   produce a crisp hairline. If cromwell's UI kit ever goes vector, these are the
   two things that will be discovered late and painfully.

2. **Depth-test world-anchored UI against the scene.** [BIN] §4 `Ui_*_WorldDepth`
   keeps markers in screen space while letting geometry occlude them. Cheap, and
   the alternative (projecting UI into the world) is much worse.

3. **Degrade font quality rather than failing.** [BIN] §5.3
   `onFontQualityLoweredBecauseGlyphCacheFull` — when the glyph atlas fills,
   lower quality and emit a metric. The instinct generalises: a cache that can
   overflow should have a designed degradation and a counter, not an assert.
   Same shape as the netcode note's `NetObjectOverflow` and
   `LargeNetObjectTrackerMode_Warning`/`_Assert`/`_Fatal`.

4. **Convert video in the UI shader.** [BIN] §4 `TexYCrCb` variants everywhere —
   no RGB intermediate for menu video or replays.

5. **Composite UI in HDR, convert to SDR in a tiled compute pass with an empty-
   tile cull.** [BIN] §3.3 `Rime_CsSdrConversionTiled` / `…TiledCull`. UI is
   mostly empty screen; culling empty tiles is free money.

6. **Categorised, filterable HUD shake.** [BIN] §3.2 Camera shake, view sway and
   aim tracking as separate categories with per-element include/exclude. This is
   an accessibility control as much as a feel one, and retrofitting it means
   touching every HUD element.

7. **Keep the debug frame-stepper.** [BIN] §2.1 `Twinkle.DebugDrawCallStep` and
   `DebugLayerStep` step the UI frame one draw or one layer at a time. For a UI
   kit this is worth more than a profiler zone, because UI bugs are ordering and
   overdraw bugs.

8. **What not to copy:** the Twinkle stack itself. A JS engine, a WASM runtime,
   an HTTP cache and asset signing exist to let a live-service front end ship
   content without a client patch. This project has no such requirement, and the
   cost — a scripting VM in the UI path, plus the signing and validation the
   remote delivery then forces — is enormous relative to the benefit.

---

## 9. Provenance

- [BIN] evidence read from `bf6.exe`, 195,618,664 bytes, file-stamped
  **2025-08-11**, retail Steam install. Same dump as
  [`frostbite_rendering.md`](frostbite_rendering.md) and
  [`battlefield6_netcode.md`](../shooters/battlefield6_netcode.md); the binary
  was read once and mined three times.
- Categorised UI extract — 2,842 lines, nine sections — in
  [`frostbite_bf6_ui_strings.txt`](frostbite_bf6_ui_strings.txt). ONNX
  `IR_VERSION` noise and the Unicode script table excluded (§0.2).
- **No frame capture, no process injection, no debug view exercised.**
