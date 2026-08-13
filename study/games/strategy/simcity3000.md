# SimCity 3000 — 110,000 pre-rendered sprites and nobody home

Deep dive on **SimCity 3000 / SimCity 3000 Unlimited** (Maxis/EA, 1999–2000):
the last 2D SimCity, and the last one whose entire visual fidelity was bought
offline. How a software-blitted isometric city gets macro and micro terrain
detail at four zoom levels and four rotations, how tens of thousands of vehicle
and pedestrian sprites are animated, how the traffic is simulated — and the
answer to "how did they handle the data for each person", which is that **there
is no person, and there is no data**.

> **On sources.** Nothing useful was published. Maxis gave no GDC talk, wrote no
> postmortem, and the team scattered after the EA acquisition. What exists
> instead is the **retail install**, which turns out to be unusually legible:
> the game ships **68 plain-text rule files**, **plain-text sprite indexes with
> Maxis's own dimension comments**, and one configuration file whose comment
> names the art director and gives the exact 3D Studio Max lighting rig every
> building in the game was baked under. This note is mostly read out of that.
>
> Tags: **[SHIPPED-DATA]** measured from the retail install on this machine —
> `C:\Program Files\GOG Galaxy\Games\SimCity 3000 Unlimited`, DLLs timestamped
> **19 April 2000**, 607 MB. **[MAXIS]** Maxis's own words, in a comment or
> header inside a shipped file — the strongest tier here and the reason the note
> exists. **[SC4]** read from SimCity 4's successor pipeline, used only to show
> where the lineage went. **[COMMUNITY]** fan documentation. **[inferred]** our
> reading. Parsers used are in §10.

**Why this game is in a study directory for a tile-based tactics prototype.** It
is the closest published relative of what this project is: a fixed-projection
tile world with a height field, per-tile simulation layers, and far more sprites
on screen than a 1999 CPU had any business blitting. It is also the cleanest
available case of a decision this project keeps circling — **bake the lighting
or run it** — taken to its absolute limit, with the costs and the freedoms both
fully visible.

Related: [`factorio.md`](factorio.md), the other pre-rendered-sprite note and the
direct comparison throughout — §3 there is the same pipeline twenty years later;
[`ruse.md`](ruse.md) §4.4 for impostors, which is what SimCity 4 turned these
sprites into; [`lod_systems.md`](../../topics/world/lod_systems.md) for the four
meanings of LOD, of which this game uses exactly one;
[`crowd_scale.md`](../../topics/scale/crowd_scale.md), whose thesis this game is
the limit case of.

---

## 1. What the install says before you open anything

### 1.1 The module list is the architecture

**[SHIPPED-DATA]** `Apps/` holds `SC3U.exe` (1.1 MB) and 24 DLLs. Every one of
them exports **exactly one symbol** — verified with `dumpbin -exports` on
`STRTSIM.DLL`:

```
    ordinal hint RVA      name
          1    0 00024389 GZDllGetGZCOMDirector
```

That is **GZCOM**, the Maxis/EA component framework, and it is the same framework
SimCity 4 shipped on three years later. The DLL names are the subsystem list:

| DLL | KB | What it is |
|---|---|---|
| `STRTSIM.DLL` | 233 | **street sim** — the traffic and pedestrian layer |
| `SIMRCI.DLL` | 393 | residential/commercial/industrial demand |
| `SIMNTWRK.DLL` | 233 | network connectivity — power, water, roads as graphs |
| `SIMSPR.DLL` | 512 | sprites |
| `SIMBABLD.DLL` | 528 | Building Architect |
| `SIMGEOM.DLL` | 221 | geometry / terrain |
| `SIMECO.DLL` | 151 | economy |
| `SIMDSTR.DLL` | 266 | disasters |
| `SIMDIRT.DLL` | 167 | terrain surface |
| `SIMADV.DLL` | 253 | advisors |
| `GIMEX.DLL` | 119 | EA's shared image codec library |
| `GZ*D.DLL` × 7 | | the GZCOM framework layer |

Linked against `MSVCP60.DLL` / `MSVCIRT.DLL` — **Visual C++ 6.0**.

**[inferred]** Two things follow. First, `STRTSIM` being its own component, sized
between `SIMECO` and `SIMSPR`, tells you the traffic layer is a real subsystem
and not a rendering flourish. Second, the names are otherwise **stripped** —
`STRTSIM.DLL` yields 437 strings and none of them is an identifier. There is no
symbol table to mine here, which is why the rest of this note is data files
rather than code.

### 1.2 It is a DirectDraw blitter

**[SHIPPED-DATA]** GOG ship a `DDrawCompat.ini` shim, which is itself the
confirmation:

