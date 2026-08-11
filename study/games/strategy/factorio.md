# Factorio — a million moving sprites at 60 UPS

Deep dive on **Factorio** (Wube Software, 2012–present): how a 2D game with an
unbounded world draws hundreds of thousands of animated sprites at a locked
60 UPS, where its terrain detail comes from, how the simulation stays cheap as
the factory grows without limit, and how every client in a multiplayer game
arrives at bit-identical state from nothing but a stream of button presses.

> **On sources.** Wube have published **Friday Facts** every week since 2013 —
> approaching five hundred posts, written by the programmers who did the work,
> with numbers in them. That makes this the best-sourced note in this directory
> by a wide margin, and the opposite situation to
> [`ruse.md`](ruse.md), where nothing was published and everything had to be
> read out of the shipped build. Here the published record is primary and the
> install is the *check* on it.
>
> Tags: **[WUBE]** stated by Wube in a Friday Facts post, the wiki, or the
> official API docs — with the post number, so it can be re-read.
> **[SHIPPED-DATA]** read from the retail install on this machine,
> `E:\SteamLibrary\steamapps\common\Factorio` — **version 2.0.77 with Space Age**,
> per the bundled `prototype-api.json`. **§10 is that pass**, and it is unusually
> productive because Factorio ships its Lua prototypes, its full modding API
> reference *and its shaders* as readable source. **[COMMUNITY]** player- or
> modder-derived. **[inferred]** our reading.

**Why this game is in a study directory for a tile-based tactics prototype.**
Factorio is the only game here whose world is *unbounded*, whose entity count is
*unbounded*, and which nevertheless holds a hard real-time contract — and it
reaches that by making almost every decision the opposite way round from the 3D
engines in the other notes. It has no LOD chain, no navmesh, no interpolation, no
occlusion culling and no scene graph. It also has, in the belt system, one of the
cleanest published instances of CLAUDE.md's first rule — **do less work, at the
level of what work exists at all** — and in its multiplayer, the most extreme
commitment to determinism of any shipped game covered here.

Related: [`ruse.md`](ruse.md) — the other continuous-zoom note, and the direct
comparison for §4; [`map_scale.md`](../../topics/scale/map_scale.md) and
[`battle_scale.md`](../../topics/scale/battle_scale.md) for the count-versus-depth
trade this game sits at one extreme of;
[`world_streaming.md`](../../topics/world/world_streaming.md), whose §1 names three
ways to have a large world and which this note supplies the fourth reading of;
[`terrain_rendering.md`](../../topics/world/terrain_rendering.md) for the
anti-tiling problem solved here in 2D;
[`elite_dangerous.md`](../space/elite_dangerous.md) for the other game that
generates rather than stores its world, and the other one that treats determinism
as a networking strategy.

---

## 1. The shape of the problem

### 1.1 The first correction: there are not millions of sprites on screen

The reputation — and the question this note started from — is "millions of moving
sprites". The measured number is smaller by two orders of magnitude, and the gap
is the most useful thing in the note.

**[WUBE] FFF-251** benchmarks by rendering "a single frame at max zoom out (about
**25,000 sprites**) 600 times". That is the drawn set: twenty-five thousand
sprites, at the most expensive camera position the game allows.

**[inferred] The simulated set is unbounded and the drawn set is bounded by the
screen, and Factorio's whole architecture is the seam between those two
statements.** A megabase holds millions of items on belts; it draws the few
thousand that are inside the viewport at a resolution where they are
distinguishable. Every rendering decision below is about making 25,000 sprites
cheap, and every simulation decision is about making the millions cheap — and
they are *different problems with different bottlenecks*, which is why the answer
to "is Factorio CPU or GPU bound" is "yes, and not for the same reasons".

This is worth stating plainly because it kills the intuitive model. Factorio is
not a game that draws a million things. It is a game that **simulates** a million
things and draws twenty-five thousand of them, and the reason it feels like a
million is that the simulated million is all *visible in principle* — you can pan
to any of it, and it will be correct when you arrive.

### 1.2 Two budgets, two bottlenecks, and an asymmetric failure mode

**[WUBE] FFF-70, "The smooth FPS"** splits the frame into **update**, **prepare**
and **render**, on distinct threads in a coordinated sequence, and the degradation
behaviour is the part worth memorising:

| Situation | What happens |
|---|---|
| Everything fits | update / prepare / render pipelined, 60 UPS / 60 FPS |
| **Render over budget** | frame skipped, **update continues** — "40 FPS / 60 UPS" |
| **Update over budget** | "the computation of the logic of the factory takes simply too much time" — **both FPS and UPS fall** |

**[inferred] That asymmetry is a design statement, not an implementation detail.**
Dropping a frame costs you a frame. Dropping a tick costs you the simulation rate,
which in a lockstep multiplayer game (§9) costs *everyone* the simulation rate, and
in single-player silently changes how fast the factory runs. So the renderer is
allowed to fail and the simulation is not, and the entire optimisation history
below reads differently once you know which of the two budgets a given post is
defending.

Wube also name the spike problem explicitly — **[WUBE]** "the time of the game
logic update might be not too high in average, but it spikes from time to time and
it is enough to make the game feel laggy" — and their standing answer is to split
large calculations (pathfinding, map generation) into pieces spread across ticks.
**[inferred]** Amortisation as a first-class technique rather than an
optimisation, which is what a hard tick contract forces.

### 1.3 The two bottlenecks, named

- **The simulation is memory-bound.** **[WUBE] FFF-204**: "Factorio is not a
  homogeneous workload - some parts are still limited by memory bandwidth, others
  by CPU." §8 is the evidence, and the community's hardware data agrees from
  outside — **[COMMUNITY]** benchmark leaderboards are dominated by AMD 3D
  V-Cache parts, and RAM CAS latency measurably moves UPS.
- **The renderer is fill-rate bound.** **[WUBE] FFF-281** is framed throughout
  around the GPU having to "read pixels from a texture, and blend it into a
  framebuffer", with GPU cores "stalled by a memory access". The optimisations
  that mattered were all about *overdraw and texture-cache residency*, not about
  triangle count or draw calls — §2.9.

**[inferred]** Both bottlenecks are memory, at different levels of the hierarchy
and for different reasons. Neither is arithmetic. That is unusual enough to be the
headline finding: **a game with a million moving objects and no 3D is limited, on
both sides, by how fast bytes can be moved rather than by how fast they can be
processed** — which is exactly the premise CLAUDE.md's "do it closer" rule rests
on, and exactly why Wube's own answer (§8) is that most of it does not thread.

### 1.4 The scale numbers worth having

| Quantity | Value | Source |
|---|---|---|
| Tick rate | **60 UPS**, fixed | [WUBE] |
| Sprites in a max-zoom-out frame | **~25,000** | [WUBE] FFF-251 |
| Chunk | **32×32 tiles** | [WUBE] wiki |
| World bound | **2,000,000 tiles square** (4×10¹² tiles) | [WUBE] wiki |
| Position representation | 32-bit fixed point, **1/256 tile** | [WUBE] API docs |
| Sprite VRAM, early 2018 | **~2.4 GB** | [WUBE] FFF-227 |
| Sprite VRAM, 0.17 uncompressed → compressed | **3.5 GB → ~1 GB** | [WUBE] FFF-281 |
| Player character sprites | **~4,000** (×2 with high-res) | [WUBE] FFF-218 |
| Render layers | **~50**, hardcoded | [WUBE] API docs |
| Vertex cost per sprite | **80 bytes** (was 144 under Allegro) | [WUBE] FFF-251 |
| Multiplayer protocol player limit | 65,535 | [WUBE] wiki |

---

## 2. The renderer

### 2.1 Leaving Allegro

**[WUBE] FFF-230, "Engine modernisation"** (Feb 2018). Allegro had been in the
codebase "since the first commit", handling window, sound, input and graphics.
The reason for leaving is not performance in the first instance, it is *floor*:

> Allegro copes with a lot of legacy hardware that we don't really have to worry
> about, which makes it hard to expand and build upon.

Concretely, Allegro's graphics paths were **DirectX 9 and OpenGL 1.2**. The split
they chose is worth noting because it is a buy/build line drawn mid-project:
**SDL** for window management, events and input; **their own code** for graphics.
Initial backend OpenGL 3.2, with DirectX 11 planned before release — the stated
reason for D3D11 being that it "allows a new shader model which has a higher
instruction count limit", plus better tooling.

