# World in Conflict — the nuke, taken apart

The tactical nuclear strike is the thing everybody remembers about **World in
Conflict** (Massive Entertainment / Sierra, 2007), and it is the most expensive
single effect in the game. This note is what it is actually made of: which files,
in what order, on which nulls, with which textures and which sounds — read out of
the retail install rather than guessed from footage.

> **Method and tags.** Everything marked **[BUILD]** was read from the retail
> install on this machine (`E:\World in Conflict`, v1.0.1.0, 26 `.sdf` archives)
> using the readers in [`../../../../tools/wic/`](../../../../tools/wic/), whose
> output lands in `wic_extracted/`. **[EXE]** is recovered from `wic.exe`'s string
> region. **[inferred]** is our reading. Identifiers and paths are Massive's own,
> transcribed; case is theirs (the `.mrb` interior is upper-cased by the exporter,
> the `.pe` files are mixed).
>
> Companions: [`world_in_conflict_particles.md`](world_in_conflict_particles.md)
> is the particle *system* — six-point lighting, cluster lighting, the DX9
> z-feather, the `.pe` format. This note assumes it. [`world_in_conflict.md`](world_in_conflict.md)
> is the renderer around both.

---

## 1. The chain, end to end

**[BUILD]** Nothing about the nuke is special-cased in a script. It travels the
same four hops every impact in the game travels, and the only thing that makes it
a nuke is how big the numbers are:

```
maps/supportweapons.ice        the tactical aid    "Support_MiniNuke"
        │                      icons, cost 300000, delivery model, flight path
        │                      buildup sound  sound/sfx/nuke_buildup_01.wav
        ▼
juice/hiteffects.ice           the hit set, one row per surface material
        │                      model   effects/mushroomsmoke/smoke_2.mrb
        │                      orient  WORLD_Y_UP_RANDOM_ROT
        │                      decal   Nuke_Crater
        │                      sound   explosive_atomic_01
        ▼
effects/mushroomsmoke/smoke_2.mrb      the effect scene — 63 emitter slots
        │                              on ~30 nulls, plus 4 meshes and 3 halos
        ▼
pelib/supportweapons/nuke/*.pe         16 particle effects, plain text
```

Two things are worth stopping on before the detail.

**The mushroom cloud is a `.mrb`, not a `.pe`** — and so is every other impact in
the game. `.mrb` is the *render mesh* format, the same one units ship in, and
Massive use it as the effect-scene format: a tree of null objects with animated
transforms, each null carrying a particle effect. **586 `.mrb` files in the
archives carry an emitter table, 5,581 slots between them**, and the smallest is
a rifle bullet hitting dirt with two. There is no separate effect-graph format
because the mesh format already had hierarchy, animation and an exporter.

That reframes the nuke. It is not an effect that outgrew the `.pe` format and got
a bespoke rig; **it is an ordinary hit effect with the dials turned up**, built by
the same artists in the same tool as a mortar round. §8 puts numbers on how far
up. The 67 `COMPOUND` `.pe` files (a flat list of `PE` lines) are the *small*
version of the same idea, used where a scene would be overkill — the nuke uses one
of them, eight times, for its ground smoke.

**The hit set is per-material, and only the numbers change.** `Support_MiniNuke`
has a row for `GROUND_DEFAULT`, `SNOW`, `GROUND_ROCK`, `WATER`, `METAL`, `HOUSE`
and the rest, and every one of them names the same `smoke_2.mrb` and the same
`explosive_atomic_01`. What varies is the decal: the `METAL` row asks for
`ScorchMark_Mortar_Ground` instead of `Nuke_Crater`, because you cannot crater a
tank. **[inferred]** That is the whole material system doing its job — the
per-surface hook exists and the nuke uses exactly one field of it.

---

## 2. The effect scene

**[BUILD]** `effects/mushroomsmoke/smoke_2.mrb` is 248,358 bytes and contains no
appreciable geometry of its own. Its nodes:

| Node(s) | Count | What hangs off it |
|---|---:|---|
| `Null_Smoke` | 1 | the stem's cluster-lighting origin |
| `Null_Zplus` … `Null_Zplus7` | 8 | the rising column — four emitters each |
| `Null_Ground` | 1 | ground-ring cluster-lighting origin |
| `Null_GroundToCenter` … `7` | 8 | the ground shockwave collapsing inward |
| `Rotatesmoke1` … `Rotatesmoke8` | 8 | the cap's rolling torus |
| `Null_Clouds` | 1 | the high, slow overcast |
| `Null_Gety_1` … `Null_Gety_8` | 8 | wreck/debris attachment points (`.gety` is the LightWave wreck format) |
| `Null_Shadow` | 1 | — |
| `NukeBlastWave_1_` | 1 | mesh, additive |
| `NukeGroundFlare_1_` | 1 | mesh, additive |
| `NukeBlastWave_2_1_` | 1 | mesh, additive |
| `NukeBlastWave_2_2_` | 1 | mesh, **heat-haze distortion** |
| unnamed halo nodes | 3 | `surfaces/halo.sur` lens flares, animated |