```ini
FpsLimiter              = msgloop(45)
FullscreenMode          = borderless
SupportedREsolutions	= 640x480, 800x600, 1024x768, 1280x720, 1360x768, 1920x1080
```

No 3D device, no texture upload path, no shader anything. The frame is a
software composite of palettised sprites into a DirectDraw surface. **[inferred]**
Everything in §2 and §3 follows from that one fact: if you cannot light a pixel
at runtime, every lighting decision must have already happened, and if the CPU
must touch every pixel it writes, the only lever on cost is *how many pixels
exist*.

### 1.3 Where the 607 MB went

**[SHIPPED-DATA]**

| | |
|---|---|
| `Apps/Res/Sprites/` | **232 MB** |
| `Apps/Res/UI/` | 37 MB |
| `Apps/Res/BA/` | 36 MB |
| `Apps/Res/Text/` (7 languages) | 19 MB |
| `Apps/Res/SSimData/` | 2.1 MB |
| `Apps/Res/TilingRules/` | 536 KB |
| all executable code | **~7 MB** |

**[inferred] The game is 33× more art than program**, and that ratio *is* the
thesis. Everything the game knows about how a city looks was decided by an artist
in 3D Studio Max and frozen. The program's job is to pick the right frozen image
and blit it.

---

## 2. The lighting rig, which is the best thing in the install

**[MAXIS]** `Apps/Res/BA/BATConfig.ini` ships with this comment intact:

```ini
[Lights]

; There are six lights in the 3DS scene Ocean set up for 3K buildings.
; All lights are "free directional."
;
; 1) pointing in the +Y direction, RGB 98,98,133, multiplier=1.3
; 2) pointing in the -Y direction, RGB 98,98,133, multiplier=1.3
; 3) pointing in the +X direction, RGB 255,249,232, multiplier=1.3
; 4) pointing in the -X direction, RGB 255,249,232, multiplier=1.3
; 5) pointing downward ~15 degrees off of vertical in the -Y,+X direction;  RGB 198,198,198, multiplier=0.75
; 6) pointing downward ~15 degrees off of vertical in the +Y,-X direction;  RGB 198,198,198, multiplier=0.75
```

"Ocean" is **Ocean Quigley**, SimCity 3000's art director. This is the actual
rig, shipped, in the retail game, twenty-six years later.

### 2.1 Read it

Six directional lights and **no sun**:

- **±Y**: a matched pair, cool blue-grey `98,98,133`, multiplier 1.3.
- **±X**: a matched pair, warm off-white `255,249,232`, multiplier 1.3.
- **±(15° off vertical)**: a matched pair of neutral top fills `198,198,198`,
  multiplier 0.75, pointing in exactly opposite horizontal directions.

**[inferred] Every light has an equal and opposite twin.** That is not an
aesthetic choice, it is a constraint solve. The city rotates in 90° steps — the
shipped GUI strings include `Rotate Clockwise` and `Rotate Counter Clockwise` —
and each rotation is a **separate baked image of the same building** (§3). Four
bakes of one object have to agree with each other or the building will appear to
be lit from a different direction depending on which way you are facing. A single
sun makes that impossible: the shadowed face in one rotation is the lit face in
another.

So they deleted the sun. What is left is a **four-fold symmetric cross** with
warm on one axis and cool on the other, which is enough to separate the four
vertical faces of a box by hue and value without any of them being "the dark
side". The two opposed top lights round the roofs without picking a direction.

**[inferred] The consequences are exactly what you see in the game.** SimCity
3000's buildings have no cast shadows on the ground, no shadow from one building
onto another, and no time-of-day. They also never look wrong when you rotate,
which is the trade that was actually being made. *The lighting model is not a
simplification of a sun; it is the most information you can bake into an object
that must look correct from four directions at once.*

This is the same rule [`factorio.md`](factorio.md) §3.4 arrives at from the other
side — Wube bake everything because their objects sit at a fixed angle under a
fixed sun, and the one entity that tumbles freely (a Space Age asteroid) needed a
real normal-mapped shader written specially for it. **Baked lighting survives
exactly as long as the orientation is fixed.** SimCity 3000 has four
orientations rather than one, and paid for the extra three by giving up the sun.

### 2.2 There are no normals and no depth

**[inferred, from the absence]** There is nowhere for them to live. A sprite is a
palettised image with a mask; the container (§3.1) carries a registration point
and nothing resembling a per-pixel depth or normal channel. There is no runtime
light. The night sky in SimCity 3000 is not a relight — **[MAXIS]** SimCity 4's
successor tool describes the mechanism the lineage used, "textures to windows
which gives the impression of lit windows during the night-time renders of the
model", i.e. **a second complete bake**, not a darkening pass.