**[inferred]** They kept the boring, well-solved, platform-shaped part (windowing
and input) and took ownership of the part where their requirements were unusual.
That is the same line [`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md)
§10 finds — buy what is generic, build what the product is about — arrived at by a
team going the other direction, from bought to built.

The pace is worth recording as a sanity check on how large this kind of rewrite
actually is: rendering was fully de-Allegro'd and matching the old output **within
a few weeks**, with the new feature work built on top afterwards.

### 2.2 Batching, and the one number that describes the vertex path

**[WUBE] FFF-251, "A Fistful of Frames"** is the CPU-side architecture. Batching
already existed under Allegro — it "means it draws multiple sprites that use same
texture and rendering state, using a single command sent to the GPU" — what
changed is how vertices reach the GPU, because "in DirectX 10, there are no
functions for drawing from C-arrays directly".

The mechanism, verbatim, because it is the whole design:

> When we finish writing a batch to the buffer, we don't send a draw call right
> away, we write where this batch starts and ends into a queue, and keep writing
> into the buffer. When the buffer is full, we unmap it from system memory, and
> send all the queued draw calls at once.

One persistent streaming vertex buffer; batches recorded as ranges; a single flush
when the buffer fills. **[inferred]** So the CPU cost per sprite is a memcpy into
a mapped buffer and nothing else — no per-sprite API call, no per-batch state
change, no driver validation until the flush.

The vertex format went **144 bytes per sprite (6 vertices) under Allegro → 80
bytes (4 vertices)** plus "a static index buffer to expand them to two triangles".

**A 48-byte point-sprite form was rejected**, and the reason is a portability
argument that is worth keeping as a cautionary tale about geometry shaders:
"Metal (and therefore MoltenVK) on macOS doesn't support geometry shaders at all",
and measured performance *regressed* on older machines even though newer GPUs
liked it. **[inferred]** A 40% bandwidth saving declined because it would have
cost a platform and hurt the low end — which is the right call for a game whose
hardware distribution is as wide as this one's, and the opposite call to the one a
console title would make.

### 2.3 One blend state for the entire renderer

**[WUBE] FFF-172, "Blending and Rendering"** contains the cleverest small idea in
Factorio's renderer, and it is the reason §2.2's batching works as well as it does.

Colour channels are stored **pre-multiplied by alpha**. Every sprite is drawn as a
"colored polygon with texture" — that is, it carries a tint as vertex data. And
then:

- a tint of `{r=1, g=1, b=1, a=0}` produces **additive** blending "without
  requiring graphics API state changes";
- a tint of `(1, 1, 1, 0.35)` produces **partial-additive** — used for fire and
  smoke, so "the brightness of overlapping flames adds up partially, but the
  flames don't completely lose their details".

**[inferred] The blend mode is data, not state.** With premultiplied alpha, the
tint's alpha scales the destination-attenuation term, so driving it to zero leaves
`dst + src` while leaving it at one gives normal alpha blending — and every value
in between is a legitimate mix. The consequence is the important part: **an
additive sprite and an alpha-blended sprite can sit in the same batch**, because
switching between them requires no API call. In an engine whose entire performance
strategy is "never break the batch", moving a piece of GPU state into the vertex
stream is worth far more than the four bytes it costs.

That is a directly transferable trick for this project's UI painter and any
particle work: *if a state change is preventing batching, look for a way to encode
it as a vertex attribute instead of promoting it to a separate pass.*

### 2.4 Atlases are specialised by what is drawn together

**[WUBE] FFF-227**. At the time, sprites alone occupied **~2.4 GB of VRAM**, plus
several hundred MB of working buffers and minimap data. The response was not one
atlas but several, sorted by co-occurrence in the draw order:

> terrain tiles, shadows, GUI graphics, icons, and recently objects that are drawn
> under shadows, are all separated into their own atlases

**[inferred] The rule that makes this coherent is that a batch breaks on a change
of source atlas**, so atlas membership is really a draw-order decision wearing a
memory-management hat. Things drawn consecutively must live together; things never
drawn near each other should not compete for the same atlas space.

The evidence that this is the operative rule is a *bug*. **[WUBE, posila]** Some
2.0 nuclear items had their glow layer packed into a different atlas from their
base sprite (via the `"light"` sprite flag), so every such entity forced a texture
switch and the draw call count exploded — "layers of one thing should be ideally
organized to the same atlas to allow sprite batching". Fixed in 2.0.16.
**[inferred]** A performance cliff caused by an *asset flag*, in a system where
the flag looks like a rendering hint and is actually a batching instruction. Worth
remembering when designing any content-authored atlas assignment.

**Mipmaps** arrived in the same post, and the justification is the clearest
statement of Factorio's zoom problem: an entity 256 px wide at 1:1 is only 32 px
when zoomed out, so **only every 8th source texel is sampled**, which thrashes the
texture cache. Costs named honestly: extra VRAM, and sprites "cannot be packed as
tightly in the atlas because they would bleed into each other in the downscaled
levels". In 0.16 mipmapped textures were always trilinear — two mip levels touched
per sample — which **[WUBE] FFF-281** later relaxed to fetching from the closest
finer level.

### 2.5 Texture compression, and the fact that compression made it *faster*

**[WUBE] FFF-281**. The VRAM ladder:

| State | Sprite atlas VRAM |
|---|---|
| High-res, uncompressed | **3.5 GB** |
| 0.16 (shadows compressed only) | **~2.5 GB** |
| High-quality compression, no mipmaps | **~1 GB** |

Target: high-res playable on **2 GB** cards. Formats: BC1 4 bpp, BC3 8 bpp, BC4
4 bpp against raw RGBA at 32 bpp; **shadows are always BC4**, being single-channel.
The default is **YCoCg-DXT** — "luma in alpha channel for higher quality
compression and chrominance in RGB channels", implemented as BC3 + BC4 = **12 bits
per pixel** — with plain BC3 as the low-quality option. BC7 was rejected as "not
supported by DirectX 10 class hardware, and it is not available in OpenGL on
macOS".

The line that matters most is not about memory at all:

> render up to twice as fast due to caches being able to fit more pixels in
> compressed formats

**[inferred] A 2× render speedup from compression alone confirms §1.3's
diagnosis** — the renderer is bound on texture memory traffic, so shrinking the
bytes *is* the optimisation, and quality-per-bit is a performance parameter rather
than an aesthetic one. It also explains why they needed YCoCg rather than plain
BC3: block compression artefacts differ between adjacent animation frames
("even tiny changes in individual pixels results in larger changes in blocks"), so
a cheap codec makes animations *shimmer* — a temporal artefact from a spatial
compressor.

### 2.6 The virtual atlas — and an honest status caveat

**[WUBE] FFF-264, "Texture streaming"** describes a virtual-texture system: "All
sprites are put into a single virtual atlas, the size of which is not restricted
by hardware limits", divided into **128×128 pixel tiles**, with the needed tiles
uploaded per frame into a smaller physical texture and an **indirection table**
mapping virtual coordinates to physical ones in the pixel shader. Two properties
are attractive: every sprite shares one virtual coordinate space so atlas
boundaries stop breaking batches (§2.4), and the atlas has mip levels so the game
can "stream-in only the mipmap levels that are needed for the current zoom level".

The hard constraint is stated: **"the entire virtual atlas needs to be in RAM"** —
they will not page from disk because it "has very high latency" and risks sprite
pop-in.

**Caveat.** The post describes this as not yet good enough — "it is still not good
enough" and "we are not sure yet if it will be feasible for us to do without
introducing bad sprite pop-ins" — and what shipped for the VRAM problem in 0.17
was §2.5's compression.

**But something streaming-shaped did ship, and the install proves it.**
**[SHIPPED-DATA]** The `SpritePriority` type in the bundled API docs describes
itself as setting

> the 'caching priority' of a sprite, so deciding priority of it being included in
> VRAM instead of streaming it

**and cites FFF-264 by URL.** The seven values — `extra-high-no-scale`,
`extra-high`, `high`, `medium`, `low`, `very-low`, `no-atlas` — are the same
`priority = "extra-high"` fields visible throughout the shipped entity prototypes
(§10.2). **[inferred]** So residency is real and prioritised, and content declares
its own importance; whether the 128×128-tile indirection scheme survived in the
form FFF-264 describes is still **not established** (§12), but "sprites are
streamed against a VRAM budget by declared priority" is a retail feature, not
abandoned R&D.

### 2.7 Draw order: a hand-stratified painter's algorithm

**[SHIPPED-DATA]** The `RenderLayer` type in the bundled API docs enumerates
exactly **71** global layers in fixed order, "from lowest to highest", with the
note that "most of the objects have it hardcoded in the source, but some are
configurable". Within a layer, draw orders are additionally sorted by world
position. The full list is in §10.3; the shape of it is: `zero`,
`background-transitions`, `under-tiles`, `decals`, `above-tiles`, five generic
`ground-layer-1…5` slots for mods, radius visualisations, `resource`,
`building-smoke`, **five rail material layers**, `decorative`, ground patches,
`remnants`, `floor`, the transport-belt group, `corpse`, `item`, the object
group, **a parallel six-layer elevated-rail stack**, `fluid-visualization`,
wires, `explosion`, `projectile`, `smoke`, `light-effect`, and finally the
selection and `cursor` affordances.

So: **back-to-front painter's algorithm, no depth buffer for world sprites**, with
the global ordering authored by hand rather than derived.

**[WUBE, posila]** They are explicit that this is now load-bearing and immovable —
"we are content locked into current method of sorting sprites back to front" — in
a forum answer explaining why they will not add a height map. The reasons given
are worth quoting because they are the general argument against retrofitting depth
into a 2D engine: "All sprite assets were already made with antialiased edges and
we were heavily utilizing transparency", both of which are incompatible with
depth-tested rendering; and the depth would only be known "as a result of sampling
the height map in the pixel shader, so it might end up being less efficient" than
the current per-pixel cost of "2 reads and 1 write".

**[inferred]** The prize they were weighing it against is real — depth testing
would let them "sort and batch sprites by texture", collapsing the atlas problem
entirely — and they still said no, because **ten years of assets were authored
against the assumption**. That is the clearest example in this directory of an
early rendering decision becoming permanent through content rather than through
code, and it is worth holding in mind for any project about to author thousands of
sprites.

Evidence of how hand-curated the layer list is: **[WUBE] FFF-378** mentions "a
very special patch that draws under terrain which is visible only in places where
the ramps or supports touch water" — a bespoke render layer created for one visual
case.

### 2.8 The prepare step is where the parallelism is

**[WUBE] FFF-215**: the prepare logic "gathers all the data (sprite draw orders)
for rendering and it is doing in parallel up to **8 threads**", iterating the game
world, and is "quite short".

**This is the exception that proves §8's rule, and the reason is stated: prepare
is read-only over the world.** Threads that only read may each hold their own copy
of a cache line; threads that write invalidate each other. §8.1 is the same post
finding that the *update* would not thread for exactly this reason.

**[inferred]** Note what this buys architecturally. The expensive world walk —
visit every visible entity, work out what sprites it wants, at what position, in
what layer — is precisely the part that scales with base size, and it is the part
that parallelises perfectly. The submission that follows is serial and cheap
because §2.2 made it a memcpy. *Split the frame at the read/write boundary and the
threading question answers itself.*

### 2.9 What actually made the renderer faster: killing overdraw

**[WUBE] FFF-281** is a list of fill-rate wins, and the shape of them is
consistent — none is about draw calls or geometry.

- **Invisible smoke.** Particles update once per **120 ticks** with interpolation
  between updates, and a bug left fully-transparent particles alive until their
  next update, producing a "huge overdraw blob". Fixing it "reduced number of
  particles being drawn by 15%, and reduced the number of pixels being rasterized
  even more". Particles below **2% opacity** are now skipped. **[inferred]** The
  cost was entirely in pixels that changed nothing — the worst kind of work, and
  invisible to any profiler looking at entity counts.
- **Turret range visualisation**, which is a nice worked example of a
  coverage problem. Ranges are drawn opaque into an offscreen buffer then
  composited semi-transparently, so overlaps read as one shape. A stencil approach
  gave "3x speed-up in our extreme test cases (20x20 turrets)" and was judged
  insufficient; evaluating the turret list per pixel was worse, because pixels
  outside every range still iterated the whole list. What shipped: render into a
  buffer with **16× smaller dimensions**, stencil-mark only the *uncertain*
  boundary pixels, and run the expensive shader only there — "up to 20x faster".
  **[inferred]** Solve the interior and the exterior cheaply at low resolution and
  pay full price only on the boundary band. That is hierarchical coverage, and it
  generalises to any "union of many overlapping shapes" visualisation — including
  this project's movement and ability ranges.
- **Lower-resolution effect buffers** for smoke and anything without "important
  high frequency detail", plus an option to render the game view below native
  while keeping the GUI sharp.
- **Tighter sprite geometry** — replacing the quad with a fitted hull so "most of
  the fully transparent areas won't be rasterized" — stated as a plan.

Benchmark setup, for calibration: **[WUBE]** 1920×1080 (GTX 980 also at 4K), i7-4790K,
high-res sprites and mipmaps on, target under 16.66 ms, measured as "time a frame
was being processed by GPU, for 1000 frames", across hardware down to an Intel HD
Graphics 5500 laptop.

**GUI rendering** got the same treatment earlier: **[WUBE] FFF-182** records the
production graph being drawn one line segment per draw call, a trains GUI that
rendered 50 minimaps 50 times over (an accidental O(n²)), and a blueprint preview
with "zero batching of sprites" — after which "it has no measurable impact on
performance". **[inferred]** Worth including because it is the reminder that the
unbatched path is usually in the UI, where nobody profiles.

---

## 3. Sprites: the pre-rendered pipeline

Everything Factorio draws in the world is a **pre-rendered image of a 3D scene**.
The models are built and lit in Blender, rendered to sprite sheets at build time,
and shipped as images. Nothing is lit at runtime except by an additive light map
and a colour grade (§3.4). This section is the answer to "how do they animate
stuff" and "do they use normal maps" — and the second answer is no.

### 3.1 The pipeline: Blender → Photoshop → After Effects → Python

**[WUBE] FFF-194, "Automated combinator pipeline"** is the clearest account.
Blender is the origin of "nearly all" Factorio graphics; scenes are pre-configured
so the resolution can be doubled for the high-res pass; objects are split into
horizontal and vertical layers because of projection differences; rendering is
triggered by an automated button. Photoshop then produces layered output whose
"main technical criteria… is the ability to import the working file into After
Effects as separate layers" — twelve combinator variants in one PSD, layers
composited with Screen and Multiply, and **ground integration produced by
Photoshop's Drop Shadow**. After Effects combines the Blender renders with the
Photoshop masks. The pipeline steps are named **Render → Ps-EDIT → Sequence →
Output**.

Then the part worth stealing. Wire connection points — "8 types × combinator
variants", producing "80 shifting values just for the wire connections" and
"we exceed 111 shiftings" overall — are not maintained by hand. **Coloured pixel
markers are baked into the render itself and parsed back out in Python**, and a
generator emits **2,000 lines of combinator Lua** automatically.

**[inferred] The 3D scene is the single source of truth for attachment points, and
the transport mechanism is the image.** A muzzle position, a wire terminal, a
smoke origin — all of them exist in the model, all of them must survive into a 2D
sprite whose relationship to the model is a projection, and encoding them as
coloured pixels means they cannot drift out of sync with the art, because they
*are* the art. Any pipeline that bakes 3D to 2D has this problem, and this is a
better answer than a hand-maintained offsets table.

### 3.2 What an animation costs to produce

**[WUBE] FFF-218, "Import bpy, Export player"** on the player character alone:

| | |
|---|---|
| Blender files merged into one project | **21** |
| Individual sprites | **over 4,000** — "double that number when we also consider high resolution" |
| Animation sequences | 7 (idle, idle with gun, mining with/without axe, running, running with gun, corpse) — **each with two armour variations** |
| Render time on one PC | **over 40 hours**, hence a distributed render farm |
| Vertices processed by their mesh-linking tool | ~5 million |

Blender **render layers feed compositor nodes**, with special handling for the
**Ambient Occlusion** and **Shadow** passes — direct evidence that the shadow
sprite and the baked AO are separate render passes composited at build time, not
runtime effects.

And explicitly: **high-res is not an upscale.** Re-rendering at high resolution
"requires shader and detail adjustments".

The scale of the offline cost shows up again in **[WUBE] FFF-283**: "The Rocket
Silo without the rocket uses sixteen 8192x8192px textures which makes our 1080Ti
run out of VRAM to render." **[inferred]** That is the *authoring* machine running
out of memory, not the game — the asset budget moved from runtime to build time,
which is the deal a pre-rendered pipeline makes. **[WUBE] FFF-378** records the
same pressure on elevated rails: "the required sprite count grew very rapidly",
needing "completely new Blender Python tools that mostly help with organizing and
rendering large amounts of outputs", and about nine months of graphics work.

The high-res project as a whole ran from 0.15 to roughly 1.0 — **[WUBE] FFF-355**
puts it at **3.5 years**.

### 3.3 The layers an entity is made of

**[WUBE]** for the existence of each; **[inferred]** for the consolidated list:

1. **Main sprite** — the shaded colour render, with key light, ambient occlusion
   and material response already baked in.
2. **Shadow** — a separate sprite, in its own atlas, **always BC4** compressed
   (single channel), on its own render layer, drawn beneath objects. Splitting
   sprites from their shadows was named in FFF-227 as an explicit VRAM
   optimisation.
3. **Colour mask** — for player/force colours, applied as a runtime tint.
4. **Light / glow layer** — `draw_as_light`; `draw_as_glow` is, in Klonan's words,
   "a shortcut" that "draws the animation first as a normal sprite, and again as a
   light layer".
5. Optional working-status overlays, remnants, and per-planet overlays in 2.0.

**[inferred]** The interesting property is that this list is *the same decomposition
a deferred renderer would make* — albedo, shadow, mask, emissive — except resolved
at build time rather than per pixel. Which is the general trade of the whole
approach: **a pre-rendered pipeline is a renderer whose expensive passes ran on
the artist's machine, and whose parameters are therefore frozen.** You get
arbitrary shading quality for free at runtime, and you cannot change the light
direction, ever.

### 3.4 Lighting: no normal maps, and what runs instead

**There are no normal maps and no per-pixel relighting of world sprites — with
exactly one exception, which is worth the detour.** The reasoning is
**[WUBE, posila]**'s explanation of why they rejected a height map, quoted in
§2.7 — antialiased edges plus heavy transparency plus back-to-front sorting rule
out depth, and the shading is already baked. What exists at runtime is two things:
an additive **light map**, and a **night colour grade**.

The exception is measured rather than argued. **[SHIPPED-DATA]** `normal_map`
occurs **once** in the entire prototype API and **zero** times in the runtime API,
on a single type — `AsteroidVariation`, which carries `color_texture`,
`normal_map` and `roughness_map` — used only by Space Age's asteroids. And it is a
genuine real-time lit shader: `core/graphics/shaders/asteroid.frag` ships as
readable source with **four dynamic lights**, tangent-space normals with
reconstructed Z, roughness in the alpha of the roughness map, a subsurface term,
and — the detail that explains why it had to exist — **the normal is rotated by
the sprite's own spin angle** so a tumbling 2D sprite lights correctly.

**[inferred] That is the exception proving the rule, and it names the rule
precisely: baked lighting works until the object's orientation stops being fixed.**
Every other entity in the game sits on the ground at a known angle under a known
sun, so its shading can be baked. An asteroid tumbles freely in space, so no
single baked light direction is correct for it, and Wube wrote a bespoke
normal + roughness + subsurface shader **for one entity class** rather than
generalising the renderer. *Add the expensive path where the assumption breaks,
not everywhere the assumption exists.*

**The light map, before 1.1.** **[WUBE, posila]** "Pre-1.1 the game had just 1 type
of lights, large spotlights usually with very wide falloff. These lights are
rendered to a lightmap of **1/4th size of the game view**."

**1.1 split it in two.** "In 1.1 light rendering was changed to separate light maps
affecting large areas (like lamps) and detail lights that are usually just tiny
LEDs." The reasoning is a resolution argument in both directions: area lights at
full resolution cost a lot for no visible gain, and detail lights at low
resolution *flicker* as the camera moves, especially zoomed out. So two paths at
two resolutions.

**The 1.1 flaw, and the 2.0 fix, are the interesting part.** Because detail lights
were rendered in a separate queue to a separate target, a light behind another
sprite would **shine through it**. In 2.0 they restructured to **multiple render
targets in one pixel shader invocation** — **[WUBE, posila]** verbatim:

> Now game sprites are rendered into multiple render targets in single pixel
> shader invocation - game view, and light map. Detail light sprites are just
> regular sprites with a flags which pixel shader interprets and either blends the
> sprite to game view normally and as light occluder into the lightmap, or as
> additive light into the lightmap.

**[inferred] One geometry pass, two targets, and a per-sprite flag choosing
between "colour + occlude light" and "additive light".** Occlusion in the light
map is what fixes shine-through, and it costs nothing extra in geometry because
the sprites were being rasterised anyway. This is the same move as §2.3 — put the
behaviour in a per-sprite flag rather than in a separate pass — applied one level
up.

**And this one can be read rather than inferred**, because the shader ships as
source (§10.5). **[SHIPPED-DATA]** `core/graphics/shaders/sprite_light.frag`
declares exactly the two outputs, and resolves the "flag" into bits of a flat
per-vertex integer:

```glsl
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 lightColor;
...
vec4 getLightColor(vec4 color, uint extra)
{
    if ((extra & 48u) != 0u)          // bits 16|32 → contributes light
        return vec4(color.xyz, 0.0);
    else                              // otherwise → occludes light
        return vec4(0.0, 0.0, 0.0, color.w);
}
```

with bit 16 separately zeroing the sprite out of the *colour* target in
`getFragColor` — that being the `draw_as_light` case, a light with no visible
surface. The companion `sprite.frag` shows the rest of the same bitfield doing
`invert_colors` (bit 4), tint versus **`tint_as_overlay` as a real hard-light
blend** (bit 2), the colour LUT lookup (bit 8) and greyscale (bit 1).

**[inferred] So §2.3's "the blend mode is data" understates it: *the entire
per-sprite shading vocabulary* is bits in one integer**, evaluated by branches in
a single übershader that never changes pipeline state. Six independent visual
behaviours, one shader, one batch.

The cost is stated and is a real regression on the low end: **[WUBE, posila]** "It
is prette cheap even on lower end dedicated GPUs, but makes every game view pixel
about **50% more expensive** to render on integrated GPUs with much lower memory
bandwidth." **[inferred]** Predictable: MRT doubles the write bandwidth per pixel,
and §1.3 already established that this renderer is bandwidth-bound.

**Day/night is a LUT.** **[WUBE] FFF-320, "Color correction"**: the old
implementation was "playing with the alpha channel of a solid black layer on top
of the game"; the new one uses look-up tables so that at night "the colors are
losing their saturation and becoming more blue and cold".

**[inferred] Put the two together and the model is: night applies a colour grade
to the whole game view, and the light map attenuates how much of that grade is
applied.** A lamp does not add light so much as **locally cancel the night
grade** — which is exactly why lamps cannot make a scene brighter than daylight,
and why the look holds together despite every sprite having been lit by a fixed
sun at build time.

Lights are sprites, not analytic falloffs. **[WUBE]** `LightDefinition` supports
`"basic"` and `"oriented"` (the latter requiring a `picture`), plus `intensity`,
`size` — "The radius of the light in tiles. Note that the light gets darker near
the edges" — `minimum_darkness` so a light only renders past a darkness threshold,
and a flicker group including `offset_flicker`, which "offsets tick used to
calculate flicker by position hash. Useful to desynchronize flickering of multiple
stationary lights". **[inferred]** That last one is the same class of trick as
`broken_arrow_squads.md`'s six independent randomisations: *a hash of position
turned into a phase offset so that identical objects do not act in unison.* One
line, and it is the difference between a row of lamps and a row of one lamp copied.

Finally, a rendering consequence of the light map that is easy to miss: **[WUBE]
FFF-421** notes they "check edges of screen for different reasons of similar kinds
(lamps making light from offscreen for example)" — an off-screen lamp still lights
on-screen pixels, so view culling for lights has to be wider than for sprites.

---

## 4. Zoom, and the absence of LOD

The question this note started from was how Factorio handles LOD across its zoom
range. The answer is that **it has no LOD system in any of the four senses
[`lod_systems.md`](../../topics/world/lod_systems.md) separates** — no geometry
chain, no aggregation into impostors, no deformation tiers, no AI LOD — and the
reasons are worth more than the technique would have been.

### 4.1 Mipmaps are the entire LOD system

Zoom out in a 3D engine and you swap meshes. Zoom out in Factorio and the *only*
thing that changes is which mip level the GPU samples.

**[WUBE] FFF-227** states the problem in exactly the terms that make mipmapping
the answer: a 256 px sprite drawn at 32 px means "only every 8th pixel" is
sampled, thrashing the texture cache and dropping the framerate on zoom-out. Mips
fix the sampling; **[WUBE] FFF-264** extends it to residency — the virtual atlas
"can stream-in only the mipmap levels that are needed for the current zoom level",
so **zoom controls what is in VRAM, not merely what is filtered**.

**[inferred] A mip chain is a LOD chain.** It is generated automatically, it is
selected per pixel by hardware with no popping, it costs a third of the source
memory, and — crucially — **it needs no authoring**. Compare
[`lod_systems.md`](../../topics/world/lod_systems.md)'s Total War numbers: 237
models × 4 LODs each, a whole generation pipeline, switching distances to tune,
and an impostor system to claw back the draw calls. Factorio needs none of it,
because in 2D the LOD problem *is* the texture filtering problem, and that was
solved in hardware in 1998.

This is the concrete form of a general point: **the cost of LOD is a function of
the representation, not of the content.** Factorio has vastly more distinct
objects on screen than any game in this directory and pays nothing for their
level of detail, because a sprite's simplification is a filter and a mesh's
simplification is a decision.

### 4.2 What zoom does *not* invalidate

The second half of the answer is that Wube arranged for zooming to be cheap on
the CPU side too, and they had to do it deliberately.

**[WUBE] FFF-323**: "we cache draw orders per-chunk, and we only run prepare render
on chunks that have just entered the player view" — and then the sentence that
matters — "**changing render scale doesn't need to invalidate the new cache, so
zooming doesn't cause prepare render to run**."

**[inferred]** That is a designed property, not a coincidence. A per-chunk draw
order that stored *screen* positions would be invalidated by any zoom change, and
zooming would then re-run the most expensive CPU pass in the frame on every chunk
in view, every frame, for the duration of the zoom — which is precisely when the
number of chunks in view is largest. Storing world positions and letting the
vertex path apply the scale keeps the cache valid across the entire zoom range.
*A cache's key determines what invalidates it, and the invalidation set is worth
designing before the cache is.*

The terrain cache (§6.5) has the same property for the same reason.

### 4.3 Read against R.U.S.E.

[`ruse.md`](ruse.md) §1.2 concludes that R.U.S.E. "is not an extent problem and it
is not a count problem — it is a *view* problem": the camera may sit anywhere from
twenty metres above a tank to the whole map in frame, and the engine must have a
correct representation at every point on that continuum. Factorio has exactly the
same requirement and answers it with almost nothing, and the comparison is
instructive:

| | R.U.S.E. / IRISZOOM | Factorio |
|---|---|---|
| Terrain across zoom | **two independently baked meshes** (327.68 m and 655.36 m chunks) | one tile set + mip levels |
| Distant objects | impostors carrying their own sun-view matrix and depth shadow | the same sprites, smaller |
| What changes with zoom | streaming priorities, mesh selection, impostor swap | which mip is resident |
| Authoring cost of the above | a content pipeline | none |

**[inferred] The difference is not cleverness, it is dimensionality.** In 3D, a
distant object's silhouette, its shading and its parallax all change with
distance, so a smaller version of it is a *different asset*. In a fixed-projection
2D game, a distant object is the same image at a different scale — literally, by
construction — so the only question left is filtering. Factorio bought its
seamless zoom with the projection, and paid for it in the freedom it gave up in
§2.7 and §3.4: no runtime relighting, no camera rotation, no depth.

### 4.4 The gap: the chart view

Beyond some zoom-out, Factorio switches to **chart (map) view**, which draws a
schematic of charted chunks rather than sprites. The threshold at which this
happens, and how chart view is actually rasterised, were **not established** — no
Friday Facts post covers it. The obvious guess is per-chunk cached chart textures
updated as radars re-scan, and the `Chart update` row in Wube's own performance
breakdown (§7.9) with the advice "reduce your number of radars" is consistent with
it, but that is inference and §12 records it as unverified.

---

## 5. The infinite world: chunks, and generation as a pure function

### 5.1 The unit of the world is 32×32 tiles, and it is never thrown away

**[WUBE]** A tile is the atom — "a square which defines the smallest possible
piece of the game world", and "one tile is generally assumed to be one square
metre in size". Chunks are **32×32 tiles, 1,024 tiles each**
([wiki, *Map structure*](https://wiki.factorio.com/Map_structure)).

The world is not literally infinite. **[WUBE]** "The maximum size of the map is a
square 2 million tiles on each side, a total of 4 trillion tiles" — ±1,000,000
tiles from the origin. **[inferred]** That bound is not a design choice about
worlds, it is the position representation showing through: positions are stored
as **32-bit fixed point with 8 fractional bits** (§9.2), which gives ±2²³ ≈ 8.4M
tiles of range, and 10⁶ is a round number comfortably inside it. *The world is as
big as the coordinate type, and the coordinate type was chosen for determinism.*

Generation is on demand and slightly ahead of you. **[WUBE]** "20 chunks distance
in each direction around each player is slowly generated over time, or immediately
if revealed directly and not yet generated"; outside the visible area "an
invisible area of about 3 chunks wide is generated as a preloading mechanism"
([wiki, *Map generator*](https://wiki.factorio.com/Map_generator)).

**Charting generates.** **[WUBE]** "If a far-away and thus ungenerated chunk is
charted, it will be generated, together with the above-mentioned invisible 3 chunk
radius." So the radar is not a viewer of the world, it is a *producer* of it —
revealing map is the same operation as creating map.

And then the part that separates Factorio from every streaming system in
[`world_streaming.md`](../../topics/world/world_streaming.md): **nothing is ever
unloaded.** **[WUBE]** "The generated chunks are fully mapped and stored in the
player's RAM, which is the practical limiting factor of exploration." There is no
paging, no eviction, no LRU. A chunk you drove past once in hour three is resident
in hour four hundred.

**[inferred] This is the decision that everything else in the game is downstream
of, and it is only affordable because of what a chunk *is*.** A Factorio chunk is
a tile array, an entity list and some cached grids — kilobytes, not the megabytes
an equivalent 3D cell costs in meshes, textures and collision. Having made the
world cheap per unit area, Wube can decline to build a streaming system at all,
and the entire class of bugs that
[`world_streaming.md`](../../topics/world/world_streaming.md) §3 is about — an
entity spawning where the client has not finished streaming — cannot occur.
*The cheapest streaming system is the one a small enough working set lets you not
write.*

What they build instead is **activity gating**, which is the same idea applied to
time rather than space. **[WUBE]** "Entire chunks are set inactive when nothing
important is happening in them. E.g.: Chunks with only fish or idling enemies may
be set inactive, halting their idle movement." **[inferred]** So the resident set
grows without bound and the *updated* set does not — memory is the thing they let
scale, and CPU is the thing they defend. That is the right way round for a game
whose contract is a tick rate.

### 5.2 Generation is a pure function of position

**[WUBE]** From FFF-390: "All you really have to work with is the X and Y
position. The terrain generator can't know anything about what is already placed."

**[inferred]** That single constraint is what makes everything above legal. If
generation is `f(x, y)` with no dependence on generation *order*, then chunks may
be generated in any order, at any time, on any machine, and the results agree —
which is simultaneously the requirement for on-demand generation, for multiplayer
determinism (§9), and for a map exchange string being a complete description of a
world. **A pure generator is not a purity aesthetic; it is what lets the same
world exist in twelve places at once without ever being transmitted.**
[`elite_dangerous.md`](../space/elite_dangerous.md) reaches the same arrangement
from the other end — a galaxy too large to store, therefore generated, therefore
identical everywhere.

### 5.3 Noise, and the language they built to author it

The generator went through three generations, and the direction of travel is
consistent: *from a noise function, to a program.*

**[WUBE] FFF-112, "Better noise"** — the diagnosis of stock Perlin: it "is
originally intended to provide a procedural texture for 3D graphics, not for
generating large 2D surfaces." What shipped is a hybrid — "a mix of Perlin noise
and the newer simplex noise" plus "a very cool hash function from *Better Gradient
Noise*", keeping "the simple square grid from Perlin noise, the gradient summation
idea from simplex noise."

The performance note in that post is the transferable one, and it is a data-layout
argument rather than a maths one: the fix was **"to not generate values for every
tile separately, but to group all calculations required for the whole chunk and
reuse the intermediate steps between individual tiles."** **[inferred]** Per-tile
evaluation recomputes every octave's lattice setup 1,024 times per chunk; batching
by chunk turns the generator from a function called per tile into a kernel run per
chunk, and the intermediate results become loop-invariant. This is exactly
CLAUDE.md's "where a call sits relative to a loop", at the scale where it is worth
an architecture rather than an edit.

**[WUBE] FFF-207, "Lua noise specification"** introduced authored composition —
"composition of noise functions from Lua code, so that we (and modders) can have
more control over how the map is generated" — with terrain written as expressions
over `new_basis_noise`, `noise.ridge`, `noise.max`.

**[WUBE] FFF-258, "New autoplace"** (0.17) unified the model: the generator
"calculates probability and richness for every autoplaceable tile, entity, and
decorative at every point on the map". Terrain properties (elevation, temperature,
moisture, aux) and resource placement became one system compiling to expression
trees.

Ore patches use **spot noise**, and the algorithm is worth stating because it is a
neat solution to "place discrete things with a continuous function": the map is
divided into regions, random candidate points are generated per region, then
"density, quantity, radius, and favorability are calculated for each point, based
on noise expressions", and "points are sorted according to their favorability,
highest-to-lowest" until the region's target quantity is met. **[inferred]** Ore
therefore avoids water without any rule saying so — water is simply low
favourability, and the sort means the bad candidates are never reached. *Sort by a
scored quantity and consume greedily until a budget is met* is the same shape as
CLAUDE.md's "sort before the expensive test, take the first pass".

**Starting area guarantees are explicit, not emergent.** **[WUBE]** "The starting
area contains only iron, copper, coal and stone, in very predictable amounts.
Uranium and oil are explicitly excluded from the starting area", patches are
"usually in one ore patch each" and "usually close together"; outside it "the
regular algorithm 'kicks in' so you can still get quite wild results, but they are
far enough that it averages out." **[inferred]** A generated world still needs an
authored opening, and the honest way to get one is a separate rule for the first
few hundred metres rather than a global distribution tortured until its tail is
survivable.

### 5.4 Noise expressions 2.0 — when the terrain language got a compiler

**[WUBE] FFF-390, "Noise expressions 2.0"** (Dec 2023, by Earendel — who had
built comparable tooling for Space Exploration's planets before joining Wube) is
the post to read, because it is a *compiler optimisation* post about terrain.

Named noise expressions historically compiled to **three noise programs** (tiles;
cliffs; entities and decoratives); the 2.0 refactor collapsed this to **one noise
program per surface**, with procedures eliminated and outputs inlined. The numbers:

| Change | Effect |
|---|---|
| Expression-object deduplication | Vulcanus: **~280,000 objects → ~5,300** |
| Constant folding | chunk compilation **18.35 ms → 2.83 ms** |
| Overall | runtime map generation "up to 20% improvement"; program compilation "up to 50% faster" |
| String-based expression syntax (replacing verbose Lua tables) | "50% less time during prototype initialization" |

**[inferred]** Two readings. First, a 53× reduction in expression objects from
deduplication alone says the authored expression graph was massively redundant —
which is what happens when a DSL is pleasant enough that content authors compose
freely, and is an argument *for* the DSL rather than against it, provided somebody
eventually writes the optimiser. Second, note which number is which: the 18.35 →
2.83 ms is **compile** time per chunk, not generation time. A terrain language
that is compiled per surface rather than interpreted per tile is the only way this
is affordable at all, and Wube got there in two steps — expressions first (0.17),
compiler later (2.0).

### 5.5 Cliffs: a constraint solve so that a finite sprite set always fits

**[WUBE] FFF-219, "Cliffs"** (0.16). Cliffs mark elevation contours; the generator
finds steep slopes in the elevation field and adds "an additional noise layer
called 'cliffiness'". Then the part that matters:

> builds up map of cliffs for an entire chunk at a time, and then, cell by cell,
> removes edges marked as cliff-crossing until no cell has more than 2
> 'cliff-crossing' edges

and having guaranteed that, it "select[s] an appropriate cliff segment to put in
the cell based on which edges crossed the cliff elevation upwards or downwards."

**[inferred] This is the whole technique, and it generalises past cliffs.** The
art department can only author a finite alphabet of cliff pieces — one per legal
edge-crossing configuration. Rather than authoring for every configuration the
noise can produce (combinatorially hopeless), or picking a wrong-but-close sprite
at draw time (visibly broken), the *generator is constrained to only emit
configurations the alphabet covers*. **Make the data unable to express what the
art cannot draw.** That is a much stronger guarantee than a fallback sprite, and
it is available to any tile game with a transition alphabet — this one included.

Stated purpose, and it is a design statement worth keeping: cliffs exist "to break
the flatness of the Factorio surface, without having to change the mechanics of
the game."

**[WUBE] FFF-401, "New terrain, new planet"** decoupled cliffs from water for 2.0:
"cliffs can go in directions completely independent of the planet's water", driven
by their own noise expression, and then "the cliff elevation is added to the water
elevation. This creates some nice terrain features. River-like sections that lead
to canyons, peninsulas guarded by cliffs, and some nice new islands."

The same post has the trick I would steal first from Factorio's map generation.
**Natural paths are authored as noise and *subtracted*:** ribbon/path noise
("canyon areas, land bridge ribbons, forest trails") is "subtracted from both
cliff and tree placement". **[inferred]** So walkable routes through obstacle
fields are generated by removing obstacles along a coherent curve, rather than by
pathfinding over the generated result and clearing it — one more noise term
against a post-process that would have to run after everything else and would
break the pure-function property in §5.2.

Decorative and tree placement were re-spread over four axes in the same pass —
"moisture, temperature, aux/terrain type, and noise layer" — producing "many
subregions with different mixes of plants and more clustering", with dry areas
losing tree density and "the absolute zero-moisture desert now doesn't have any
trees at all."

### 5.6 The Space Age planets are the same machine with different parameters

Worth recording because it is the test of whether the generation architecture was
actually general.

- **Vulcanus** (**[WUBE]** FFF-386): three authored zones — volcanic mountains,
  ashlands, basalt basins with "vast lava lakes and rivers" in "a labyrinthine
  network". The starting area is explicitly constructed from "3 distorted circles
  representing a mountain, basalt basin, and an ashland plateau" using "basic
  maths and trigonometry" — the §5.3 starting-area rule again, drawn by hand in
  the expression language. The lava got a dedicated shader, and the workflow is
  the notable bit: "Fearghall made a prototype lava shader outside of the game and
  got something looking good using a multitude of texture layers and distortion" —
  prototyped by an artist, outside the engine, before implementation. Cliff
  readability on near-black basalt was fixed in the *art*, not the lighting: "a
  large fake shadow below the north-facing cliff and a top edge highlight".
- **Fulgora** (**[WUBE]** FFF-398/399): plateaus as islands in an ocean of oily
  sand. The generator guarantees a gameplay property — islands "are usually
  separated by a gap that's larger than (basic quality) big electric poles" — so
  *isolated power networks* are a terrain-generation invariant rather than a rule
  in the electricity code. **[inferred]** That is the cleanest example in the note
  of gameplay specified as a constraint on the generator.
- **Gleba** (**[WUBE]** FFF-413): built with "the terrain generation noise and
  autoplace algorithms, experiment with proper LUT's, and adding some new shader"
  — confirming the stack is shared and per-planet identity is colour LUTs, shaders
  and autoplace parameters.
- **Aquilo** (**[WUBE]** FFF-432): "This planet has no land. It's an ocean world
  of liquid ammonia... at least 200km deep." Buildable ground is floating ice that
  the *player extends* by freezing more of it, and heated buildings melt it back.
  **[inferred]** Player-editable terrain, arriving in year twelve, on a tile
  engine whose tiles were never meant to be player-created outside of landfill.

---

## 6. Terrain rendering: how macro and micro detail coexist

This is the part of Factorio that looks most expensive and is most carefully
arranged not to be. The question — *why does the ground read as detailed when you
are nose-down on a single tile, and also read as varied when you are looking at
four hundred tiles at once, without either revealing a grid* — has four separate
answers stacked on top of each other, plus a caching scheme that stops the whole
thing being recomputed.

### 6.1 The stated problem

**[WUBE] FFF-150, "New Terrain Experiments"** names it exactly:

> A certain solution might look good when zoomed in, but it reveals repeating
> patterns when zoomed out

and calls terrain design "a permanent search for the best compromise among
different zoom levels". The knobs they tune together are "tiles, doodads (plants,
rocks), tile patches (decals, holes, rigs, etc.)", coordinating "probabilities of
tiles with different sizes, doodads and tile patches distribution, sizes of
biomes."

**[inferred]** That list is the answer in miniature: **the anti-repetition budget
is spread across four independent frequency bands**, and no single one of them is
carrying the load. This is the 2D form of the five-layer ladder in
[`terrain_rendering.md`](../../topics/world/terrain_rendering.md) §5 — macro
variation, multi-scale, distance tiers, texture bombing, histogram-preserving
blending — arrived at for the same reason and by the same argument.

### 6.2 Multi-size tiles, to destroy the grid

**[WUBE]** From 0.12 onward, "instead of having only variations of 32px tiles, a
tileset was produced with different sizes (x32, x64, x128, x256) in order to break
the grid sense and render more detail". 0.16 added **decals** — "ground-related
doodads meant to generate terrain accidents and details without being oppressed by
tileability and size."

**The shipped data gives the exact shape of it.** **[SHIPPED-DATA]** A standard
tile's `variants` are built by `tile_variations_template`, which emits **16
variants each at size 1, size 2 and size 4 — 48 per tile**, rising to **64** where
`max_size = 8` is declared (`sand-1` does). Each size band carries an independent
`probability` and a **16-entry weight table**, and `grass-1`'s reads:

```lua
[1] = { weights = {0.085, 0.085, ..., 0.005, 0.025, 0.045, 0.045 } },
[2] = { probability = 0.91, weights = {0.150, 0.150, ..., 0.010, 0.025 }, },
[4] = { probability = 0.91, weights = {0.100, 0.80, 0.80, ..., 0.01 }, },
```

and the sheets on disk reconcile exactly — `grass-1.png` is **4096 × 576** with
the size-1/2/4 bands at y = 0/128/320, and `sand-1.png` is **4096 × 1664** with
the extra size-8 band at y = 640.

**[inferred]** So a 4×4-tile patch of grass can be filled by one size-4 sprite
chosen from sixteen, or four size-2 sprites, or sixteen size-1 sprites, biased
toward the larger by `probability` — and the weight tables let an artist make a
distinctive variant *rare* (note the `0.005` entries) so a memorable feature does
not visibly repeat. **This is texture bombing implemented entirely in data**: no
noise lookup, no shader, no runtime cost beyond a weighted pick at chunk
generation.

**[inferred]** The insight is that a 32 px tile variation set can only ever produce
detail with a 32 px period, and the eye finds a 32 px period instantly at any zoom
where several are on screen. Adding 64/128/256 px variants means the *dominant
spatial frequency of the noise is no longer the grid frequency*, and the decal
layer — explicitly freed from tileability — supplies detail with no period at all.
Anti-tiling in 3D chases the same property with texture bombing and stochastic
blending; in 2D with a fixed camera you can simply author it.

### 6.3 The layer stack, and masks shared across materials

**[WUBE] FFF-214, "Concrete rendering"** describes the architecture the tile
renderer converged on:

1. a **material layer** — a large seamless texture laid out "in a regular grid
   across the world", world-anchored so it never swims relative to the ground;
2. a **detail layer** — tile sprites of several footprint sizes (1×1, 2×2, 4×4)
   scattered on top;
3. a **transition layer** — borders between terrain types, resolved at runtime;
4. **mask-based blending**, where "the alpha mask is a greyscale or single channel
   image which is multiplied with the actual texture, so that a pixel in the
   texture becomes more transparent the darker the corresponding pixel in the
   mask is."

And then the sentence that carries the whole design:

> masks are shared by multiple terrains, so we will save some video memory. This
> will be big deal when we do transition from any tile type to water.

**[inferred] That is the combinatorial fix, and it is the most transferable idea
in this section.** A transition between terrain A and terrain B is factored into
*shape* (which is a property of the boundary geometry, shared by every pair) and
*material* (which is a property of A and B). Authoring per-pair transitions is
O(N²) art for N terrain types and is why tile games historically ship four ground
types; authoring a shared mask alphabet and compositing at runtime is O(N) art and
O(1) VRAM per additional pair. Wube pay for it with a runtime composite, and §6.5
is how they stop paying for it every frame.

**The shipped transition sheets confirm the factoring and give its size.**
**[SHIPPED-DATA]** A transition set is described by a named layout —
`transition_16_16_16_4_8`, whose name *is* its counts — giving **16 inner corners
+ 16 outer corners + 16 sides + 4 U-transitions + 8 O-transitions = 60 sprite
variants** for one terrain-pair transition. And each of those exists in **three
parallel blocks packed side by side in the same PNG**, selected by x-offset:

```lua
  overlay    = { x_offset = 0    },
  background = { x_offset = 1088 },
  mask       = { x_offset = 2176 }
```

`water-transitions/grass.png` is **3200 × 2368**, which reconciles exactly
(2176 + 1024 = 3200). Read visually, the sheet is precisely that: a coloured
shoreline overlay on the left, the darker water-side background in the middle, and
a **pure black-and-white binary mask** on the right — five horizontal bands for
the five piece kinds, sixteen variants across each.

There is also a **second-order system**: `transitions_between_transitions` ships
dedicated art for the three-way junction where a grass→water transition meets a
grass→out-of-map one. **[inferred]** That is the case naive marching-squares tile
blending always gets wrong, and Wube's answer was to author it rather than to
approximate it — consistent with §5.5's principle of never emitting a
configuration the art cannot represent.

The same post notes multi-pass resolution for special cases: for hazard concrete,
"first we resolve transitions as if all hazard concrete was regular concrete, then
we render transitions just for the hazard concrete" — a second pass over a
relabelled world rather than a widened rule set.

And the artist-facing justification, which is the real reason the system exists:
masks "freed" artists "from having to create 32 by 32 pixel sprites that need to
tile with each other."

### 6.4 The transitions themselves: an eight-year bug, and its resolution

This is a good story about a representation choice that was wrong and expensive to
change, so it is worth the space.

**[WUBE] FFF-199, "The story of tile transitions"** states the original sin:

> the transition is on the tile of the grass. This means that the whole transition
> is considered to be a grass tile, which leads to weird looking things.

Moving the transition onto the *water* tile — the correct choice — required an
alphabet the art did not have: "we would need all the other shapes like 'O', 'U',
'L', and mainly the graphics of the transition suddenly wouldn't have the space of
the whole tile, as a possible transition to another terrain can be on the other
side." The options weighed were smaller transitions, two or three layered
transitions for combinatorial coverage, or *tile correction* — logic forbidding
configurations the art cannot represent.

They shipped tile correction, and lived with it for years. **[WUBE] FFF-346**
records the fix: "the ground draws shore transitions over water tile now, and
shores are not limited to grass terrain any more" — previously "the transition
graphics was tileable only with grass terrain". The payoff is stated in gameplay
terms: tile correction became "mostly obsolete, but it is still used to enforce
some soft tile placement rules during map generation", and the map generator could
finally produce "newly possible 1 tile wide creeks".

**[inferred]** Note the shape of that arc, because it recurs: a transition system
whose *shape alphabet was incomplete* forced a **correction pass on the world
data** to keep the world inside what the renderer could draw — and the correction
pass then constrained map generation for years. Compare §5.5, where the cliff
generator constrains itself to the alphabet by construction and needs no
correction at all. *Same problem, two solutions, and the one that constrains the
producer is much cheaper than the one that repairs the product.*

**[WUBE] FFF-344** adds the consequence that makes it more than cosmetic:
transitions gained "an additional layer of collision checks, which considers the
transitions when performing the logic of what can go where". So the drawn
shoreline and the collidable shoreline are the same shoreline — the character
walks around the curve you can see rather than the tile square underneath it.
**[inferred]** This is the counter-case to the rule that
[`dcs_clouds.md`](../flight/dcs/dcs_clouds.md) §11 and
[`ruse.md`](ruse.md) §5 keep landing on — *render geometry and query geometry are
different assets*. Here they were deliberately unified, and it worked, because a
tile transition is a small, static, analytically simple shape. The rule survives
with its condition attached: unify when the render representation is cheap to
query, split when it is a raymarched volume.

### 6.5 The scrolling cache: composed terrain as a torus

Composition per frame would be far too expensive. **[WUBE] FFF-333, "Terrain
scrolling"** is unusually blunt about it:

> Factorio's terrain rendering is insane due to its complicated tile transition
> rules, and re-rendering it every frame is just not fast enough.

So terrain is composed into a persistent render target and reused. The *old*
scheme already did this — keep last frame's composed terrain, "render the texture
shifted to the new position, fill up the gap, and then copy the final result back
into the texture for reuse in next frame" — but that "would result in rasterizing
2 screens worth of pixels" every frame the camera moved.

The measured cost of that, and it is the clearest CPU/GPU-class datum in the note:

| GPU | Resolution | Stationary | Moving |
|---|---|---|---|
| Intel HD Graphics 3000 (iGPU) | 1600×900 | ~2 ms | up to ~5 ms |
| GeForce GTX 750 Ti (dGPU) | 1080p | <0.5 ms | <0.5 ms |

**[WUBE]** ~5 ms is "nearly 1/3rd of a frame time (16.66 ms)", and the reason the
iGPU suffers so much more is "an order of magnitude lower memory bandwidth" —
this pass is pure fill rate and touches no geometry.

The fix (0.18.4) is to treat the cache texture as a **torus**: "we can just adjust
the offset and update the parts that changed. So, the number of pixels copied is
proportional to how much the terrain scrolled." **[inferred]** A wrap-around
scrolling buffer, so panning re-composites only the newly exposed L-shaped strip
instead of a full screen, and the cost becomes proportional to camera *velocity*
rather than to screen area. Standard for terrain clipmaps in 3D; the point is that
it applies just as well to a 2D tile compositor, and that Wube shipped eight years
without it because on a discrete GPU the naive version is free.

Two further caches sit above it:

- **[WUBE] FFF-323**: "we cache draw orders per-chunk, and we only run prepare
  render on chunks that have just entered the player view", and — the detail that
  matters for §4 — "changing render scale doesn't need to invalidate the new
  cache, so zooming doesn't cause prepare render to run."
- **[WUBE] FFF-358**: "A flag was added to cached per-chunk tile draw orders to
  determine if the chunk contains a dynamic effect", so a chunk with no water pays
  nothing for the animated-water path.

**[inferred]** That flag is CLAUDE.md's derived-cache escape hatch exactly: the
fast path may only skip work that provably does nothing, and the bit that says
"this chunk is complicated" is computed once at cache-build time rather than
tested per frame.

### 6.6 Animated water for free

**[WUBE] FFF-323, "Animated water"**. Before: "Water in Factorio is static. It has
foam, which is static. There are also some static reflections."

The implementation is a good example of choosing a technique so that it lands on
the right side of the cache above. The shader reuses the existing water sprite *as
a noise texture*, and UVs are world-space — "I simply used UV coordinates to
represent the game world position". The consequence is the design:

> the water animation depends only on global time, and the vertex data of water
> tiles doesn't change in between the frames

**[inferred]** So animation costs **zero CPU re-preparation** and does not
invalidate the per-chunk draw-order cache — it is a pure function of
`(world position, time)` evaluated in the fragment shader, which is the only kind
of animation compatible with a system whose whole performance strategy is *not
rebuilding draw lists*. A vertex-animated or CPU-scrolled water would have
defeated §6.5 entirely.

Effect data is packed into an auxiliary render target — "Red for reflections,
green for transparency, and blue for foam" — one channel per effect, sampled by
the water shader.

---

## 7. The simulation

This is the half of Factorio that the game is actually about, and the half where
the published record is strongest. Wube state their own method in one word.

### 7.1 "Do less"

**[WUBE] FFF-148, "Optimizations for 0.14"** contains the design philosophy, and
it is CLAUDE.md's first rule in Wube's words: because the game has no build limits
and actively encourages expansion, even the best computer eventually fails to
sustain 60 updates per second, so the main method for addressing slowdown is
**"do less"**.

The diagnosis in the same post names the pattern every system below attacks:
"each belt and each pipe is updated individually piece by piece. That means the
cost to run those increases as the count of entities does" — combined with the
observation that most belts are **long simple runs** with no intersections,
splitters or inserters.

**[inferred] Every major optimisation in Factorio's history is one of four moves**,
and it is worth having the list before the details:

1. **Collapse a chain of per-element simulation into one segment object** updated
   once — belts, fluids, rails.
2. **Store deltas rather than absolute values**, so the common case moves two
   integers instead of N.
3. **Sleep by push, not poll** — an entity with nothing to do deregisters and asks
   the things that could change to wake it.
4. **Aggregate a whole network** and update it as a sum — electricity, fluid
   segments.

All four are "do less work" in CLAUDE.md's sense — they change *what work exists*,
not how fast it runs — and all four are one to two orders of magnitude, exactly as
that rule predicts.

### 7.2 Belts: the famous one

**[WUBE] FFF-176, "Belts optimization for 0.15"** is the delivery of FFF-148's
plan, and it is the single best-documented optimisation in the game. The stated
goal is a gameplay one: players "would never have to build underground belts for
performance reasons."

**Items are stored as the distances between them, not as positions.** Verbatim:
"we no longer store absolute coordinates of items, instead we store the distance
between items." A transport line holds two boundary gaps (to the segment's start
and end) plus the inter-item gaps.

The consequence for a flowing belt is the whole win: under normal flow only the
terminal gaps change, so the update is **"incrementing/decrementing two integers
instead of incrementing the position of all 200 items on those belts."**

**The blocked case gets a monotone cursor.** When the line is blocked, the system
decrements the **last non-zero gap**, and the index of that gap is cached and
"can never increase, only decrease" — giving **amortised constant time** for
compression. The invariant that makes this legal is a gameplay fact, not a
programming one: **"whenever a belt compresses - it will stay that way forever."**

**[inferred] That is the part worth studying.** The data structure is fast because
a *domain* invariant was identified and encoded — belts only ever compress, never
spontaneously decompress from the middle — so a cursor that only moves one way is
sufficient. Find the monotone quantity, and an O(n) scan per tick becomes O(1)
amortised. The general lesson is that the cheapest algorithms in this game came
from noticing what the *rules of the game* forbid, not from better data layout.

Merging rules and interfaces:

- Lines adjacent to inserters keep to roughly **9-tile** limits with dynamic
  merge/unmerge; inserter-free lines extend to about **100 tiles**.
- **Splitters are the primary entity that cuts transport lines apart** — they are
  the topological boundaries of the segment graph.
- Inserters hold a **cursor into the gap list** rather than re-deriving position,
  giving O(1) position updates with no binary search.
- **Rendering consumes the simulation representation directly** — item positions
  are reconstructed by walking the gap list from the first item's position. There
  is no parallel array of drawable positions.

**[inferred]** That last point is the discipline that makes the optimisation
survivable. A second, render-side copy of item positions would have to be kept in
step with a structure whose entire design is that it does not update per item —
and would have reintroduced exactly the O(items) cost the change removed.

The numbers:

| Measure | Result |
|---|---|
| Item movement itself | **50–100× faster** |
| Belt system overall | **5–10×** |
| Real saves | **25 UPS → 35–40 UPS** |

### 7.3 Sleeping, and why it must be ordered

**[WUBE]** The sleep/wake mechanism dates to 0.9: an inserter with nothing to do
goes inactive and **tells the chest and the belt to wake it** when something is
removed or arrives. Push-based activation, not polling.

**[COMMUNITY, Rseding91]** Modern versions moved the active set from per-chunk to
per-map registration — "Entities are no longer registered per-chunk when they are
active but instead per-map". **[inferred]** So there are two distinct structures
doing two distinct jobs: a **per-chunk spatial index** for queries, and a
**global active list** for updating. Conflating them is the mistake; a system that
needs to answer "what is near here" and "what needs a tick" with one structure ends
up doing both badly.

And then the constraint that makes this harder than it looks: **the wake order is
part of the deterministic game state.** **[WUBE] FFF-364** — "Transport lines
cannot simply wakeup that inserter, because it could be woken up by another
thread, and activation order is important as it also defines the order in which
Inserters will be updated." §8.2 is how they solved that.

### 7.4 Electric networks: cost scales with network *count*

**[WUBE] FFF-209, "Optimisation is a Way of Life"**. The electric network's
connector data was reorganised so that it is **categorised by prototype**, letting
prototype-constant values (`inputFlowLimit`, `outputFlowLimit`) be "accessed once
at the beginning of the processing of one category" rather than per entity.
Result: "electric network transfers are more than twice as fast compared to 0.15",
contributing to a large save running **2.4× faster in 0.16 than 0.15**.

The player-visible consequence is stated in Wube's own performance guide:
**[WUBE]** the electric network cost is "particularly affected by the number of
different electric networks, not their size", with the diagnosis that "someone has
built a boat-load of isolated networks instead of one big network, for example: 10
solar panels and a radar, copy-pasted around."

**[inferred]** So a network is an aggregate: supply and demand are summed once and
distributed, and the per-machine work is a small contribution to a sum rather than
an interaction. Ten thousand machines on one network is one pass; ten thousand
networks of one machine is ten thousand passes with fixed overhead each. **The
cost model is the opposite of the intuitive one, and it is the reason Fulgora's
island-separated power (§5.6) is a real performance consideration rather than only
a puzzle.**

**[COMMUNITY, Rseding91]** The price of the aggregate representation was paid in
flexibility: per-entity electric flow fields are no longer adjustable at runtime.
**[inferred]** Which is the standard trade — an aggregated system stops being able
to answer per-member questions, and that has to be acceptable before you aggregate.

The same post has a nice small instance of the layout rule: smoke became
`TrivialSmoke` objects at **32 bytes instead of 256**, stored "in continuous
memory on chunks", updated at guaranteed **120-tick** intervals.

### 7.5 Fluids: the rewrite that took six years to be allowed

The fluid story is the most honest engineering narrative in the Friday Facts, and
it is a good case study in *why a known-bad system survives*.

**[WUBE] FFF-260, "New fluid system"** (2018) is the diagnosis. The old model had
fluid boxes "try to equalise with their neighbours", computed per box, per
connection, every tick, and the failure list is damning:

1. junctions "behave in a very random fashion", so recipients that obviously
   should get fluid do not;
2. **order dependence** — "fluids moving faster or slower depending on the entity
   update order";
3. it never settles — "you rarely have a tank that is entirely full or entirely
   empty";
4. cost — "even though the formulas are simple, they are calculated for every
   connection in every fluidbox, which adds up";
5. throughput degrades with distance.

The **rejected alternatives** are the valuable part, because they price the
options: pure memory/layout optimisation was worth only **30–50%** and fixed none
of the behaviour; max-flow algorithms (Ford-Fulkerson) gave "ridiculous" junction
behaviour; a full electric-network analogy required unlimited throughput or
prohibitive complexity; fluid-box merging alone did not address splitting,
ordering or direction.

**[WUBE] FFF-416, "Fluids 2.0"** (2024) is the rewrite, and what finally forced it
was Space Age playtesting bringing the old algorithm "to its knees" — with the
stopgap (larger pipe volumes) creating buffers so large it "made storage tanks
pointless". The new model:

- pipes, underground pipes and storage tanks **merge into fluid segments**;
- each segment holds **a single fluid type** and inherits its volume;
- **"Fluid pushed to a segment will be immediately available at any point along a
  segment"** — no propagation, no flow, no distance term;
- machines push at unlimited rates, and **pull rates scale with segment fill
  level** — "if a segment is half full, then the pulling rate is half of the
  maximum".

They are explicit that this is "a significant step back in realism", accepted
because it gives players the behaviour they expect.

**[WUBE] FFF-430, "Drowning in Fluids"** is the correction, and it is the
interesting half. Uncapped segments let players build "ridiculously huge
pipelines" across continents, so a **250×250 tile pipeline extent limit** was
added — with the stated design constraint that "the limitation needs to be
extremely easy, obvious, and interactive". Output rate became inversely
proportional to the sink's fullness, with a hardcoded cap of **100 fluid per
operation → 6000/s**, applied proportionally on both source and sink fullness.
Balance fell out of it: pumps nerfed **10× to 1200/s**, fluid wagons doubled to
50,000, and **1 water → 10 steam** in boilers to cut water throughput without
changing power output.

**[inferred] Read FFF-148→176 (belts) and FFF-260→416 (fluids) together and it is
the same move twice, six years apart** — collapse the chain into a segment, update
it once, re-derive per-element appearance for rendering. Belts took one version
and fluids took six years, and the difference is entirely that **the belt change
was invisible to the player and the fluid change was not.** Belts kept their
observable behaviour; fluids had to give up a physical model players had built
intuitions around, so the engineering was ready years before the decision was.
*An optimisation that changes observable behaviour is a design change, and it
moves at design speed.*

The FFF-430 postscript is its own lesson: **removing a limit created a new
problem, and the fix was to add a different, legible limit.** The old system's
distance falloff was accidental and physical; the new one's is explicit and
authored. Players need *some* constraint on pipeline reach for the building to
stay interesting, and the accidental one had been carrying that load invisibly.

### 7.6 Trains: pathfinding on the signal graph, with a penalty table worth stealing

**[WUBE]** Train pathfinding is A* **over rail segments**, where "a segment is an
uninterrupted plain sequence of rails, with no intersections, stops, or signals" —
so the search graph is the junction/signal graph, orders of magnitude smaller than
the rail tile count. The search proceeds from **both ends of the train
simultaneously**.

The published penalty table is the most directly reusable piece of data in this
note:

| Condition | Penalty |
|---|---|
| Base cost | segment length |
| Occupied rail block | **2 × length ÷ number of blocks from start** |
| Rail signal at red | +1000 |
| Train stop | +2000 |
| Train stopped at a station | +500 |
| Stopped train with no valid schedule | +1000 |
| Manually controlled stopped train **with** passenger | +2000 |
| Manually controlled stopped train **without** passenger | +7000 |
| Automatic train without schedule | +7000 |
| Train arriving at stop/signal | +100 |
| Train waiting at a signal | +100 **+ 0.1 per tick waited** |
| Train with no path | +1000 |

**[inferred] Two of these rows are doing something subtle and both generalise.**

The **occupied-block penalty decays with distance from the train** (`÷ blocks from
start`). Congestion right in front of you matters; congestion twenty blocks ahead
barely does, because it will probably have cleared by the time you arrive. That is
a closed-form model of *the reliability of your own information decaying with
prediction horizon*, expressed as one division — and any system that plans over a
world that changes while the plan executes wants it.

The **waiting-train penalty accumulates at 0.1 per tick**. A train blocked for ten
seconds becomes 60 units more expensive to route through than one that just
arrived. So **traffic re-routes around a jam with no jam detection anywhere** —
the cost of a route through a stuck train rises continuously until alternatives
win. *A monotonically increasing penalty on "this has not moved" is a deadlock
detector that needs no deadlock detector*, and it degrades gracefully where a
threshold would flap.

Note also that both are penalties on the *heuristic-visible* cost rather than
special-case logic, so they compose with everything else automatically.

**Repathing triggers** are enumerated: manual control changes, signal or station
changes, failed path revalidation, a destination becoming (in)accessible, and
failure to reserve a needed signal. **[inferred]** That is an explicit invalidation
list — the same discipline as CLAUDE.md's "invalidate at the boundary that owns
the data", applied to a plan rather than a cache.

### 7.7 Rails 2.0: a geometry constraint that set a gameplay number

**[WUBE] FFF-377, "New new rails"**. Curved rails were replaced by **half-curve
pieces** that compose into S-bends of any length, plus half-diagonal segments, and
the **curve radius went from 9 to 13 tiles**.

The old representation's specific sins are listed, and one is a genuine engine
smell: **curved rails were the only entities in the game with multiple bounding
boxes.** **[COMMUNITY, Rseding91]** corroborates from the other side — the game
has exactly one entity with multiple collision boxes, "it already causes a massive
amount of problems", and they will never add another. **[inferred]** A single
special case in the collision system, tolerated for a decade because it was only
one entity, and eventually a first-order motivation for rewriting the whole rail
representation. *The cost of a special case is not its code; it is that every
system touching that code must handle it forever.*

Why 13 and not 10 or 12: **[WUBE]** "every connection point has to be located on
integer grid coordinates." Smaller radii put control points off-grid and produced
jagged joins; at 13, "the edge pieces are a section of a perfect circle, and the
middle control point is on integer grid coordinates."

**[inferred] A gameplay-visible constant — how tight your railways can turn, which
every player's blueprints are built around — was set by the requirement that a
curve's endpoints land on integers.** That is a very pure example of a
representation choice propagating all the way into design, and it is the sort of
number that looks arbitrary from outside and is completely determined from inside.

**[WUBE] FFF-378** notes the refactor is what made elevated rails possible at all
— "we can now define any kind of rail shape" — with elevated rails using "exactly
the same rail shapes as the new ground rails do". The internal representation of
the elevation layer was **not established** (§12).

### 7.8 Biter pathfinding: use the abstraction as a heuristic, not as the answer

**[WUBE] FFF-317, "New pathfinding algorithm"** replaced plain A* with
straight-line distance, whose named failure case is precise: **artillery aggroing
distant biters around a large lake**, where the heuristic drives the search into
the shore and it floods.

The replacement is two-tier:

1. **A chunk-based abstraction.** Each 32×32 chunk is decomposed into
   **components** — "a component is an area of tiles where a unit can go from any
   tile within the component to any other within the same component" — and
   crucially **only perimeter tile data is cached**, because "what we really care
   about is what other components (in neighbouring chunks) each component is
   connected to."
2. **A dual pathfinder.** The **base** pathfinder does the real routing over
   tiles; an **abstract** pathfinder over the component graph supplies only the
   **heuristic**.

The abstract search is **Reverse Resumable A\***: it searches backwards from the
goal and *keeps its nodes*, so "the next time the base pathfinder creates a new
node and needs to know its distance estimate, we simply look at the abstract nodes
we kept from the previous search" — amortising the abstract search across the
whole base search and across repeated queries to the same goal.

**[inferred] The design decision worth taking is which job the abstraction was
given.** This is HPA\*-shaped, but they did *not* path on the abstract graph and
refine; the base search stays authoritative and the hierarchy only answers `h()`.
The consequence is the reason to prefer it: **a stale or approximate abstraction
can only make the search slower, never make the path wrong.** Hierarchical
refinement has to keep the abstraction correct or it produces invalid paths;
heuristic-only use degrades into plain A*. That is a materially safer failure mode,
and it is also why they could add it to existing code without touching the
pathfinder's guarantees — Wube note it "only replaces the heuristic function" and
needs "minimal recalculation when entities are placed or removed", since only the
edited chunk's components and their perimeter connectivity change.

This is directly relevant to
[`navigation.md`](../../topics/agents/navigation.md)'s framing and to this
project: it is the cheapest possible way to add hierarchy to an existing grid
search, and it cannot break correctness.

### 7.9 Where the time actually goes

**[WUBE]** The F4 debug menu's `show-time-usage` breakdown, with Wube's own
remediation advice, is effectively a published list of the game's subsystem
managers:

| Category | Wube's note |
|---|---|
| **Entity manager** | typically the largest; drill in with `show-entity-time-usage` |
| **Transport lines** | optimising belt UPS "needs advanced techniques" |
| **Fluid manager** | "reduce your amount of pipes… replace nuclear power with solar power" |
| **Heat manager** | "use less heat entities" |
| **Electric networks** | "particularly affected by the number of different electric networks, not their size" |
| **Circuit networks** | "use fewer combinators" |
| **Trains** | "use fewer but bigger trains" |
| **Chart update** | "reduce your number of radars" |
| **Script update** / **Lua garbage incremental** | per-mod cost and mod GC |

**[inferred] Read the shape of that list rather than the rows: it is one entry per
aggregated manager, and everything that was *not* aggregated falls into "Entity
manager".** The performance architecture of the game is legible directly from its
profiler categories — belts, fluids, heat, electricity, circuits and trains each
earned their own aggregate system, and everything else is still updated as
individual entities. That is also a good model for how to name profiler zones, and
it agrees with CLAUDE.md's rule that granularity is earned by cost.

`show-entity-time-usage` then splits Entity manager **by entity class, showing
both time and active count** — confirming a per-class active set, and giving
players the same view the developers use.

### 7.10 The 2.0 optimisation pass

**[WUBE] FFF-421, "Optimizations 2.0"** is a compact catalogue of the four moves
from §7.1 applied to systems that had escaped them:

| System | Method | Result |
|---|---|---|
| **Roboports** | sleep when idle; wake on charging or rare events — "most Roboports don't have anything to do except consume power" | **1 ms → 0.025 ms** per tick |
| **Radars / map revelation** | a **counter per chunk** by registration instead of per-radar loops, so "things can overlap as much as they want and it simply increases the counters" | **3.6%** of total frame time |
| **Lamp "always on"** | removes the circuit connection needed to switch lamps by daylight, eliminating control behaviours | **~1.2 ms** |
| **Belt readers + combinators** | one reader reads a whole belt; combinators multithreaded | **9.5%** on the playtest save; **131 s → 8.2 s (14.9×)** synthetic |
| **Electric network threading** | **abandoned** — memory-throughput limited | 0.5 → 0.39 ms but CPU 0.5% → 15% |
| **Worker robots** | movement-intention prediction: a robot commits to up to a **20-tick** movement cycle, and "rendering code can use this intention to pretend the robot is moving smoothly" | **15%** on the office save; 10–25% typical |

**[WUBE] FFF-415** adds the best complexity win of the set: construction robot
task checking went from testing a task against **36,815 roboports** to against
**~900 rectangle union areas** — O(N) → O(log N), "from expensive to essentially
free". **[COMMUNITY, Rseding91]** notes a quadtree was rejected here because "the
range isn't constrained for robots because the player moves around and roboports
come and go" — the dynamism defeats a hierarchical structure, so they used a flat,
rebuildable union of rectangles instead. **[inferred]** A good corrective to the
reflex that the answer to a spatial query is always a tree.

**[inferred] The robot entry is the general pattern behind several of these, and
it is worth naming: decide once, coast for N ticks, and let the renderer
interpolate.** It is the same shape as the belt offset (§7.2) and the crafting
machine's animation offset (a per-item animation vector replaced by a single
offset value, worth another 2% in FFF-204). The simulation gets to run at a
fraction of the visual rate precisely because the visual rate is a *presentation*
concern — which only works because the renderer is downstream of the simulation
and never feeds back into it.

---

## 8. Threading, and the memory wall

Factorio is the best published case study available on **why a simulation with
obvious task-level parallelism does not thread**, because Wube tried it, measured
it, wrote up the negative result, and then tried again seven years later and got
the same answer.

### 8.1 The negative result

**[WUBE] FFF-215, "Multithreading issues"** (2017). Kovarex parallelised three
semi-independent update tasks — **trains, electric networks and belts** — chosen
carefully: "neither trains or belts use electricity and belts don't interact with
trains." The tasks were genuinely independent. The result:

> the parallel version didn't speed things up, it was actually even slower.

The diagnosis is cache-line ownership, not bandwidth in the abstract. With shared
L3 and per-core L1/L2, when several cores touch the same memory pages **and
write**, "whenever something is changed, the other copies of the same page need to
be invalidated and updated. This means that the threads are invalidating each
others cache all the time."

And the control experiment is in the same post, which is what makes it convincing
rather than anecdotal: **the render prepare step scales fine across up to 8
threads** (§2.8), walking the entire game world — because it is **read-only**.
When cores only read, "each core has its own copy of the memory page" and nothing
is invalidated.

**[inferred] So the failure is not task granularity, not synchronisation
overhead, and not the size of the world walk — all three are identical between the
two cases. The only difference is writes.** That isolates the cause about as
cleanly as a real-world experiment can.

The unimplemented fix they identify is the right one and they say why it did not
happen: give each chunk or task **its own allocator**, so an entity's data is
physically grouped with its neighbours' and two threads working on different
chunks never share a page. Ruled out as "big changes in the code", infeasible
before 1.0.

**[inferred] That is the load-bearing lesson for this project.** The obstacle to
threading a simulation is usually not the logic, it is that **general-purpose
allocation interleaves unrelated objects on the same cache lines**, and no amount
of task decomposition fixes false sharing you did not create deliberately. If
threading is ever wanted, the allocator decision comes first and cannot be
retrofitted cheaply — which is the strongest possible argument for CLAUDE.md's
insistence that data layout is the expensive-to-change part.

### 8.2 What does thread, and the deterministic merge point

**[WUBE] FFF-364** (1.1) got belts threaded, and the technique is the interesting
part. The grouping insight: "We can group transport lines in a way, that lines
from one group don't interact with any line from any other group" — so groups
update on separate threads with no synchronisation.

But determinism forbids the obvious wake-up path (§7.3), because "it could be
woken up by another thread, and activation order is important as it also defines
the order in which Inserters will be updated". The solution:

**Worker threads never mutate shared state. They append wake-up *requests* to a
list. When all threads finish, the main thread collects and merges the requests
and wakes entities in a defined order.**

**[inferred] That is the general pattern for threading anything under a
determinism constraint, and it is worth stating as a rule: workers produce
*intentions*, a single-threaded merge step applies them in a deterministic
order.** The parallel phase is pure; the ordering is imposed by the serial phase.
It costs a barrier and a merge pass, and it makes the result independent of thread
scheduling — which is the whole requirement.

Results: transport belt update **4 ms → 1.6 ms**, and "between 20 to 40% overall
performance gain" on a megabase producing 10k science per minute.

### 8.3 The 2017 finding, re-confirmed in 2024

**[WUBE] FFF-421** tried to thread electric networks for 2.0 and **abandoned it**:
the system is "memory throughput limited", so threading bought 0.5 ms → 0.39 ms of
wall time while CPU usage went from **0.5% to 15%** — thirty times the CPU for
20% of the wall clock, and a net loss overall.

In the same post, **belt readers threaded successfully**, and Wube name the reason
in the same terms as 2017: "belt reader is mostly a read operation".

**[inferred] Two independent attempts, seven years apart, on different systems,
reaching the same rule: read-heavy passes thread, write-heavy passes do not.**
That is as close to a settled empirical finding as this directory contains, and it
is a much more useful decision procedure than "is this task independent" — because
in FFF-215 the tasks *were* independent and it still failed.

### 8.4 Memory is the ceiling, from three directions

- **[WUBE] FFF-204**: software prefetching — "before an entity is updated, the
  next entity is already requested so that it can be loaded in the background",
  with a tuned window of **−128 to +384 bytes (8 cache lines)** — bought
  **9–13%** on general saves but only **+5%** on belt-heavy ones. **[inferred]**
  Belts benefited least because FFF-176 had already made them cache-dense; there
  was no latency left to hide. Their measurement methodology is worth copying:
  update times averaged over **3,600 ticks**, boxplotted over **20 repeated runs**.
- **[WUBE] FFF-209**: the electric network's 2× came from reorganising data by
  prototype; smoke went from 256 to **32 bytes**. Layout, not algorithms.
- **[COMMUNITY]** The hardware data agrees from outside: UPS tracks memory
  performance, RAM CAS latency measurably matters, and benchmark leaderboards are
  dominated by AMD 3D V-Cache parts. One reported pair on the same 10k-belt
  megabase — a Ryzen 7 3700X at **84 UPS** against an i5-9600KF at **97 UPS** —
  has the slower-core part losing, which is what a memory-subsystem-bound workload
  looks like from the outside.

### 8.5 The cost of threading under determinism

**[WUBE] FFF-415** records a determinism bug caused by threaded chunk generation
that required four conditions to coincide (mods, chunk generation, and **differing
CPU core counts between the two machines**), had been present **since July 2017**,
and was only fixed for 2.0.

**[inferred] Seven years latent, and undetectable on any single machine.** That is
the real price of §8.2's discipline being violated anywhere: a threading bug in a
deterministic simulation does not present as a race or a crash, it presents as two
players disconnecting occasionally, years later, under a condition nobody thought
to vary. It is the strongest argument for the merge-point pattern being a rule
rather than a preference.

---

## 9. Multiplayer: everyone computes everything

Factorio's multiplayer is **deterministic lockstep**, taken further than any other
game in this directory. It is also the clearest available worked example of what
determinism actually costs in practice, because Wube have written up their desync
post-mortems in public for a decade.

### 9.1 The model

**[WUBE] FFF-302** states it in one paragraph, and it is worth having verbatim
because every other decision follows from it:

> All clients simulate the game state and they only receive and send the player
> input (called Input Actions). The server's main responsibility is proxying Input
> Actions and making sure all clients execute the same actions in the same tick.

So the server is an **orderer and a relay**. It runs the simulation too, but its
authority is over *the order of actions within a tick*, not over the resulting
state. **[WUBE] FFF-30** (2014, before multiplayer shipped) had already committed
to it — "all multiplayer peers will calculate the simulation themselves and only
the player input (we call it input actions) will be exchanged over the network" —
with deterministic simulation described as "an absolute must".

The reason is bandwidth *independence*, and **[WUBE] FFF-76** puts the scaling
property plainly: the advantage is "the low amount of data sent over the network",
and "You can play the game just the same no matter if it has hundred objects or
million."

**[inferred] That is the whole argument, and it is why no other model was
available to this game.** A state-replicating architecture sends deltas
proportional to *how much of the world changed*, and in Factorio essentially all
of the world changes every tick — a megabase is millions of moving items by
construction. Lockstep is the only model whose bandwidth is proportional to
*player intent* rather than to world activity, and Factorio has the largest ratio
between those two quantities of any game here. Compare
[`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md) §10, where Eugen's
deterministic lockstep is contrasted with per-entity replication and lag
compensation: the deciding question is the same one, and Factorio sits at the far
end of it.