**Eight is the magic number.** Eight `Zplus` nulls, eight `GroundToCenter` nulls,
eight `Rotatesmoke` nulls, eight `Gety` nulls. **[inferred]** These are radial
arrays — eight copies of one emitter at eight bearings — which is why the fireball
reads as a volume rotating about an axis rather than as a billboard facing you.
It is also why the effect is not a compound `.pe`: the *transform animation on
each of those 24 nulls* is the effect, and only a scene format can carry it.

### 2.1 The emitter table

**[BUILD]** The `.pe` references sit in one table near the end of the file, each
record `<path>\0<4-char state tag><u32>`. Sixty-three slots:

| `.pe` | Slots | State |
|---|---:|---|
| `nuke/NukeSmoke_Rotate` | 8 | `STND` |
| `nuke/NukeSmoke_Rotate_Hanger` | 8 | `STND` |
| `nuke/NukeSmoke_Rotate_Flare_Inner` | 8 | `STND` |
| `nuke/NukeSmoke_Rotate_Billow` | 8 | `STND` |
| `nuke/NukeSmoke_Rotate_Flare` | 8 | `STND` |
| `SupportWeapons/Nuke_GroundSmoke` (compound) | 8 | `STND` |
| `nuke/NukeSmoke_Ground_Still` | 1 | `STND` |
| `nuke/NukeSmoke_Billow_Ground_Slow_Back` | 1 | `STND` |
| `nuke/NukeSmoke_Billow_Ground` | 1 | `STND` |
| `nuke/NukeSmoke_Additive` | 1 | `STND` |
| `nuke/NukeSmoke_Flare` | 1 | `STND` |
| `nuke/NukeSmoke_Billow_Pillar_Hanger` | 1 | `STND` |
| `nuke/Clouds` | 1 | `STND` |
| `nuke/Rotate_Billow` | 8 | **`WALK`** |

The eight `Nuke_GroundSmoke` slots are the corpus's compound form, and each of
them expands to three more effects — `NukeSmoke_Billow_Ground_Ring`,
`..._Ring_Ytter` and `GroundFire` — so the true emitter count is **87**, from 16
distinct authored `.pe` files.

`STND` and `WALK` are **MRB animation-state names**, and the census across all
`.mrb` files settles what that field is. The full vocabulary attached to emitter
slots, game-wide:

```
STND 3210   WALK 1142   LIVE 119   FIRE  33   STFR  32   CRFR  32   CRCH  25
PROP   20   PRFR   19   WLFR  19   RELX  16   READ   6   CRRD   3   OPEN   7
```

— stand, walk, crouch, stand-fire, crouch-fire, prone-fire, walk-fire, relax,
reload, crouch-reload. **That is an infantry animation state machine**, and it is
the same field. **[EXE]** The exe carries `ParseSetMrbState()` and a
`SetMrbState` parser error, so scripts can drive it. (Cutscene FX scenes use a
separate `BODS`/`BODE`/`BOCD`/`BOCE`/`BOPD`/`BOPE`/`BOGI` set — 1,700 slots
between them — which is not decoded here.)

**[inferred]** So an emitter is gated on the animation state of the object it
belongs to, which is exactly what a muzzle flash or an exhaust plume needs, and
the nuke inherits it for free. Nothing in the shipped scripts sets the nuke's MRB
state, so its 8 `WALK` emitters are a variant left wired and unreached — the sort
of thing that survives in a shipped file because it costs nothing.

### 2.2 Cluster lighting anchored to a node inside the effect

**[BUILD]** `CLUSTERLIGHTINGBONE` on the nuke's effects names nodes of this scene:

```
NukeSmoke_Billow_Ground.pe          CLUSTERLIGHTINGBONE Null_Smoke
NukeSmoke_Rotate_Billow.pe          CLUSTERLIGHTINGBONE Null_Smoke
NukeSmoke_Billow_Ground_Ring.pe     CLUSTERLIGHTINGBONE Null_Ground
NukeSmoke_Billow_Ground_Ring_Ytter  CLUSTERLIGHTINGBONE Null_Ground
NukeSmoke_Ground_Still.pe           CLUSTERLIGHTINGBONE Null_Clouds
```

That is [`world_in_conflict_particles.md`](world_in_conflict_particles.md) §2.3 at
full stretch. The cluster term brightens a particle on the sunward side of its
plume and darkens it on the far side, using `dot(normalize(particlePos −
plumeOrigin), sunVector)`; here the *plume origin is three different animated
nodes*, so the stem self-shadows about the stem, the ground ring self-shadows
about the ground, and the high cloud self-shadows about the cap — three
independent volumes lit as three spheres, for three dot products, with no sorting
and no depth pass between them. **[inferred]** This is the single reason the nuke
does not read as a heap of sprites at the scale it is drawn: at 300-unit sprite
sizes an unlit or uniformly-lit billboard is enormous and obviously flat.

### 2.3 Half the game's shadow-casting particles are in the nuke

**[BUILD]** `CASTSHADOWS` appears on **14** of 2,104 effects in the whole game.
**Seven of them are the nuke:**