### 2.3 The rest of the file: the tool had three lighting modes

**[MAXIS]** The same `BATConfig.ini` defines three separate five-light sets for
the Building Architect's own viewport — `Construct*` (164/72/164/72/141),
`Texture*` and `Preview*` (both 190,174,164 / 71,71,91 / … / 141,141,141) — the
last commented "*used in file mode, and to preview and render the result for
SimCity*". The BAT is a **block builder**: the colour table names a block palette,
left/right/flat editing planes, `ConstructShadow` projected onto shadow planes,
an isometric volume outline, and separate props and texture palettes.

**[inferred]** Preview and Texture sharing a light set while Construct differs is
the tell that Construct mode is deliberately *flatter* — you want unambiguous
face separation while placing geometry, and the real rig only when judging the
result.

---

## 3. Sprites: the numbers, and what LOD means here

### 3.1 One container format, and it is DBPF's ancestor

**[SHIPPED-DATA]** Every data file in the game — `.IXF` tables, `.DAT` sprite
archives — uses one format. Magic `D7 81 C3 80`, then a dense directory of
20-byte little-endian records:

```
(type, instance, group, offset, size)
```

That is **TGI**, the same triple SimCity 4's DBPF archives are keyed by. The
directory is allocated in 1024-slot blocks with unused slots zeroed; payload is
95–99% of file size.

Alongside them, **[MAXIS]** ships plain-text indexes with a self-describing
header:

```
;  SimCity3000 Image Info
;  2/23/2000 1:36:17 PM
;  This file should be formatted as follows:
;     Version:      [Version]
;     Record Count: [Record Count]
;     [ImageGroup], [ImageInst], [Span L (reg pt x)], [Span T (reg pt y)], [Span R], [Span B]
;     ...
;  Comments can only be used at the top of the file. Do not leave blank lines anywhere
```

So a sprite carries an id and a **registration point given as four spans** from
that point to the image edges. For a 32 × 75 sprite the record is
`0, 66, 32, 9` — L+R = 32, T+B = 75, and the anchor sits **9 px above the
bottom edge**, which is the tile origin the building stands on. The anchor is
authored, not derived, which is what lets a 600 px skyscraper and an 8 px patch
of grass be placed by the same code.

### 3.2 The counts

**[SHIPPED-DATA]** Indexing every archive:

| Archive | sprites | bytes |
|---|---|---|
| `00000006_Vehicles.DAT` | **19,008** | 7.6 MB |
| `00000008_Other.DAT` | 18,312 | 28.7 MB |
| `54AACF44_Holiday.DAT` | 16,080 | 31.7 MB |
| `00000005_Roads.DAT` | 13,352 | 15.2 MB |
| `00000007_People.DAT` | **10,240** | 1.4 MB |
| `0000000B_Utilities.DAT` | 5,488 | 9.3 MB |
| `00000015_Boats.DAT` | 4,896 | 12.8 MB |
| `00000004_Industrial.DAT` | 4,640 | 8.6 MB |
| `00000003_Commercial.DAT` | ~3,809 | 8.7 MB |
| `00000002_Residential.DAT` | 2,720 | 5.4 MB |
| `0000000A_Landmarks.DAT` | 2,560 | 21.7 MB |
| `00000009_Landscape.DAT` | 1,160 | 1.4 MB |
| `EffectSprites.dat` | 1,074 | 9.0 MB |
| `00000010_Smoke.DAT` | 460 | 0.4 MB |
| `00000014_CityObjects.DAT` | 304 | 0.3 MB |
| four disaster archives | 953 | 22.3 MB |

Base categories alone are **~87,000 sprites**; with the Holiday expansion,
effects, disasters and the numbered add-on archives, comfortably **over
110,000**. The **median sprite is 52–120 bytes compressed**, the smallest 8, the
largest ~180 KB.

**[inferred]** 19,008 vehicle sprites against 2,720 residential building sprites
is the number that reframes the game. The buildings are what you remember; **the
moving things cost seven times as much art.**

### 3.3 The id encodes zoom and orientation, and the layout falls out

**[SHIPPED-DATA]** An instance id splits as `high 16 = object`, `low 16 = view`.
Grouping every archive by object gives an exact, uniform count per object, and
sorting by the low bits makes the structure obvious. One residential building:

```
obj 0070: 0:149  1:166  2:173  3:165 | 4:358  5:418  6:442  7:415
        | 8:1017 9:1181 A:1255 B:1215 | C:2868 D:3389 E:3542 F:3533
```