The cost, also from **[WUBE] FFF-76**: "with lock step architecture you never
directly see the other player's game state, there is no way to correct for these
small errors". There is no reconciliation. A divergence of one bit is a desync,
and the game can only stop.

**The topology changed, the model did not.** The original implementation was pure
peer-to-peer. **[WUBE] FFF-147, "Multiplayer rewrite"** gives the reason it died,
and it is infrastructural rather than architectural: "Everyone needs to be actually
sending packets to everyone else, which isn't that easy in the current world, where
IPv6 isn't everywhere, and public IPv4 address is becoming quite a luxury." The
client-server move took packet count "from O(n^2) to O(n)", let clients stop
knowing about each other at all, and — the operationally important part —
"The server can safely omit a player from the package if he has a lag spike, so
the lag spike is isolated from the rest of the game." Under P2P, by contrast,
**[WUBE] FFF-76**: "the game speed is limited by the slowest player."

**[WUBE] FFF-149** adds the security dimension, which is the sort of thing pure
P2P designs discover late: the old scheme let a spoofed-IP connection attempt make
peers "flood the target with 700 packets by sending a single packet to the server".
Fixed with client-generated IDs and server verification. Kovarex: "added+removed
more than 20 000 lines of code."

### 9.2 What determinism actually costs

The folklore is that determinism means no floating point. **That is not what Wube
did**, and their own account is more useful than the folklore.