```
pelib/supportweapons/nuke/nukesmoke_billow_ground.pe
pelib/supportweapons/nuke/nukesmoke_billow_ground_ring.pe
pelib/supportweapons/nuke/nukesmoke_billow_ground_ring_ytter.pe
pelib/supportweapons/nuke/nukesmoke_billow_ground_slow_back.pe
pelib/supportweapons/nuke/nukesmoke_billow_pillar_hanger.pe
pelib/supportweapons/nuke/nukesmoke_rotate_billow.pe
pelib/supportweapons/nuke/nukesmoke_rotate_hanger.pe
```

The other seven are the daisy cutter, the fuel bomb, carpet bombing and one smoke
marker — i.e. **every shadow-casting particle in World in Conflict is a tactical
aid.** The particles note called out `CASTSHADOWS` as "a capability shipped and
then used 0.7% of the time"; this is where it went. The whole feature exists for
the moment the mushroom cloud lays its shadow across the town.

---

## 3. The sixteen particle effects, by job

**[BUILD]** All sixteen live in `pelib/supportweapons/nuke/` (plus the compound
one directory up). Grouped by what they are for:

### The fireball — first two seconds, additive

| `.pe` | Surface | Texture | Notes |
|---|---|---|---|
| `NukeSmoke_Rotate` | `particle_additive` | `Explosions\ExploWithFireEnd_128x128_64F.dds` | `EMITTORSPACE`, emit circle r=5, `VELZ 50–70`, `LOOPTEXTUREANIM 0` — plays the flipbook **once** and dies |
| `NukeSmoke_Additive` | `particle_additive` | `Explosions\Explosions_128x128_128F.dds` | the central ball, size 25–40, rotation randomised over 360° |
| `NukeSmoke_Flare` | `particle_additive` | `Flares\SmallBulletHit.dds` | **size 100–200**, tinted `255 208 176` → `176 198 255`: warm core cooling to blue |
| `NukeSmoke_Rotate_Flare` | `particle_additive` | `Flares\NukeFlare.dds` | size 150–350, tint `72 205 255` |
| `NukeSmoke_Rotate_Flare_Inner` | `particle_additive` | `Flares\NukeFlare.dds` | size 200–300, tint `204 224 255`, `RADIUS 3000` |
| `GroundFire` | `particle_additive` | `Explosions\Explosions_128x128_128F.dds` | `EMITORIGIN RECTANGLE 70 0` — fire across the whole footprint |

`LOOPTEXTUREANIM 0` on `NukeSmoke_Rotate` is the one flag in the set that is
about *correctness rather than look*: a 64-frame explosion flipbook that loops
would restart the fireball, so this effect alone opts out of the corpus-wide
default.

### The column and the cap — six-point lit

| `.pe` | Texture pair | Notes |
|---|---|---|
| `NukeSmoke_Rotate_Billow` | `BillowingSmokeThick_1/2_128x128_64F` | `EMITTORSPACE`, circle r=20, four phases that push out, up, back in and up again — the rolling torus |
| `NukeSmoke_Rotate_Hanger` | `rotateSmokeFill_6p_0pos/1neg_128x128_128` | `FORCEONEPARTICLE 1`, size **130–160**, `VELY −30` — one enormous sprite per null hanging under the cap |
| `NukeSmoke_Billow_Pillar_Hanger` | `BillowingSmokeThick_1/2` | size 30–70, 40 s, `SORT` — the stem |
| `NukeSmoke_Billow_Ground` | `BillowingSmokeThick_1/2` | the base of the stem, `VELY 18` |
| `NukeSmoke_Billow_Ground_Slow_Back` | `rotateSmokeFill_6p_0pos/1neg` | size **60–80**, four phases over 57 s, `RANDOMFLIPV` at p=0.5 |
| `NukeSmoke_Ground_Still` | `BillowingSmokeThick_1/2` | `FORCEONEPARTICLE 1`, 60 s — the residual pall |
| `Clouds` | `rotateSmokeFill_6p_0pos/1neg` | `RADIUS 1000 REMOVEDIST 5000`, `ANIMTIMEMIN 30` (one frame every 30 s), size 18–22 → ×1.5 — the top-of-atmosphere spread |

`Clouds` is the interesting one. `RADIUS 1000` and `REMOVEDIST 5000` are the
largest in the corpus by a wide margin (the medians are 100 and 600), and
`ANIMTIMEMIN 30` walks the 128-frame flipbook so slowly it is effectively a still
that drifts. **[inferred]** This is the layer that has to still be there when the
camera pulls back after the blast, and it is authored to cost nothing while it
waits.

### The ground shockwave

| `.pe` | Surface | Notes |
|---|---|---|
| `NukeSmoke_Billow_Ground_Ring` | `particle_sixpointlight` | `EMITORIGIN RECTANGLE 70 0`, `SPACE EMITTOR`, `CLUSTERLIGHTING` + `CASTSHADOWS`, 45 s hold |
| `NukeSmoke_Billow_Ground_Ring_Ytter` | `particle_sixpointlight` | the outer ring (*ytter* = outer, Swedish), size 5–20, tighter spawn |
| `Rotate_Billow` | **`particle_normalmap_transparent`** | `NukeSpecial\SmokeRotate_Nuke_64x64_64f.dds`, size 180–210, final phase tinted `255 189 140` |