Four groups of four. **Within a group the compressed sizes are near-identical**
(the same building seen from four sides); **between groups they scale by ~3×**.
So the low bits are `(zoom << 2) | rotation`, and a building is **4 zoom tiers ×
4 rotations = 16 sprites.**

**[MAXIS]** The `.SII` index confirms it in real pixels, with Maxis's own
trailing dimension comments:

```
654FCB99, 25100000,   0,  66,  32,   9 ; 32 x 75
654FCB99, 25100001,   0,  65,  32,   9 ; 32 x 74
654FCB99, 25100002,   0,  66,  32,   9 ; 32 x 75
654FCB99, 25100003,   0,  65,  32,   9 ; 32 x 74
654FCB99, 25100004,   0, 133,  64,  17 ; 64 x 150
654FCB99, 25100008,   0, 267, 128,  33 ; 128 x 300
654FCB99, 2510000C,   0, 535, 256,  65 ; 256 x 600
```

**32 → 64 → 128 → 256. Exact doubling, four tiers, four rotations each.** Roads
show the same ladder at tile scale — 8 → 16 → 32 → 64 px across a tile.

### 3.4 Different asset classes ship different numbers of tiers, and that is the LOD system

**[SHIPPED-DATA]** Running the same grouping over every archive:

| Class | sprites per object | reading |
|---|---|---|
| Residential / Commercial / Industrial / Landmarks / CityObjects | **16** | 4 zoom tiers × 4 rotations |
| Roads / Landscape / Utilities | **20** | **5** zoom tiers × 4 rotations |
| Vehicles | **144** | **3** zoom tiers × 48 (8 headings × 6 frames) |
| Boats | 144 (2 objects at 720) | as vehicles |
| People | multiples of 16: 48, 64, 96, 112, 128, 144, 160, 176, 192, 240 | **2** zoom tiers × 8 headings × 3–15 frames |

The vehicle table is unambiguous once you print it — 144 sprites in three blocks
of 48 whose mean compressed sizes are **132 → 285 → 750 bytes**, and inside each
block a repeating period of 8 whose size profile is symmetric (large at the ends,
small in the middle), which is exactly what eight compass headings look like when
a car is wide seen along its length and narrow seen end-on.

**[inferred] This is the whole LOD system, and it is authored rather than
computed: the number of zoom tiers an asset ships is set by the zoom at which it
stops being worth drawing.** Ground and roads get five because the ground is
visible at every zoom. Buildings get four. Vehicles get three. **Pedestrians get
two** — below that a person is sub-pixel and simply is not drawn.

Compare the alternatives. [`factorio.md`](factorio.md) §4.1 has *no* LOD system
because a mip chain is one, generated free by hardware — but Factorio is
GPU-textured and has one camera orientation.
[`lod_systems.md`](../../topics/world/lod_systems.md)'s Total War numbers are 237
models × 4 LODs with a generation pipeline and switching distances to tune.
SimCity 3000 is between the two: **LOD by asset, decided per class, with the
cheap classes truncated early** — which costs nothing at runtime (you index a
different sprite) and costs a great deal offline (four bakes of everything).

### 3.5 Eight headings, not four rotations × eight headings

**[inferred]** The economy worth naming: a building needs 4 rotations because it
is a fixed object seen four ways. A car needs 8 **headings** because it drives in
eight directions on the grid — and those same eight sprites serve all four city
rotations, because rotating the city by 90° maps heading *h* to heading *h+2*.
A vehicle therefore costs 8 images per frame per zoom rather than 32.

That only works because the lighting rig in §2 is four-fold symmetric. **A sun
would have broken it**: a car facing north-east under a fixed sun is not the same
image as a car facing south-east in a rotated city. The lighting decision and the
sprite-count decision are the same decision, and it is worth ~4× on the largest
art category in the game.

---

## 4. Terrain and networks: the rules ship as text

### 4.1 68 plain-text autotiling rule files

**[SHIPPED-DATA]** `Apps/Res/TilingRules/` holds 68 `.txt` files covering six
network types — `ROAD`, `HWAY` (highway), `RAIL`, `SUBW` (subway), `POWR` (power
lines), `PIPE` (water pipes) — each with the same suite: `_Set`, `_SIMPLERULES`,
`_COMPLEXRULES`, `_Convert`, `_Complex_Convert`, `_Protected`, `_SlopeRULES`,
`_Bridges`, `_final`. Plus `ROAD_Exits`, `RAIL_Exits`, `SUBW_Exits`, `DIAG_Set`,
`Collapse.txt`, `LandfillRules1..6`, and `networkIntesection.txt` — Maxis's typo,
shipped.