**Floats are used, and mostly behaved.** **[WUBE] FFF-52**: "Originally we were
quite afraid of Floating point operations discrepancies across different
computers. But surprisingly, this hasn't been a big problem so far (knocking on
the table)." **[inferred]** IEEE 754 basic arithmetic is exactly specified, so
`+ - * / sqrt` agree across compilers and vendors provided nobody enables
fast-math, changes x87 precision or lets the optimiser contract into an FMA. The
hazards are elsewhere, and they found all of them.

**Positions, however, are fixed point.** **[WUBE]** From the official API docs
([`MapPosition`](https://lua-api.factorio.com/latest/concepts/MapPosition.html)):
"The coordinates are saved as a fixed-size 32 bit integer, with 8 bits reserved
for decimal precision, meaning the smallest value step is 1/2^8 = 0.00390625
tiles." **[inferred]** 1/256-tile fixed point removes the one place where float
error would have been fatal *and* structurally unfair — precision degrading with
distance from the origin, in a game explicitly about travelling a million tiles
from the origin. It also makes §5.1's world bound a consequence rather than a
choice.

The three real desync sources, all found the hard way:

- **Transcendental functions.** **[WUBE] FFF-36**: `sin`/`cos`/`asin`/`atan` "are
  not guaranteed to give same results across different platforms", and "when we
  tried to load the replay on a machine with different operating system it
  desynchronized immediately." Kovarex wrote "custom implementations of these that
  give the same results on Win, OSX and Linux". **[inferred]** This is the correct
  and standard finding: libm is not specified to the last bit, so a deterministic
  simulation must own its transcendentals.
- **Number formatting.** **[WUBE] FFF-108**: "some of the standard libraries (gcc)
  print the number 0.00001 using format '%g' as '1e-05' and some other ones
  (current visual studio 2013) print the same number as '1e-005'". Fixed with a
  custom formatter. **[inferred]** It matters because Lua mod code does string
  operations on numbers *inside* simulated state, so a printing difference becomes
  a state difference.
- **Unspecified ordering in the standard library.** **[WUBE] FFF-52**: an
  "ambiguous sort comparator (c++ stl sort across different compilers obviously
  behaves differently for elements that are equal to each other)" desynced map
  generation. **[inferred]** The classic non-strict-weak-ordering trap, and the
  reason a deterministic codebase should treat "sort is stable enough" as a bug
  class rather than a detail.

### 9.3 Finding a desync

Two modes, and the split is instructive.

**[WUBE] FFF-47, "CRC fun"** describes the debug mode: "The game runs in a special
mode when it makes a CRC from the whole map every tick", replays are compared
tick-by-tick, and the first mismatch throws. Alongside it they save the map "with
human readable tags" every tick and **binary-diff the two saves at the first bad
tick** — and the difference is "usually very small… typically just a value of one
variable." Cost: performance drops to "units of FPS", and it takes "a lot of
patience."

In production the same machinery runs cheaply: real logs read
`Multiplayer desynchronisation: crc test (heuristic) failed for crcTick(9323753)
serverCRC(391068773) localCRC(592890033)` — a partial-state CRC per tick, server
against client. **[inferred]** Full-state hashing per tick is affordable only when
you have already given up the frame rate; the shipped build hashes a subset and
accepts a detection delay, which is the right trade when the *response* to
detection is a full state dump anyway.

**Desync reports.** **[WUBE] FFF-188**: "When a desync occurs, the game generates
a desync report. This report contains the game state from the server and the
client and by looking at the differences, we can try to determine what went
wrong." The wiki is careful about what the report is not: "The desync report does
not state the cause of the desync but rather is a record of the game state at the
time of the desync", and — the line that saves an enormous amount of misdirected
debugging — **"Networking-, latency or performance problems do *not* cause
desyncs."** A desync is always a determinism bug. Clients quit after three.

### 9.4 Two post-mortems worth keeping

**[WUBE] FFF-340, "Deep desyncs"** works through two, both surfaced by the
Krastorio 2 release wave, and both are failure modes any replicated simulation can
have:

**Iteration order, via a serializer.** The Construction Drones mod had circular
references (player ↔ drone). Serpent, the Lua serializer used for mod state,
marked already-visited tables with `nil` — but "In Lua, writing a table value as
nil is the same as deleting the key, and the key won't be seen when looping the
table in the future." Deleting and reinserting keys perturbs the hash part's
iteration order, so peers serialized the same table in different orders and
diverged after a save/load. The fix was to use `0` as the placeholder instead of
`nil`, preserving iteration order. **[inferred]** The C++ version of this is
FFF-52's sort comparator; the general rule is that **any unordered container
touched by replicated logic is a desync waiting to happen**, and the mitigation is
to iterate a deterministic order rather than to hope the containers match.

**A stale derived cache.** Unit groups cached the group's max speed (the slowest
member's). That was safe in 0.16 when unit speed was static; 0.17 added
tile-based speed effects and script-set speeds. Krastorio 2's creep tiles slowed
units, the cached speed was not recalculated on load, and a peer with the stale
cache diverged from one that recomputed. Fixed with a change notification forcing
recalculation.

**[inferred] That second one is CLAUDE.md's derived-cache rule with a network
consequence bolted on, and it is the sharpest warning in this note for this
project.** A derived cache in replicated state has a failure mode that a
single-player derived cache does not: it can be *correct on one machine and stale
on another*, so the bug does not present as wrong behaviour, it presents as a
disconnect with no stack trace. The discipline — invalidate at the boundary that
owns the data — is the same; the cost of skipping it is much higher.

### 9.5 Latency hiding: a speculative copy, rebuilt from scratch every tick

Lockstep means your own input has a round-trip delay before it takes effect, which
would be intolerable for movement. **[WUBE] FFF-83, "Hide the latency"** describes
the fix, and the mechanism is unusually clean:

> Every tick this latency state is cleared and initialized from the regular game
> state. Then all the buffered local user actions that hasn't been applied yet in
> the game state are applied to the latency state.

The screen is drawn from the latency state, and new user actions are created
against it. **[inferred] Because it is rebuilt from authoritative state every
tick, there is no rollback code path at all** — a misprediction simply fails to
reappear next tick. That is a significantly cheaper architecture than
rollback-and-replay, and it is available because the speculation window is short
and the speculated subset is small.

Latency-hidden: player movement, entity selection, opening/closing GUIs,
building and fast-replacing entities, mining, picking items to cursor.

Not hidden, and **[WUBE]** the reason is the important part: "we don't plan to do
any latency hiding for interacting with entities (apart from basic operations like
opening, rotating, etc.) or fighting", because "Shooting can result in cascade of
game state changes which would have to be captured in the special layer."

**[inferred] That sentence is the design rule.** The latency state can only
speculate on actions whose consequences stay inside the duplicated subset. An
action that ripples outward — a shot that damages a biter that wakes a nest that
re-paths a group — cannot be speculated without duplicating the whole world, at
which point you have written rollback netcode. *The set of predictable actions is
determined by blast radius, not by importance.*

2.0 extended it to vehicles (**[WUBE] FFF-412**), previously excluded; the edge
cases they name are the ones you would expect — speed modifiers, audio, particles,
damage, entry/exit — and one-shot effects had to become `LatencyOneTimeEffect`s so
a sound or particle does not play twice when prediction and reality differ
slightly.

### 9.6 Joining, replays, and why they are the same feature

**Joining** a running game: **[WUBE] FFF-149**, "Map upload is done in in the
background, while others are playing the game" — the joiner downloads the save
while the game continues, receives every input action produced during the
download, then simulates as fast as it can to catch up. **[WUBE] FFF-147**: under
the server model this no longer interrupts anyone, since the server "can just start
sending his actions as part of the merged package without any interruption."

**[inferred]** The catch-up is the determinism machinery again. A save is game
state at tick T; the action stream from T to now regenerates the present exactly.

**Replays** are the third use of the same mechanism, and were a stated byproduct
in **[WUBE] FFF-30** ("functional replays again"). They are also the *test rig* —
FFF-36's cross-platform trig bug was found by loading a replay on a different OS,
and FFF-47's CRC mode runs over replays. **[inferred] One mechanism, three
products: multiplayer, replays, and reproducible bug reports.** The general lesson
for a deterministic design is that the replay system is not a feature you add
later; it is the free consequence of the property you needed anyway, and it is the
only practical way to *test* that property.

### 9.7 The megapacket

**[WUBE] FFF-302, "The multiplayer megapacket"** is a good failure story because
the symptom and the cause are in completely different subsystems.

The event was KatherineOfSky's 200+ player game; Twinsen joined and was
disconnected "every, single, time" on a healthy connection. The mechanism:

> a client would sometimes end up in a situation where it would send a network
> package of about 400 entity selection input actions in one tick

Entity selection changes are "by far the most common input action sent by the
clients" (already compressed as a low-precision relative offset from the previous
selection). The server rebroadcasts those ~400 actions to 200+ clients, saturating
upload, causing packet loss, delaying input actions — and "Delayed input actions
then cause more clients to send megapackets, cascading even further."