`Rotate_Billow` is the only effect in the nuke using
`particle_normalmap_transparent`, and the only one drawing the bespoke
`NukeSpecial\` texture — **[BUILD]** the one flipbook in the game authored for
this effect and nothing else. It is also the whole of the `WALK` state set.

---

## 4. The four meshes, and the heat haze

**[BUILD]** Four mesh nodes in the scene, each an animated LightWave object with
a surface and two textures:

| Node | Geometry | Surface | Textures |
|---|---|---|---|
| `NukeBlastWave_1_` | `EFFECTS/ATOMBOMB/NUKEBLASTWAVE.LWO:0` | `standard_additiveblend_2tex.sur` | `special/blastring_2`, `waves/blastwave_alpha` |
| `NukeGroundFlare_1_` | `EFFECTS/ATOMBOMB/NUKEGROUNDFLARE.LWO:0` | `standard_additiveblend_2tex.sur` | `flares/flare_01`, `waves/blastwave_alpha` |
| `NukeBlastWave_2_1_` | `EFFECTS/ATOMBOMB/NUKEBLASTWAVE_2.LWO:0` | `standard_additiveblend_2tex.sur` | `fireanimations/fireani_128x128_64f`, `waves/blastwave_alpha` |
| `NukeBlastWave_2_2_` | `EFFECTS/ATOMBOMB/NUKEBLASTWAVE_2.LWO:1` | **`game/postfx_heathaze.sur`**, tagged `DISTORT` | `waves/blastwave_distortalpha`, `_dummy_objects/heathaze_testobject/dudvtest` |

Two readings worth recording.

**The expanding shockwave is geometry, not particles.** A ring that must stay a
*ring* while it expands is exactly the case billboards are bad at — a ring made
of sprites shows its sprites at the seams and its thickness varies with the camera
— so Massive modelled it, animated the scale, and scrolled an alpha over it.
`blastwave_alpha` is shared by three of the four meshes: one authored gradient,
three different looks, produced by the *other* texture in the pair.

**The heat haze is the same mesh, second submesh, different surface.**
`NUKEBLASTWAVE_2.LWO:0` is drawn additive with fire on it and `:1` is drawn into
the distortion pass with a DUDV map. One export, one animation, two passes — the
distortion is guaranteed to be exactly where the fire is, for free, because it is
the same object. **[inferred]** That is the cheapest possible way to keep two
passes in sync, and it is unavailable to anyone whose effect system cannot put two
materials on one animated object.

**Three halo nodes** (`surfaces/halo.sur`) carry the lens flare, with animated
tracks: one on `flares/black` and two on `flares/explosionflare_2`. **[inferred]**
The black one is the occluder — `halo.sur` needs something to test against — and
the two bright ones are the flare itself at two scales.

---

## 5. Every texture the nuke touches

**[BUILD]** Sizes are the archived payload. Every reading below is confirmed
twice — the byte arithmetic is exact, *and* the atlas decodes to coherent frames.
Both checks are needed; §5.1 is what happens when only the first is done.

### Particle flipbooks — all of them single grid atlases

| Texture | Payload | Reading | Grid |
|---|---:|---|---|
| `SmokeAnimations\New_Nmap\rotateSmokeFill_6p_0pos_128x128_128` | 10,485,760 | 2048×1024 A8R8G8B8 + 1 mip | 16×8 of 128² |
| `SmokeAnimations\New_Nmap\rotateSmokeFill_6p_1neg_128x128_128` | 7,864,320 | 2048×1024 R8G8B8 + 1 mip | 16×8 |
| `SmokeAnimations\New_Nmap\BillowingSmokeThick_1_128x128_64F` | 5,242,880 | 1024×1024 A8R8G8B8 + 1 mip | 8×8 |
| `SmokeAnimations\New_Nmap\BillowingSmokeThick_2_128x128_64F` | 3,932,160 | 1024×1024 R8G8B8 + 1 mip | 8×8 |
| `Explosions\Explosions_128x128_128F` | 1,048,576 | 2048×1024 DXT1, no mip | 16×8 |
| `Explosions\ExploWithFireEnd_128x128_64F` | 1,048,576 | 1024×1024 DXT5, no mip | 8×8 |
| `NukeSpecial\SmokeRotate_Nuke_64x64_64f` | 348,160 | 512×512 DXT5 + 3 mips | 8×8 of 64² |
| `FireAnimations\FireAni_128x128_64f` | 699,040 | 1024×1024 DXT1 + mips to 8×8 | 8×8 |

**The names lie about the layout and the file names are the only place the frame
size is written down.** `_128x128_128` means *128 frames of 128×128*, and the
file is one 2048×1024 image. The `.pe` says the same thing in
`PARTICLEWIDTH` / `PARTICLEHEIGHT` / `NUMTEXTURES`, and says nothing about how
they are packed.

The mip situation is the same everywhere: **one extra level for the whole sheet,
or none at all** — never a chain. That is the right call for a sheet, because
level 2 would start bleeding neighbouring frames into each other; `FireAni` is
the exception that proves it, carrying mips down to 8×8 because it is sampled by
a *mesh* with authored UVs rather than by the particle system's frame indexer,
and the bleed does not matter to it.

The two `_6p_` pairs and the two `BillowingSmokeThick` pairs are **shared, not
authored for the nuke** — they are the same texture pairs used by 224 and 59 other
effects respectively. **The nuke's total unique texture cost is one 340 KB
flipbook and a handful of small flares.** Everything that makes it look enormous
is parameters over art that was already resident.

### Flares, rings and haze

| Texture | Payload | Reading | Used by |
|---|---:|---|---|
| `Flares\NukeFlare` | 32,768 | 256² DXT1, no mip | `Rotate_Flare`, `Rotate_Flare_Inner` |
| `Flares\SmallBulletHit` | 10,936 | 128² DXT1 + full mip chain | `NukeSmoke_Flare` |
| `Flares\Flare_01` | 43,704 | 256² DXT1 + full mip chain | `NukeGroundFlare` mesh |
| `Flares\ExplosionFlare_2` | 43,008 | 256² DXT1 + 2 mips | two halo nodes |
| `Flares\Black` | 184 | 8×32 DXT1 + 3 mips | halo occluder |
| `Special\BlastRing_2` | 10,912 | 128² DXT1 + 4 mips | `NukeBlastWave_1_` |
| `Waves\BlastWave_Alpha` | 43,728 | 128×256 DXT5 + full mip chain | three of the four meshes |
| `Waves\BlastWave_DistortAlpha` | 10,960 | 64×128 DXT5 + full mip chain | heat-haze submesh |
| `_dummy_objects\heathaze_testobject\DudvTest.tga` | 49,196 | uncompressed DUDV | heat-haze submesh |
| `Effect_Textures\Special\Flat_Normalmap.tga`, `surfaces\dummy_normalmap_flat.tga`, `surfaces/dummy.dds` | — | placeholders in `TEXTURE2`/`TEXTURE3` for effects that use neither slot | most of the additive set |

Also, outside the effect itself: `ground/explosionbrushes/explosion_atlas` holds
`Nuke_Crater` at **(0, 0) 1024×1024** in a 2048² atlas — every other brush in that
atlas is 512², so the nuke's crater is the only one given a quarter of the sheet.
And `ui/icons/support_weapons/nuke{,_active,_inactive,_disabled}.dds`,
`ui/minimap/minimap_ta_nuke.dds`, `ui/markers/supportweapons/nuke/nuke_minimap.dds`
and `ui/icons/tacticalaid_playfieldicons/tacticalnuke.tga` are the UI set.

### 5.1 The layout the arithmetic could not see, and the bug it hid

This section exists because the table above was wrong on the first pass, in a way
that is worth recording rather than quietly fixing.

**A stack of 128 frames each carrying its own small mip chain, and one
2048×1024 image carrying one mip, are the same number of bytes.**

```
128 × (128·128·4 + 64·64·4)   =  128 × 81,920      = 10,485,760
      2048·1024·4 + 1024·512·4 =  8,388,608 + 2,097,152 = 10,485,760