**The file sizes say where the complexity is.** `HWAY_GRND_COMPLEXRULES.txt` is
**75 KB**; `ROAD_GRND_ComplexRules` 24 KB; `PIPE_GRND_COMPLEXRULES.txt` is
**5 bytes**. Pipes are buried, so they never need the hard case.

### 4.2 The rule format

An opcode stream, one comma-separated instruction per line:

```
0,16          ; rule count
1,<mask>      ; neighbour bitmask to match
2,<n>         ; n additional conditions follow
3,<dir>,<id>  ;   ...neighbour in direction dir must be piece id
4,1           ; result count
5,255,<id>    ; weight 255 -> resulting piece id
```

`ROAD_GRND_final.txt` is exactly the **sixteen four-bit N/E/S/W cases** — masks
0, 8, 4, 1, 2, 12, 9, 10, 5, 6, 3, 13, 14, 11, 7, 15 — the classic autotile.
`ROAD_GRND_SimpleRules.txt` uses **eight-bit masks** (72, 68, 136 …), diagonals
included, with `3,<dir>,<id>` conditions naming specific neighbour pieces.
`ROAD_GRND_Set.txt` is the membership set: the ~100 piece ids that count as
"road" when a neighbour is tested.

**[inferred] Resolution is layered — Simple, then Complex, then final — with
Convert, Protected, Slope and Bridge passes beside them, and `final` as the
total-function backstop that always answers.** That is the important structural
property: the simple pass handles the common case cheaply, the complex pass
handles junctions and named neighbours, and because `final` covers all 16 cases
unconditionally, **the system can never fail to pick a tile**. This is
[`factorio.md`](factorio.md) §5.5's rule — never emit a configuration the art
cannot represent — enforced by making the last rule set exhaustive rather than by
constraining the world.

Cross-network crossings are a lookup rather than rules. `networkIntesection.txt`
is a 3 × 3 matrix indexed by (network A, network B), zero on the diagonal:

```
{{"0", "00004500", "00004300"}, {"00004501", "0", "00004700"},
  {"00004301", "00004701", "0"}}
```

**[inferred]** Six authored pieces for three crossing network types. This is the
O(N²) problem [`factorio.md`](factorio.md) §6.3 solves for terrain transitions by
factoring shape from material; Maxis did not factor it, they enumerated it —
which is fine at N = 3 and is exactly why there are only three crossable
networks.

### 4.3 The terrain tools, from the game's own strings

**[SHIPPED-DATA]** `Res/Text/ENGLISH`: `Raise Terrain`, `Lower Terrain`,
`Level Terrain`, `Create Surface Water`, `Plant Trees`, `Terrain Edit`,
`Terrain Grid Toggle`, and for generation `New Terrain`, `Re-Generate Terrain`,
`Accept this Terrain`, `Load Real City Terrain` / `Real City Terrain Options`.

Same source gives the simulation's own layer vocabulary — the data maps are
**Crime, Density, Land Value, Pollution, Power, and "Traffic and
Transportation"**, with `Police Station Coverage`, `Fire Station Coverage`,
`Air Pollution`, `Garbage Pollution`, `Water`, `Education Quotient (eQ)`,
`Public Health / HQ`, `Life Expectancy`, `Population Density` beside them, and
the internal phrase **`Health, Education, Aura`**.

And the four simulation speeds, which are `Turtle`, `Llama`, `Cheetah` and
**`African Swallow`**.

---

## 5. The street sim, and the answer about people

### 5.1 What ships

**[SHIPPED-DATA]** `Apps/Res/SSimData/` — the data for `STRTSIM.DLL`:

| Table | live rows | record size |
|---|---|---|
| `VATTRIB.IXF` — vehicle attributes | **67** | 49 B fixed |
| `PATTRIB.IXF` — pedestrian attributes | **46** | 28 B fixed |
| `PPATHATT.IXF` — pedestrian path attributes | 192 | variable |
| `ANSPRATT.IXF` — animated sprite attributes | 112 | variable |
| `VCLPROD.IXF` — **vehicle production** | 31 | variable |
| `PPLPROD.IXF` — **people production** | 63 | variable |
| `TRNPROD.IXF` — train production | 2 | |
| `AIRTRMGR.IXF` — air traffic manager | 2 | |

Each also ships a `*_Holidays` variant. **[inferred]** The Holiday expansion
reskins the entire street sim by swapping whole tables rather than branching in
code — which is only possible because the tables are pure type data.

### 5.2 A vehicle record is a type, not an instance

**[SHIPPED-DATA]** All 67 `VATTRIB` records are exactly 49 bytes and decode as:

```
00  "BIN" 0x0D            tag + version 13
09  u32  ~118..125        varies per row
11  u32  ~35..52          varies per row
15  u32  0x222B7EE4       group of ANSPRATT
19  u32  0x822B7E6B       type  of ANSPRATT
1d  u32  0x2CCE, 0x2CCD…  instance in ANSPRATT  -> its animated sprite
21  u32  0x822B51AA       group of SIMSCRPT
25  u32  0xE22B51C4       type  of SIMSCRPT
29  u32  2001             instance in SIMSCRPT  -> its sim script
2d  u32  123999
```

**A vehicle is two numbers, a pointer to an animation and a pointer to a script.**
There is no position, no heading, no route, no origin, no destination and no
occupant. This is a table of 67 *kinds of vehicle*, not a roster of vehicles.

`SIMSCRPT` records are plain ASCII, tab and CRLF delimited — a clause count, then
per clause an `opcode<TAB>argcount` line followed by that many numeric lines:

```
1
0	5
10
0
1
12500
5000
```

### 5.3 So: how is each Sim represented?

**[inferred] It is not.** The evidence is entirely negative and entirely
consistent:

- The only pedestrian data in the game is **46 fixed 28-byte type records** and a
  `PPLPROD` **production** table. Production, not population — a rate at which
  pedestrian *sprites* are emitted.
- The people sprite archive holds **two zoom tiers only**. A representation of a
  citizen that vanishes when you zoom out one step is a representation of a
  citizen's *picture*.