The root cause was not bandwidth. It was **incomplete edge-case handling in the
latency-state queue** around tick-skipping and round-trip-latency renegotiation
(renegotiated every 5 seconds); a corrupted queue dumped ~400 backed-up selections
at once on recovery. Two weeks of work fixing "all the edge cases of updating and
maintaining the latency queue."

**[inferred]** The transferable part is the failure *shape*: a bug in the
speculative layer (§9.5) manifested as a network flood, and any amount of
profiling the network would have found congestion and not the cause. It also
demonstrates the positive-feedback risk in any design where *lateness produces more
traffic* — a property worth checking for deliberately, because it converts a small
hiccup into a total collapse above some player count.

For scale context: **[WUBE]** the protocol limit is 65,535 players, and "popular
streamers have managed to get well over a hundred" in practice.

**[COMMUNITY]** The community's answer to the ceiling is instructive by contrast.
Clusterio runs *many separate Factorio servers* bridged by out-of-game resource
teleportation; the Eternity Cluster event ran 400+ players across 160+ simultaneous
instances. **[inferred] Since every client simulates everything, a single world
cannot be sharded — so the community shards across worlds and glues them at the
application layer**, which is precisely the move
[`mmo_architecture.md`](../../topics/scale/mmo_architecture.md) describes EVE
making with solar systems, arrived at from outside the game by people who could not
change the engine.