```

Exactly equal, and equal again for the `_1neg` triple, the two
`BillowingSmokeThick` files and both explosion books. **So the size model cannot
tell them apart, and neither can the mip-agreement test, because a frame stack of
smoke frames also decodes smoothly.** The first reading here — and the assumption
built into `wic_tex.py`'s `identify_atlas` — was the frame stack. It is wrong.

The pixels say so immediately. Read as a stack, every frame comes out as a comb
of interleaved rows, because a 128-pixel-wide view of a 2048-pixel-wide image
walks a sixteenth of a row at a time and then jumps. Read as a grid, the frames
are clean smoke. **The contact sheets in §5.2 are the whole argument.**

Three fixes went into [`wic_tex.py`](../../../../tools/wic/wic_tex.py):

1. `identify_atlas` now solves the **grid** — divisors of the frame count that
   keep both atlas axes a power of two, squarest first — and returns the atlas
   dimensions, so the decoder needs no special case. Where two layouts tie on
   bytes (128 frames of 128² is 16×8 *or* 8×16), it decodes both and takes the
   smoother, which separates them cleanly.
2. `pe_hints` no longer reads `TEXTURE3`. That slot is `surfaces/dummy.dds` in
   nearly every effect in the game, so including it declared the shared dummy to
   be a 128-frame flipbook.
3. **A hint that two effects disagree about is dropped rather than taken
   first-writer-wins.** This is what had been mis-sizing the six-point atlases:
   another effect that also names them declares 64×64 frames, so the 128×128
   pair was being read with the wrong geometry *and* marked fully confident.
   Dropping the hint sends them to the ordinary search, which gets them right.

`to_png` now writes the **whole grid** rather than frame 0, because a single
frame tells you nothing about whether the layout was read correctly and a grid
tells you at a glance.

**[inferred]** The lesson is the one the `.mrb` reader in
[`../../../../tools/wic/README.md`](../../../../tools/wic/README.md) already
records from the other direction: a size model can only rank hypotheses you
already had, and "it decoded without raising" is not evidence. The earlier note's
claim that this path *"independently reproduces the hand-derived layout"* was
true about the byte count and false about the layout, and nothing in the pipeline
could have caught that — only looking at the image could.

### 5.2 Looking at them

**No decoded art is committed** — Massive's textures are read for reference only,
which is why `wic_extracted/` is gitignored and why this note carries arithmetic
and prose instead of pictures. The contact sheets that settled §5.1 are
reproducible from the install:

```
py -3 tools/wic/wic_nuke_sheets.py            # -> wic_extracted/_sheets/*.png
```

Five sheets, and what each one is for:

| Sheet | Shows |
|---|---|
| `01_sixpoint_pair` | `_0pos` RGB, `_0pos` alpha, `_1neg` RGB across 8 frames — the six-point layout in pixels, and the single shared silhouette |
| `02_billowing_pair` | the same for `BillowingSmokeThick_1/2`, the pair the stem, cap and ground ring all draw from |
| `03_fireball_flipbooks` | `Explosions`, `ExploWithFireEnd`, and `SmokeRotate_Nuke` — the one texture authored for this effect alone |
| `04_flares_rings` | the flares, the strip textures that wrap the blast-wave ring meshes, and the DUDV map behind the heat haze |
| `05_fireani_atlas` | `FireAni` whole, as an 8×8 grid — the clearest single demonstration that these are grids and not stacks |

Worth knowing before reading them: **`_0pos` RGB is not a colour image.** It is
three independent scalars — the light response along +right, +up and +front —
displayed as false colour because there is nowhere else to put them. A normal map
would centre on (128,128,255) and these do not; a greyscale replicated across
three channels would be neutral and these are not. §2.4 of the particles note
measures that; the sheet is what it looks like.

`04_flares_rings` also answers something the byte table cannot: `BlastRing_2` and
`BlastWave_Alpha` do not look like rings. They are **strips** — a flat band that
becomes a ring once the mesh's UVs wrap it — which is why the ring is geometry
(§4) and why one authored gradient serves three different meshes.

---

## 6. Every sound

**[BUILD]** Three pieces of audio are the nuke itself, and they are bound in two
different files.

| File | Format | Length | Bound in |
|---|---|---:|---|
| `sound/sfx/nuke_buildup_01.wav` | PCM 16-bit stereo 44.1 kHz | 2.52 s | `maps/supportweapons.ice`, on the tactical aid |
| `sound/hits/explosion_ta_atomic_01_a.wav` | **IMA ADPCM** 4-bit mono 44.1 kHz | 10.69 s | `sound/agentsounds.ice`, variation 1 of 2 |
| `sound/hits/explosion_ta_atomic_01_b.wav` | IMA ADPCM 4-bit mono 44.1 kHz | 7.22 s | `sound/agentsounds.ice`, variation 2 of 2 |

`juice/hiteffects.ice` names the *event* — `explosive_atomic_01` — and
`agentsounds.ice` defines it as a two-variation randomised event with per-variation
falloff. The parameters, in file order after each path:

```
explosion_ta_atomic_01_a.wav   0.000  1.000  178.0  0.480  588.0  0.000  0  4
explosion_ta_atomic_01_b.wav   0.000  0.575  374.0  0.990  828.0  0.000  0  4
```

**[inferred]** From the shape of neighbouring entries (`explosion_ta_clusterbomb`,
`hit_bullet_ta_warthog`) these read as pitch/volume randomisation followed by an
inner radius, a rolloff and an outer radius. What matters is the comparison: the
cluster bomb's outer radius is **234 and 783**; the nuke's is **588 and 828**, and
its inner radius is 178/374 against the cluster bomb's 84/241. **The nuke is audible
across most of the map, and it is the falloff numbers rather than the waveform
that make it feel that way.**

The two variations are 10.7 s and 7.2 s — long enough that they are the tail, not
the crack. **[inferred]** IMA ADPCM at 4 bits is a quality compromise Massive made
for the hit bank generally (it is what `sound/hits/` uses throughout), and a
ten-second explosion tail is exactly the content where 4-bit ADPCM's artefacts
hide.

### Voice and music around it

**[BUILD]** 32 HQ callouts, `sound/feedback/{eu,ru,us}/hq/`:

```
eu_hq_br_{enemy,player}_nuke_001..004      16 files (br + fr)
ru_hq_{enemy,player}_nuke_001..004          8
us_hq_{enemy,player}_nuke_001..004          8
```

**[EXE]** wired through `EXP_GeneralFeedback` with the fields `myNukeDetectedSounds`
and `myNukeNames` — the game announces both *that* a nuke is inbound and *whose it
is*. The campaign adds `ClientCommand('PlayMusicEvent', 'OnNuke')` and the loading
track `music/loading/the_president_and_the_nuke.mp3`.

---

## 7. Everything that is not the particle effect

**[BUILD]** Three systems fire alongside the emitters, and together they are why
the nuke leaves a map that stays changed.

**A dedicated ground-effect manager.** **[EXE]** `WICG_NukeGroundEffect.cpp`,
`WICG_NukeGroundEffectManager::NukeExplosionOccurred`, and a log line
`UpdateDarkening: tot(%d) nuke(%d) obj(%d) wreckDirUpload(%d)` — terrain darkening
is accumulated from three sources and the nuke is its own budgeted category. The
hit-effect struct in the exe is `inner_rad / outer_rad / hit_effect_index /
crater_brush / nuke`, so **`nuke` is a boolean on an ordinary hit effect**, not a
type of its own: any impact can ask for the nuclear ground treatment.

**The crater.** `Nuke_Crater`, 1024² in `explosion_atlas`, projected as a decal on
everything but `METAL`.

**A lighting mood.** `maps/ustown3/moods/nuked.ice` and `maps/norway1/moods/nuked.ice`
are complete mood definitions — `myFog.myColor` at `(4.18, 5.89, 17.52)`,
`myFog.myColor2Sun` at `(4.86, 8.39, 4.74)`, `myFog.myFogEnd 520`, water reflection
scale, cloud wind speed. The campaign switches to it with one call.

**[BUILD]** The campaign's nuke, from `script/maps/ustown3/python/server.py`, is
worth quoting because it shows how little of the effect is scripted:

```python
def StartNukeEndSequence():
    ...
    actList.append(Action(DestroyAllGroups))
    actList.append(Action(Delay, 0, Action(ClientCommand, 'SetMoodFrame', 'nuked', 0)))
    actList.append(Action(building.Building('Suburbia_House_03__0').Damage, 1000000))
    ...            # ~100 more buildings, individually, by name