- The simulation's own output vocabulary is entirely aggregate: `Population
  Density`, `Education Quotient`, `Life Expectancy`, `Land Value` — quotients and
  densities over tiles, with no place for an individual to be stored.
- The shipped strings include a **`Traffic Visible`** setting.

**[inferred]** That last one is the sharpest. A toggle called "traffic visible"
is only meaningful if drawing the traffic and simulating it are separable — and
if turning the drawing off changes nothing, the drawn cars were never the
simulation. The traffic model is a **flow over the road network**; the cars are a
*visualisation* of that flow, produced by a production table at a rate the flow
sets, and destroyed when they leave view.

This is [`crowd_scale.md`](../../topics/scale/crowd_scale.md)'s thesis at its
limit. That note concludes about Assassin's Creed Unity, World War Z and Left 4
Dead that "none of these three made characters cheaper — all three made **most**
characters *not be characters*". **SimCity 3000 made all of them not be
characters.** There is no expensive tier at all, and therefore none of the
transition machinery that note identifies as the actual engineering. The city
supports an unbounded population because the population is a number.

### 5.4 What that bought, and what the sequels paid to undo it

**[SC4/COMMUNITY]** SimCity 4 (2003) replaced this with genuine agents. Cities:
Skylines (2015) put a hard ceiling on it — **[COMMUNITY]** ~65,000 agents — and
Colossal Order's own design deep dive is candid that the ceiling is structural:

> "There's a limit of how many citizens and vehicles can be simulated on the
> streets at the same time."

They then had to build a compensating mechanism, which is the tell:

> "we created the worker system so that workers not actually physically going to
> their workplaces don't give the player a penalty"

**[inferred] That sentence is the entire cost of the decision SimCity 3000
declined to make.** Once a Sim is an agent, the ones you cannot afford to
simulate become *absent*, and absence has gameplay consequences that must then be
explicitly cancelled out. SimCity 3000 has no such problem because nobody was
ever present.

Two further details from the same source are worth keeping, because both are
rules this project already holds:

> "When vehicles plan their route, they stick to it. They will not recalculate in
> the middle of the journey unless something on their path has been modified."

— CLAUDE.md's incumbency rule, in a shipped traffic simulator; and

> "the car movement is simulated about 4 times per second while the rendering
> will use two simulation frames to derive a smooth position."

— a 4 Hz simulation with the renderer interpolating between two sim frames, which
is the same split SimCity 3000 gets for free by never simulating the cars at all.

---

## 6. Where the lineage went: SimCity 4 turned the sprites into geometry

The successor is worth a section because it shows which of SimCity 3000's
constraints were essential and which were 1999.

**[SC4]** SimCity 4's Building Architect Tool is documented by the community
encyclopaedia, and the export step is the direct descendant of §3.3:

> "**Export** - Produces a render of the model for all four viewport rotations
> (North, East, South, West), 5 zoom levels (zoom 6 is not rendered), and in both
> day and night mode (**40 renders in all**)."

Four rotations survived. Zoom tiers went from four to five. Night became a second
full bake rather than a trick. And the pipeline gained a distortion correction
that only exists because the projection is frozen:

> "It is well documented that the game's camera angle causes buildings to look
> shorter than they actually are… As a result, it is advisable to consider
> stretching models along the Z-axis to correct this illusion."

**[SC4]** Reading `memo33/BAT4Blender`, a reimplementation of Maxis's `BAT4Max`
renderer, gives the constants:

```python
cam_ob.data.type = "ORTHO"
angle_zoom     = [radians(60), radians(55), radians(50), radians(45)]  # zoom 4,5,6 share 45°
angle_rotation = [radians(-67.5), radians(22.5), radians(112.5), radians(202.5)]
zoom_sizes     = [8, 16, 32, 73, 146]  # from SFCameraRigHD.ms (horizontal extent of a 16×16 cell in px)
```

Three findings, all of which reflect back on SimCity 3000:

1. **SimCity 3000's tile ladder is 8, 16, 32, 64 — exact doubling. SimCity 4's is
   8, 16, 32, 73, 146**, and the break is because **the camera pitch changes with
   zoom** (60°, 55°, 50°, 45°). SC3K's four tiers are the same projection at four
   scales; SC4's five are five *different* projections. **[inferred]** That is
   why SC4 could not have used a mip chain even if it had wanted to, and it is
   the reason a "zoom level" in this family is an asset and not a filter.
2. **The sun rotates with the view.** `Sun.py` derives its orientation from
   BAT4Max's "South sun location" `(-474, -352, 575)` and then adds 90° per
   rotation. SC4 kept a directional key light and made it a property of the
   *camera* rather than the world. **[inferred]** That is the other legal
   solution to §2.1's constraint — Quigley removed the sun, Maxis's SC4 team
   welded it to the viewer — and it is why SC4 buildings have shadows and SC3K's
   do not.
3. **The sprite became a texture on a box.** `LOD.py` builds an eight-vertex,
   six-face cube fitted to the model's bounding box, one per zoom tier, slices it
   to match the 256 × 256 image tiles, and exports the set as an `.obj` shipped
   alongside the renders. **[inferred] SimCity 4's world is 3D geometry wearing a
   photograph** — which is precisely the impostor system
   [`ruse.md`](ruse.md) §4.4 describes, and it is what buys SC4 correct depth
   sorting, occlusion and mouse-picking against terrain that SimCity 3000 must
   solve with a painter's algorithm and a registration point.

**[inferred]** The through-line: SimCity 3000 and SimCity 4 make the *same* art,
by the same method, at nearly the same cost. What SC4 buys with hardware is not
better images — it is **the freedom to place them in a real depth buffer**, and
everything SC4 does that SC3K cannot (shadows, a fifth and steeper zoom,
occlusion) follows from that one change rather than from the renderer being
prettier.

---

## 7. What transfers

Ranked by how directly it applies to this project.

1. **LOD by asset class, truncated by visibility.** The single most portable idea
   here. Do not give every class the same number of levels — give each class
   exactly as many as it is still legible at, and let the cheap classes fall off
   early. SimCity 3000: terrain 5, buildings 4, vehicles 3, pedestrians 2. This
   costs nothing at runtime and is a content decision, not an engine one.
2. **Symmetric lighting is what makes a bake survive rotation.** If anything in
   this project is ever pre-rendered or pre-shaded for a camera with discrete
   orientations, the rig has to be symmetric under those orientations or the
   orientations will disagree. Quigley's six-light cross is the minimum
   construction that separates four faces without committing to a direction.
   Corollary: **the orientation count and the lighting model are one decision**,
   and getting the symmetry right is worth a 4× multiplier on the art (§3.5).
3. **An exhaustive final rule set beats a constrained world.** The layered
   Simple → Complex → `final` structure, where `final` covers all 16 neighbour
   cases unconditionally, means tile selection is a total function. Prefer this to
   validating that bad configurations cannot arise.
4. **Return the decomposition, not the verdict — and its converse.** Maxis
   enumerated the 3 × 3 network-crossing matrix rather than factoring it. That is
   the right call at N = 3 and the wrong call at N = 8; the factoring cost is
   worth paying only once N² stops being small. Know which side of that you are
   on before you build a transition system.
5. **A "visible" toggle is a design smell worth reading.** `Traffic Visible`
   exists because the drawn traffic and the simulated traffic were never the same
   object. If a system in this project acquires such a toggle, that is evidence
   the two halves have separated — which may be exactly right, but should be
   deliberate.
6. **Put the authoring rules in text and ship them.** 68 readable rule files and
   self-documenting sprite indexes are why this note could be written at all, and
   they were presumably why the game could be tuned at all. Compare
   [`ruse.md`](ruse.md) §2.3, where the same property makes Eugen's build legible
   twelve years on.
7. **Do not take: the total absence of agents.** SimCity 3000's model is right for
   a city of a million and wrong for a squad of eight. It is included here as the
   far end of the axis, not as a recommendation — the value is in seeing that the
   axis has an end, and what living there costs (§5.4).

---

## 8. What was not established

Stated plainly, because the negative list here is long.

- **The sprite codec.** The per-sprite header is ~16 bytes with a payload-length
  field and a constant `8` at offset 6 (plausibly bits per pixel, i.e. palettised
  — unconfirmed). The compression scheme was not decoded, and no claim in this
  note depends on it. The two fields at offsets 6 and 8 scale with zoom tier and
  were used only as a cross-check on §3.3, which the `.SII` dimensions establish
  independently.
- **Which end the fifth tier is on.** Ground, roads and utilities ship five zoom
  tiers where buildings ship four. Compressed sizes are consistent with the extra
  tier being the most zoomed-*in*, but not decisively, and the alternative
  (an extra zoomed-*out* tier where individual buildings stop being drawn) is not
  excluded.
- **The traffic algorithm itself.** §5 establishes what traffic is *not* — it is
  not agents — and that `STRTSIM` is a distinct component with production tables.
  It does not establish the routing model: whether commutes are a flood fill with
  a depth limit, a random walk in the SimCity 1 tradition, or something else; how
  often it runs; or how congestion feeds back. `STRTSIM.DLL` is stripped and
  there is no published account.
- **The simulation tick structure.** No evidence was found for how often each
  layer updates, whether the map is swept whole or amortised in fractions, or
  what the four speed settings actually change.
- **Map dimensions, height resolution, and the `.sc3` save layout.** Not opened.
  The saves are present (`Cities/*.sc3`, 150–850 KB) and are the obvious next
  target, because the per-tile arrays in a save file *are* the simulation's state
  layers with their bit widths — the single highest-value unread thing in the
  install.
- **`Res/Occupant/OccupantAttribs.IXF`** (472 KB, ~17,000 rows) — almost certainly
  the per-building-type table, and the corrected parser did not handle its
  directory layout.
- **Terrain generation and deformation.** The tool names are known from the
  strings (§4.3); the generator's parameters, the algorithm, the number of
  altitude levels, and what a height edit invalidates are all unestablished.
- **The `.bld` format** (17 shipped Building Architect files, 24–215 KB) and
  `BARender/BAT.DAT` (2.4 MB).
- **The published record, entirely.** This note is written from the install and
  makes no use of press, interviews or retrospectives. So: the cancelled 3D
  SimCity 3000 shown at E3 1997, the late-1997 restart, what carried over from
  SimCity 2000, team size and the programmer:artist ratio, the published system
  requirements, and contemporary accounts of late-game slowdown are all
  **unexamined here**. The artist:programmer ratio in particular would be worth
  having, because §1.3's 33:1 art-to-code *byte* ratio is an argument about where
  this game's fidelity came from and headcount would corroborate or kill it.
  `Res/Text/ENGLISH/SC3StringsCredits.IXF` was opened and yields only the **role
  headings** — `Director of Development`, `Lead Engineer`, `Senior Engineer`,
  `Engineering`, `Art Director`, `Art`, `Audio Programming Lead Engineer`,
  `Designer`, `Scenario Creators`, `Guest Architects`, `SimCity Exchange
  Development` — with the credited names held in a separate table that a
  flat string scan does not recover. Suggestive (engineering is three headings,
  one of them singular) but **not a count**, so no claim is made from it.
- **Whether SimCity 3000 has cast shadows at all.** §2 argues from the rig that it
  cannot have directional ones. It was not visually verified.

---

## 9. Reproducing this

Everything above came from the retail GOG install plus two small scripts, kept in
the session scratchpad:

- `ixf3.py` — the archive reader. Magic `D7 81 C3 80`, then a dense run of 20-byte
  `(type, instance, group, offset, size)` u32 LE records from byte 4. The
  directory length is **derived**, not assumed: walk forward and stop at the first
  record whose offset does not clear the directory or whose blob leaves the file.
  Zeroed slots are holes, not terminators.
- Grouping by `instance >> 16` and sorting by `instance & 0xFFFF` is what makes
  the zoom/rotation layout visible; comparing mean compressed size across
  candidate group counts is what identifies the tier boundaries.
- `dumpbin -exports` from VS2022 for the GZCOM finding.
- The `.SII`, `TilingRules/*.txt` and `BATConfig.ini` files are plain text and
  need nothing.

The obvious next pass is the `.sc3` saves, and it is worth doing — a save file is
the simulation's state layers written down, which is the one thing the stripped
binaries will never tell you.