---

## 10. Read from the install

Everything above is Wube's published record. This section is the check on it: a
read-only pass over `E:\SteamLibrary\steamapps\common\Factorio` — **version
2.0.77, API version 6** per the bundled `prototype-api.json` — with the game never
launched. It confirms the published account in every case where the two overlap,
and it supplies numbers no Friday Facts post gives.

### 10.1 The art, measured

**[SHIPPED-DATA]** Reading the IHDR header of every PNG in the install:

| | |
|---|---|
| PNG files | **8,522** |
| Total PNG bytes | **2,580 MB** |
| **Total source art** | **7,717 megapixels — 7.72 gigapixels** |
| Largest dimension found, either axis | **exactly 8192, with zero exceptions** |
| Lua files | 1,847 (7.6 MB) |
| Audio | 4,391 `.ogg` (1,329 MB) |

The 8192 cap is a documented rule the data obeys perfectly.
**[SHIPPED-DATA]** `AnimationParameters::line_length` explains why it exists:

> This is to allow having longer animations loaded in to Factorio's graphics
> matrix than the game engine's width limit of 8192px per input file. The
> restriction on input files is to be compatible with most graphics cards.

Individual *frames* are separately capped at 4096 px.

**[inferred] 7.72 gigapixels at RGBA8 is roughly 31 GB uncompressed** — about
thirty times the compressed VRAM figure in §2.5, and four times what a
high-end card has. **That single number is the reason every mechanism in §2.4–2.6
exists.** The art does not fit in memory by two orders of magnitude, so atlas
specialisation, block compression, priority-driven residency and per-surface
partitioning are not optimisations; they are the only reason the game can open its
own assets.