```

with the client side being two lines:

```python
    SetMoodFrame('nuked', 0)
    EnableWarFilter(True)
```

The blast itself is not scripted at all — the script fires the tactical aid, and
everything in §§2–6 happens because `Support_MiniNuke` is an ordinary support
weapon with a very large hit set. What the script does is the part the effect
system *cannot* do: kill the units, flatten the named buildings, and change the
weather.

---

## 8. The other explosions, measured against it

**[BUILD]** Since every impact is an `.mrb` scene, they are all measurable the
same way: count the emitter slots, count the distinct `.pe` files behind them,
and count how many of those ask for each expensive feature. `slots` is how many
emitters run; `uniq` is how many were authored.

| Effect | slots | uniq | 6-pt | additive | normalmap | CASTSHADOWS | CLUSTERLIGHTING | PHYSICAL | meshes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| **phosphorus bomb** | **69** | **23** | 6 | 16 | 4 | 0 | 3 | 1 | halo ×3, envmap ×2, normalmap, alpha |
| **nuke** | 63 | 14 | 7 | 5 | 1 | **5** | 3 | 0 | additive ×3, halo ×3, **heat haze** |
| cutscene nuke | 62 | 13 | 6 | 5 | 1 | 4 | 3 | 0 | additive ×3, heat haze |
| carpet bombing (NATO) | 38 | 8 | 3 | 5 | 0 | 0 | 1 | 0 | **halo ×18** |
| bunker buster (RU) | 35 | 11 | 4 | 4 | 2 | 0 | 2 | 1 | halo ×2, heat haze |
| bunker buster (US) | 29 | 10 | 4 | 3 | 2 | 0 | 2 | 1 | halo ×2, heat haze |
| fuel bomb | 26 | 9 | 4 | 3 | 1 | 3 | 3 | 0 | normalmap ×2, halo ×2, heat haze |
| mortar (heavy) | 26 | 10 | 6 | 2 | 1 | 0 | 4 | 0 | halo ×3, heat haze |
| daisy cutter | 25 | 7 | 2 | 4 | 0 | 2 | 3 | 0 | halo ×2, heat haze |
| napalm | 22 | 4 | 2 | 2 | 0 | 0 | 1 | 0 | additive, halo |
| gas attack (US) | 11 | 6 | 3 | 1 | 0 | 0 | 0 | 0 | halo |
| artillery barrage | 7 | 7 | 3 | 4 | 1 | 0 | 2 | 0 | halo ×2, heat haze |
| tank shell, ground | 7 | 7 | 3 | 3 | 1 | 0 | 0 | 1 | halo |
| A-10 strafe | 6 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | — |
| cluster bomb | 5 | 5 | 1 | 4 | 0 | 0 | 1 | 0 | — |
| mortar | 5 | 5 | 3 | 1 | 0 | 0 | 0 | 0 | alpha, halo |
| **rifle round, dirt** | **2** | **2** | 1 | 0 | 0 | 0 | 0 | 0 | — |

Four things fall out of that table.

**The nuke is not the biggest.** The **phosphorus bomb** runs more emitters (69)
and nearly twice as many distinct authored effects (23 against 14). What the nuke
has that it does not is *duplication* — five effects instanced eight times each
around a radial array — where the phosphorus bomb is 23 different things happening
once. **[inferred]** Those are two different kinds of expensive, and they are two
different visual reads: the nuke is one enormous coherent structure, the phosphorus
bomb is a chaotic scatter, and the slot/uniq ratio says which is which before you
have seen either. It is also the only one in the table with `PHYSICAL` debris and
`VELOCITYORIENTEDROTATION` streaks, and it has a wider technique spread than
anything else in the game — including
`particle_normalmap_transparent_no_shine_through`, which exists in only 51
effects corpus-wide.

**The heat haze is standard equipment, and shadow-casting is not.**
`postfx_heathaze` appears on seven of the seventeen scenes here, all the way down
to a single artillery shell. `CASTSHADOWS` appears on four, and the nuke has more
of them than the other three combined. **[inferred]** That is a cost ranking
written by the artists: a distortion mesh is one more object in an existing pass,
and a shadow-casting particle writes into the shadow accumulation buffer, which
is a per-particle cost against a target the whole scene reads.

**The cutscene nuke is a copy with the lights taken out.**
`cutscenes/effects/cin_effects/nuke/nuke_gs_nolt_hemicircle.mrb` runs 62 slots to
the in-game nuke's 63, over 13 of the same 14 effects, and the name says what was
done to it — *gs* ground smoke, *nolt* no light, *hemicircle*. **[inferred]** The
cinematic version is the gameplay version with the halos stripped and the geometry
halved, because a cutscene has a fixed camera and does not need the half you never
see.

**Carpet bombing's 18 halos** are the outlier in the mesh column, and the reason
is structural rather than aesthetic: it is many bombs rather than one, so the
scene carries one flare rig per impact.

**And the floor is two.** A rifle round hitting dirt is a two-slot `.mrb` scene
with one six-point-lit puff. It goes through the identical pipeline — hit set,
material row, effect scene, emitter table — as the nuke. **[inferred]** That is
the actual architectural finding here, and it is worth more than any number in
the table: *there is no separate system for the big effect.* The tactical nuke is
what the rifle-hit pipeline looks like when an artist is given a week and no
budget ceiling, and the reason Massive could afford to build it is that they did
not have to build anything to build it.

---

## 9. What to take

**Have one effect format, and let the mesh format carry the structure.** The
nuke is 87 emitters on ~30 nulls whose *transforms* are the effect; a rifle hit
is two emitters on one null; both are `.mrb` scenes and both reference the same
flat forty-keyword `.pe` text file. Massive never added hierarchy, timelines or a
node graph to the effect format, because the mesh format already had all three
and an exporter. **[inferred]** The general rule: before extending the effect
format for everybody, check whether an authored format you already ship has the
structure you need. §1, §8.

**Radial arrays of eight.** Three separate eight-fold arrays — column, ground ring,
cap torus — are what make a billboard cloud read as a rotating volume. Cheap,
authored once, and the reason the silhouette survives the camera orbiting.

**Anchor the self-shadowing term to a node that moves.** §2.2. The cluster-lighting
origin being `Null_Smoke` / `Null_Ground` / `Null_Clouds` rather than the effect's
spawn point means three parts of one effect self-shadow about three different
centres, and it costs one dot product each.

**Put the distortion pass on the same animated object as the thing distorting.**
§4. Two submeshes of one export, two surfaces, guaranteed registration between the
fire and the heat haze with no synchronisation code.

**Spend the unique art on one thing.** The nuke's only bespoke texture is a 340 KB
64-frame flipbook. Everything else is either shared with 224 other effects or is a
32 KB flare. The scale comes from `SIZE 200 300`, `RADIUS 3000` and
`REMOVEDIST 5000` — numbers, not pixels.

**And spend the expensive capability where it is the point.** Half of the game's
shadow-casting particles are in this one effect (§2.3). A feature used seven times
in a 2,100-effect corpus is not a failed feature; it is a feature that was scoped
correctly.

---

## 10. What is not read

- **The `.mrb` node tree** — transforms, keyframes and parent/child links are not
  decoded. `wic_mrb.py` recovers geometry by scanning for mesh blocks and
  deliberately does not parse the tree, so which null carries which of the 63
  emitter slots is **[inferred]** from name and count, not read.
- **The animation curves** on the four meshes and three halos are visible as float
  tracks in the file and are not decoded, so the timing of the blast wave's
  expansion is unknown.
- **`EFFECTS/ATOMBOMB/*.LWO`** are referenced by the scene but the archives ship
  the geometry inside `smoke_2.mrb`; the source `.lwo` files are not present.
- **The `.ice` grammar.** Values here are read by pulling length-prefixed strings
  in file order, so field *names* come from adjacency and the exe, not from a
  parsed schema. Numbers quoted are exact; their labels are **[inferred]** except
  where the exe names them.
- **`sound/agentsounds.ice` keys are hashed**, not stored as strings — the binding
  from `explosive_atomic_01` to the two `.wav` files is by position in the file,
  confirmed by the pattern holding across neighbouring tactical aids.