Distribution of the largest sheets is also informative: the biggest are the
**rail material layers at 4096 × 8192 (33.6 Mpx each)**, and the single largest
file is `vulcanus/tint-noise.png` at 17.1 MB.

### 10.2 An entity is N sheets, and the geometry reconciles exactly

The §3.3 layer anatomy is directly inspectable. `assembling-machine-2`
**[SHIPPED-DATA]**:

```lua
        layers =
        {
          { filename = ".../assembling-machine-2.png",
            width = 214, height = 218, frame_count = 32, line_length = 8,
            shift = util.by_pixel(0, 4), scale = 0.5 },
          { filename = ".../assembling-machine-2-shadow.png",
            width = 196, height = 163, frame_count = 32, line_length = 8,
            draw_as_shadow = true,
            shift = util.by_pixel(12, 4.75), scale = 0.5 }
        }
```

On disk: body **1712 × 872** (8 × 214 ✓, 4 × 218 ✓), shadow **1568 × 652**
(8 × 196 ✓, 4 × 163 ✓). Three facts fall out:

- **The shadow is a separate sheet with its own frame size and its own shift**
  (12, 4.75 px against the body's 0, 4) — because the shadow's bounding box is a
  different shape from the body's.
- **The shadow file is 32 KB against the body's 2.24 MB** — 69× smaller at 69% of
  the pixel count. **[inferred]** Shadows are single-channel silhouettes, which is
  corroborated by the existence of `shadow-a8.frag` and `sprite-r8.frag` shaders
  and a `group=shadow` atlas flag.
- **`scale = 0.5` is universal**: art is authored at 2× and drawn at half size.
  With the documented 32 px per tile at normal resolution, **the art is authored
  at 64 px per tile.**

**[SHIPPED-DATA] The shadow shift is always a large horizontal offset in the same
direction** — 12 px on the assembler, 14.5 on the furnace, 30 on the running
character. **[inferred] The sun direction is a global constant baked into every
sprite**, which is precisely the constraint §3.4's asteroid exception had to
escape.

**Tinting is a separate mask layer, not a shader parameter on the albedo.** The
biter's run animation is **four layers** — albedo, `mask1` (tinted), `mask2`
(tinted, `tint_as_overlay = true`), and shadow — so two independent tint channels
produce every biter colour variant across evolution tiers from one set of art.

**Frame counts, for calibration** **[SHIPPED-DATA]**:

| Animation | Frames | Sheet |
|---|---|---|
| Transport belt | **320** (16 frames × **20 direction/topology variants**) | 2048 × 2560, one file |
| Biter run | **256** (16 frames × 16 directions) **× 4 layers** | 3184 × 2480 × 4 files each |
| Biter attack | 176 (11 × 16) | 3916 × 1392 |
| Character running | 176 (22 × 8) | 1936 × 1056 |
| Character running **with gun** | **396** (22 × **18** directions) | 2376 × 2448 |
| Assembling machine 2 | 32 | 1712 × 872 |

**[inferred] The belt is a flipbook, not a scroll.** Twenty direction variants —
four straight runs, eight quarter-turns, eight start/end caps, all named in
commented-out index constants preserved in the shipped Lua — mean the "curve" a
belt appears to have is **pure sprite selection from neighbour topology**. There
is no geometry, no UV animation, and no per-belt state beyond a frame index. The
faster tiers differ only by `frame_count = 32` instead of 16.

And the largest frame counts are on *directions*, not on time: the character with
a gun has 18 aiming directions × 22 running frames. **[inferred]** In a
pre-rendered pipeline, every axis of variation multiplies into the sheet, which is
why §3.2's asset costs escalate the way they do.

### 10.3 The 71 render layers, and one entity scattered across six

**[SHIPPED-DATA]** The full ordered list, lowest to highest:

```
zero · background-transitions · under-tiles · decals · above-tiles ·
ground-layer-1 … ground-layer-5 · lower-radius-visualization · radius-visualization ·
transport-belt-integration · resource · building-smoke ·
rail-stone-path-lower · rail-stone-path · rail-tie · decorative ·
ground-patch · ground-patch-higher · ground-patch-higher2 ·
rail-chain-signal-metal · rail-screw · rail-metal · remnants · floor ·
transport-belt · transport-belt-endings · floor-mechanics-under-corpse · corpse ·
floor-mechanics · item · transport-belt-reader · lower-object ·
transport-belt-circuit-connector · lower-object-above-shadow · lower-object-overlay ·
object-under · object · cargo-hatch · higher-object-under · higher-object-above ·
train-stop-top · item-in-inserter-hand · above-inserters · wires · under-elevated ·
elevated-rail-stone-path-lower · elevated-rail-stone-path · elevated-rail-tie ·
elevated-rail-screw · elevated-rail-metal · elevated-lower-object ·
elevated-object · elevated-higher-object · fluid-visualization · wires-above ·
entity-info-icon · entity-info-icon-above · explosion · projectile · smoke ·
air-object · air-entity-info-icon · light-effect · selection-box ·
higher-selection-box · collision-selection-box · arrow · cursor
```

**[inferred] The most revealing artefact in the install**, and three readings:

**One: entities do not choose a depth, they choose a bucket.** Within a bucket the
sort is by Y position. This is a fixed global stratification, exactly as §2.7
describes, and the five empty `ground-layer-1…5` slots are deliberate extension
space for mods.

**Two: one entity's layers are deliberately scattered across many buckets.** The
circuit-reading transport belt puts its six sheets on six *different* layers —
`floor` (shadow and bottom), `transport-belt-endings` (under-middle),
`floor-mechanics` (middle), `transport-belt-reader` (base) and `object` (top).
**That is how a 2D game gets correct interleaving without depth**: the items
riding the belt, the belt surface and the reader housing each sort independently
against everything else on the map, because they were never one object as far as
the renderer is concerned.

**Three: the rail layers are why elevated rails were possible.** A rail is five
separately authored material layers — `stone_path_lower`, `stone_path`, `tie`,
`screw`, `metal` — each pinned to its own global layer, so a train's shadow can
fall between the ties and the metal. 2.0 then added a **complete parallel
six-layer elevated stack**. **[inferred]** §7.7 credits the rail rewrite for
making elevated rails possible; the layer list shows the other half of the reason
— *the render architecture already expressed "these parts of one object sort
independently", so a second elevation was a new set of buckets rather than a new
concept.*

Note also `light-effect` sitting near the very top, above all world geometry,
confirming §3.4's compositing order.

### 10.4 Atlas assignment is content-authored

**[SHIPPED-DATA]** `SpriteFlags` has 33 values, and roughly a third are explicit
atlas directives: `group=none`, `group=terrain`, `group=shadow`, `group=smoke`,
`group=decal`, `group=low-object`, `group=gui`, `group=icon`,
`group=icon-background`, `group=effect-texture` — alongside
`not-compressed` / `always-compressed` (per-sprite BC overrides), `mipmap`,
`terrain-effect-map` and `reflection-effect-map`.

This is §2.4's atlas specialisation, declared per sprite in data.

**2.0 added two more hints, and the reason is Space Age** **[SHIPPED-DATA]**:

> **`surface`** — "Provides hint to sprite atlas system, so it can try to put
> sprites that are intended to be used at the same locations to the same sprite
> atlas."
>
> **`usage`** — "Provides hint to sprite atlas system, so it can pack sprites that
> are related to each other to the same sprite atlas."

`SpriteUsageSurfaceHint` is `any, nauvis, vulcanus, gleba, fulgora, aquilo, space`;
`SpriteUsageHint` is `any, mining, tile-artifical, corpse-decay, enemy, player,
train, vehicle, explosion, rail, elevated-rail, air, remnant, decorative`. Every
biter layer in §10.2 carries `surface = "nauvis", usage = "enemy"`.

**[inferred] Five planets of art cannot be resident at once, so 2.0 made the art
declare which planet it belongs to.** That is the same idea as `priority` (§2.6)
one axis over: *residency decisions need information only the content author has,
so the content declares it rather than the engine guessing.*

One more shipped mechanism worth naming: **[SHIPPED-DATA]** `allow_forced_downscale`
lets a sprite "be downsampled to half its size on load even when 'Sprite quality'
graphics setting is set to 'High'", depending on "detected hardware and other
graphics settings" — and biters and rail remnants opt in. **[inferred]** A
content-declared quality floor: the art says *I am allowed to be halved*, so the
engine can shed memory on weak hardware without an artist reviewing every entity.

`dice` is the other packing lever — "number of slices this is sliced into when
using the 'optimized atlas packing' option" — and the biter uses `slice = 8`,
dicing each 398 × 310 frame into an 8 × 8 grid so fully transparent cells can be
discarded rather than packed.

### 10.5 The shaders ship as readable source, in three languages

**[SHIPPED-DATA]** `data/core/graphics/shaders/` contains:

| Form | Count |
|---|---|
| GLSL `.frag` / `.vert` | **68 / 34** |
| HLSL `.psh` / `.vsh` | **68 / 34** |
| Metal `.metal` | **102** (68 frag + 34 vert) |
| Precompiled D3D `.cso` | **3** |

**A perfect 1:1:1 ratio**, and the HLSL carries `SPIRV_Cross_Input` /
`SPIRV_Cross_Output` structs while the *GLSL* on disk has machine-generated
variable names (`_78`, `_217`, `_240`). **[inferred] The pipeline is: author once,
compile to SPIR-V, transpile to HLSL and MSL, ship all three as source and compile
at runtime for whichever backend is live.** That is how one renderer serves
OpenGL, Direct3D 11 and Metal without three hand-maintained shader sets — and it
is a strong argument for SPIR-V as the authoring intermediate in any project
facing the same spread.

**The three precompiled blobs are all texture compressors** —
`compress-bc4.psh.cso`, `compress-rygDXT.psh.cso`, `compress-ycocg-dxt.psh.cso`.
**[inferred] Factorio compresses its 7.7 gigapixels to BC formats *on the GPU at
load time*,** which is why §2.5's compression costs no disk space and why loading
is as fast as it is. They must be precompiled because they run before the shader
compiler is warm.

The inventory itself documents the renderer. Beyond the sprite and tile paths,
note: **YCoCg variants of everything** (`sprite_ycocg`, `tiles-ycocg`,
`mipmap-gen-ycocg`, `compress-ycocg-dxt`) confirming §2.5's default format;
`sprite565` (a 16-bit path for alpha-free sprites); `sprite-r8` and `shadow-a8`
(the single-channel paths §10.2 predicted); **three dedicated tree shaders**
(`sprite-treeLeaves.vert`, two leaf fragment shaders, `sprite-r8-treeShadow.frag`)
making foliage the only entity class with vertex-level animation; `blend-luts.frag`
plus the `sampler3D lut` in `sprite.frag`, which is §3.4's day/night grade; and
**`turret-ranges-prepass.frag` alongside `turret-ranges.frag`** — the two-pass
union from §2.9, still shipping.

Nine colour LUTs ship in `core/graphics/color_luts/`: `identity`, `dawn`, `day`,
`night`, `sunset`, `orange-dawn`, `nightvision`, and **`frozen`** (Aquilo).
**[inferred]** The entire day/night cycle, the nightvision equipment and a
planet's signature look are one 3D texture lookup selected per sprite by a flag
bit — which is why §3.4's night model composites so cheaply.

**Backends**, from an ASCII scan of `factorio.exe` (43.5 MB, shipped with a
403 MB PDB) **[SHIPPED-DATA]**: 175 `GL_ARB`, 93 `angle`, 69 `OpenGL`, 63 `EGL`,
31 `D3D11`, 11 `Vulkan`, 6 `Direct3D`, 3 `d3dcompiler`, and **zero `D3D12`**. No
backend DLLs ship at all — `bin/x64/` holds only the executable, its PDB,
`steam_api64.dll` and a Razer Chroma XML. **[inferred]** OpenGL and Direct3D 11,
with **ANGLE statically linked** as the GL-over-D3D11 fallback, `d3dcompiler` for
runtime HLSL compilation, and no D3D12 path.

### 10.6 Tile effects: where the runtime shading actually lives

§6.6 covers animated water from the Friday Facts. The install shows how far that
generalised. **[SHIPPED-DATA]** `TileEffectDefinition` has three shaders —
`water`, `puddle`, `space` — and an `EffectVariation` enum of
`lava, wetland-water, oil, water`. Base ships **one** tile effect; Space Age ships
**thirteen**.

Base water is parameterised by a 512 × 512 noise texture plus thresholds
(`dark_threshold`, `reflection_threshold`, `specular_threshold`, `foam_color`,
`animation_speed`, `tick_scale`). Vulcanus lava reuses `shader = "water"` with
`shader_variation = "lava"` and a second 2048 × 1024 texture. Fulgora's deep oil
uses **four** textures, including — with the comments preserved in the shipped
source — a *"gradient map for thin film effect"* and a dedicated specular sheet.
Gleba's puddles are a water effect masked by a noise texture. Space platforms use
`shader = "space"` with fifteen parameters driving a **fully procedural
starfield** (`star_density`, `star_parallax`, `nebula_scale`, `zoom_exponent`…).

**[inferred] One shader family, four visual identities, all authored as data.**
The deep-oil case is the tell: iridescent thin-film shading on an ocean of waste
oil is a bespoke look, and it was reached by adding two textures and a variation
enum to the water shader rather than by writing an oil shader.

**And Space Age added lightmapped terrain.** **[SHIPPED-DATA]** Tiles can carry a
`lightmap_layout` spritesheet alongside the overlay/background/mask blocks, with
the shipped comment `-- this added the lightmap spritesheet` next to Aquilo's
lava-stone transitions. The `lightmap_alpha` documentation is the clearest
statement anywhere of how terrain and lighting interact:

> Value 0 makes water appear as water in water mask, but does not occlude lights,
> and doesn't overwrite lightmap alpha drawn to pixel previously… Light emitted by
> water-like-tile (for example lava) will blend additively with previously
> rendered light. Value 1 makes water occlude lights, but won't be recognized as
> water in water mask used for masking decals by water.

**[inferred] Lava emits into the same light buffer §3.4 describes** — so a lava
lake lights the machines beside it through exactly the mechanism a lamp uses, and
the tile renderer participates in the light map rather than being composited under
it.

### 10.7 Counts

**[SHIPPED-DATA]**

| | base | space-age | elevated-rails | quality |
|---|---|---|---|---|
| Entity-prototype declarations | **~703** | 249 | 22 | 4 |
| `type = "tile"` | **40** | **89** | — | — |
| `type = "tile-effect"` | 1 | 13 | — | — |
| `type = "optimized-decorative"` | **41** | **115** | — | — |
| PNG files | 4,246 | 3,517 | 187 | 53 |
| Install size | 1,269 MB | 2,506 MB | 123 MB | 39 MB |

The entity count is lexical and therefore an upper bound — a few entity typenames
also appear as nested trigger discriminators — but the taxonomy behind it is
exact: **133 instantiable entity types** derive from `EntityPrototype`, and the
Space Age additions are legible in the list (`segment`/`segmented-unit` are the
demolishers, `lightning`/`lightning-attractor` Fulgora, `plant`/`agricultural-tower`
Gleba, `thruster`/`asteroid`/`asteroid-collector`/`cargo-pod` the space platforms).

**Decoratives** are ~156 prototypes at **12–20 variations each — roughly 2,500
distinct sprites**, and each variation is individually cropped with its own width,
height and shift rather than sharing a frame. `TilePrototype` carries
`bound_decoratives` and `decorative_removal_probability`, so **decorative placement
is a property of the terrain prototype**, not a separate pass — §6.1's fourth
frequency band, wired directly to the tile that owns it.

---

## 11. What transfers

Ranked by what this project could actually use, most valuable first.

**1. Find the domain invariant before optimising the data structure** (§7.2). The
belt rewrite is 50–100× not because of cache layout but because *belts only ever
compress and never spontaneously decompress*, which makes a one-way cursor
sufficient and turns an O(n) scan into amortised O(1). The general procedure is to
ask what the rules of the game forbid before asking how to lay out the array — and
it is the highest-leverage thing in this note by a wide margin.

**2. Use a hierarchy as a heuristic, not as the answer** (§7.8). Factorio's
abstract pathfinder supplies only `h()`; the base search stays authoritative. A
stale abstraction then costs speed and can never cost correctness. This is the
cheapest safe way to add hierarchy to an existing grid search, and it is directly
applicable here.

**3. Read-heavy passes thread; write-heavy passes do not** (§8.1–8.3). Established
twice, seven years apart, with a clean control experiment. Combined with the
corollary — that the obstacle is allocator-induced false sharing, not task
decomposition — it is a better decision procedure than "are these tasks
independent", which is what misled Wube in the first place.

**4. When a state change breaks batching, encode it as data instead** (§2.3).
Premultiplied alpha plus a per-sprite tint gives additive, alpha and
partial-additive blending in a single batch with no API state change. Directly
usable in this project's UI painter and in any particle work.

**5. Workers produce intentions; a serial merge applies them in a defined order**
(§8.2). The general pattern for parallelising anything that must be deterministic
or reproducible — which includes anything replayable, anything networked, and
anything whose bugs you want to be able to reproduce from a seed.

**6. Constrain the generator to what the art can draw** (§5.5). Factorio's cliff
generator removes edges until no cell exceeds two cliff crossings, guaranteeing a
sprite exists for every configuration it can emit. Compare §6.4, where an
incomplete transition alphabet instead forced a *correction pass over the world*
that then constrained map generation for years. **Constraining the producer is far
cheaper than repairing the product**, and this project has exactly the same tile
transition problem.

**7. Factor transitions into shared shape × per-material fill** (§6.3). Masks are
shared across terrain pairs, so N terrain types cost O(N) art rather than O(N²).
This is the difference between a tile game that ships four ground types and one
that ships forty.

**8. Decide once, coast N ticks, interpolate in the renderer** (§7.10). Robots
commit to a 20-tick movement intention and the renderer fakes smooth motion
between updates. Works only because the renderer is strictly downstream of the
simulation and never feeds back — which is a property worth preserving
deliberately.

**9. Give a repeated decision an accumulating penalty rather than a threshold**
(§7.6). The train that has been waiting gains 0.1 cost per tick, so traffic routes
around jams with no jam detection. Compare CLAUDE.md's incumbency-discount rule:
both are cases where a continuously varying bias replaces a discrete state machine
and degrades better.

**10. Two structures for two questions** (§7.3): a per-chunk spatial index for
"what is near here", and a global active list for "what needs a tick". This
project already has the first; the second is the pattern to reach for when entity
counts grow.

**11. Aggregate networks, and accept losing per-member queries** (§7.4). Cost
scales with the number of networks, not their size — and per-entity flow fields
stopped being adjustable as the price. Know which questions you are giving up
before aggregating.

**12. Hierarchical coverage for unions of overlapping shapes** (§2.9). Render at
1/16 resolution, stencil-mark only the uncertain boundary pixels, run the
expensive shader only there — 20× on turret ranges. Applies directly to movement
and ability range visualisation here.

**13. Put the whole per-sprite shading vocabulary in one integer** (§3.4). Six
independent behaviours — invert, tint, tint-as-overlay, colour LUT, greyscale,
light/occlude — are bits of a flat per-vertex `uint` branched on inside a single
übershader, so none of them can break a batch or change pipeline state. The
generalisation of idea 4, and readable in the shipped `sprite.frag`.

**14. Author shaders once and transpile** (§10.5). Factorio ships 68 fragment and
34 vertex shaders as GLSL, HLSL *and* Metal at a perfect 1:1:1 ratio, with
SPIRV-Cross fingerprints in all three. One authored source, compiled to SPIR-V,
transpiled per backend, compiled at load. The right answer for any project facing
more than one graphics API — and worth knowing before writing a second shader by
hand.

**15. Let content declare its own residency and quality floor** (§10.4). Sprites
carry a `priority` (VRAM versus streaming), a `surface`/`usage` atlas hint, and
`allow_forced_downscale` — *"I may be halved on weak hardware"*. Residency needs
information only the author has, so the engine asks rather than guessing.

**16. Bake metadata into the render and parse it back out** (§3.1). Coloured pixel
markers carry wire attachment points from Blender into the sprite, so attachment
data cannot drift from the art. The right answer for any 3D→2D bake pipeline.

**17. Hash position into a phase offset** (§3.4). `offset_flicker` desynchronises
identical stationary lights with one line. The same trick as the six independent
randomisations in
[`broken_arrow_squads.md`](../flight/broken_arrow/broken_arrow_squads.md) §2.2.

**18. Add the expensive path only where the assumption breaks** (§3.4). One normal
map exists in the entire game, on the one entity class that tumbles freely and so
has no fixed sun. Wube wrote a bespoke lit shader for asteroids rather than
generalising the renderer — and the rule that identifies such cases is worth
having: *find the assumption your cheap path rests on, and add the expensive path
exactly where it stops holding.*

**And two things not to copy.**

**Do not read Factorio's rejection of LOD as general** (§4). It has no LOD chain
because in a fixed-projection 2D game the mip chain *is* the LOD chain and it is
free. A 3D project inherits none of that.

**Do not let asset authoring lock a rendering decision by accident** (§2.7). Wube
cannot add a depth buffer — with the large batching win it would bring — because
a decade of sprites were authored with antialiased edges and heavy transparency.
The decision was made in year one and ratified by every asset since. Whatever this
project bakes into its content pipeline early is similarly permanent, and it is
worth knowing which decisions those are *before* the content exists.

---

## 12. What was not established

The Friday Facts are an unusually good source, which makes it worth being precise
about where they stop. Everything below is a gap, not a claim.

**Rendering**

- **Whether the FFF-264 virtual atlas shipped in the form described** (§2.6).
  Partly resolved: the shipped `SpritePriority` docs describe sprite residency as
  "included in VRAM instead of streaming it" and cite FFF-264 directly, so
  priority-driven streaming is retail. Whether the 128×128-tile indirection scheme
  behind it survived is still unconfirmed.
- **Virtual/physical atlas dimensions.** Only the 128×128 tile granularity is
  stated in the post, and nothing in the install exposes the runtime layout.
- **Old-vs-new renderer speedup in FFF-251.** The post gives methodology and byte
  counts but no before/after millisecond or FPS figure that could be confirmed.
- **The chart-view handoff** (§4.4) — the zoom threshold, and how chart view is
  rasterised. No post found.
- **The exact light-map composite formula** (§3.4). The mechanism is well
  supported across several posila forum posts; the precise per-pixel operation
  combining light map, night LUT and game view is inference.
- **Sprite draw-order sorting details within layers.** That some layers sort by
  position and secondary draw order is stated; the exact rule is not.

**Simulation**

- **A canonical update order of entity classes.** The subsystem managers are
  visible in the profiler categories (§7.9), but no Wube statement enumerates the
  sequence.
- **Splitter and priority algorithm.** FFF-176 establishes only that splitters cut
  transport lines. The balancing/priority logic is undocumented.
- **Whether fluids, heat, or robot/logistics updates are multithreaded in 2.0.**
  Not established either way.
- **The elevated-rail internal representation** (§7.7) — layer, z-coordinate, or
  separate surface. FFF-378 stays at the gameplay level deliberately.
- **Quality's per-entity simulation cost** (2.0). No Wube statement found; the
  suggestion that quality is UPS-positive by reducing entity counts is inference
  and should be treated as such.
- **A full per-system millisecond breakdown of one megabase tick.** The numbers in
  §7 and §8 are per-optimisation deltas from *different saves and different
  versions* and **must not be summed**.
- **Real megabase entity counts from a primary source.** The commonly cited
  figures are community save statistics, not Wube's.
- **Unit group movement** — how biter swarms form, cohere and share paths. FFF-317
  covers the pathfinder, not the group behaviour.

**Terrain**

- **The richness-versus-distance law.** The behaviour is real and the mechanism
  (noise expressions, spot-noise cones) is sourced, but no verbatim statement of
  the distance scaling was captured.
- **Whether vanilla ever deletes chunks.** The wiki is silent; only the scripting
  API's deletion is known, and §5.1's "nothing is ever unloaded" rests on the
  wiki's "stored in the player's RAM, which is the practical limiting factor of
  exploration" rather than on an explicit statement that eviction does not exist.

**Multiplayer**

- **Which subset of state the live heuristic CRC covers** (§9.3). The mechanism is
  proven by the log message format; its coverage is undocumented.
- **Which state fields are double and which are fixed point.** Positions are
  documented as 1/256 fixed point; there is no published enumeration beyond that,
  and FFF-52 confirms floats are used somewhere in the simulation.
- **FFF-302's bandwidth figures**, and FFF-76's latency-buffer numbers — not
  present in the posts.

**The install — what it settled, and what it could not**

§10 is a read-only pass over the 2.0.77 install, and it upgraded a good deal of
§2, §3 and §6 from published claim to measured fact. What it could **not** reach:

- **`data.raw` as the game actually builds it.** §10.7's prototype counts are
  *lexical* — every `type = "..."` in the Lua, filtered against the 133
  instantiable entity typenames. A few entity typenames (`fire`, `explosion`,
  `corpse`, `projectile`, `container`) also appear as nested trigger
  discriminators, so ~703 is an upper bound on base entity prototypes. The true
  count needs the game running.
- **Whether world sprites get mipmaps.** The evidence points both ways and was not
  resolved: `mipmap_count` is documented as loaded "only if this is an icon", and
  the animation docs state mipmaps cannot be laid out for a multi-frame animation
  at all — yet a `mipmap` sprite *flag* exists, `mipmap = true` appears explicitly
  on the rail metal and backplate sheets, and FFF-227 describes enabling mipmaps
  for terrain and decoratives. **[inferred]** The likely resolution is that
  single-frame sprites and tiles can be mipmapped via the flag while animations
  cannot, but that was not confirmed.
- **The atlas layout at runtime.** The `group=*` flags and the `surface`/`usage`
  hints say how sprites are *assigned*; how many atlases actually exist, at what
  dimensions, and how the packer resolves the hints is not visible from data.
- **Compiled behaviour.** The shaders ship as source and were read, but the C++ is
  a single 43.5 MB executable. Everything about update order, allocation, the
  active-entity structures and the belt segment representation remains as §7 has
  it — Wube's description, not observed code. The 403 MB PDB shipped alongside it
  would make far more legible, and was not opened.
- **Nothing was run.** No save was loaded, no benchmark executed, and no in-game
  profiler output was captured — so every performance number in this note is
  Wube's or the community's, and none is ours.
