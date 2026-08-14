# OpenRA — the engine under D.O.R.F., read from source

A teardown of the four subsystems asked about — **rendering, input, resource
loading and networking** — plus the game loop and trait system they hang off,
read from the `bleed` branch downloaded on **2026-08-14** (no version tag: the
tree ships `VERSION` as the literal `{DEV_VERSION}` placeholder, stamped by the
packaging step).

Why it earns a note of its own: [`dorf.md`](dorf.md) describes a commercial RTS
built on this engine, and every architectural claim in that note is either
inherited from here or a deliberate departure from here. This is the baseline.
It is also, separately, the most legible **deterministic-lockstep RTS** in
existence — the whole netcode is ~3,900 lines of readable C# with the design
reasoning in comments, which is not true of any shipped commercial RTS.

**Sizing, measured:**

| Project | Files | Lines | What it is |
|---|---:|---:|---|
| `OpenRA.Game` | 226 | 43,868 | Engine: loop, renderer, netcode, filesystem, traits, server |
| `OpenRA.Mods.Common` | 1,084 | 158,816 | The actual game logic, as traits |
| `OpenRA.Mods.Cnc` | 140 | 20,769 | C&C/TS-specific traits and loaders |
| `OpenRA.Platforms.Default` | 18 | 4,752 | SDL2 + OpenGL + OpenAL + FreeType |
| `OpenRA.Mods.D2k` | 23 | 3,546 | Dune 2000 |
| `OpenRA.Test` | 20 | 4,248 | Tests |

**The ratio is the first finding.** The engine is a quarter of the mod library
it serves — 44k lines of engine against 183k lines of traits. Almost nothing a
player would call "the game" lives in `OpenRA.Game`; the engine's job is to
load data, order simulation deterministically, and draw sprites. **A mod is a
`.dll` plus a yaml tree, and the mods that ship are written the same way a
third-party mod is.**

## Sourcing

| Tag | Meaning |
|---|---|
| **[SRC]** | Read from the `bleed` tarball, 2026-08-14. Paths and identifiers are OpenRA's own |
| **[YAML]** | Read from the shipped mod data in `mods/` |
| **[inferred]** | My reading |

Related: [`dorf.md`](dorf.md) — the commercial fork, and §9 here is the direct read
of what it had to change. [`factorio.md`](factorio.md) — the other deterministic-lockstep
2D game in this directory, at a scale that forced different answers to the same
questions; read its §9 against §3 here. [`ruse.md`](ruse.md) §9 for the contrasting
(client-server, non-deterministic) model.

---

## 1. The spine: two clocks, deliberately decoupled

**[SRC]** `Game.Loop()` is 90 lines and worth reading in full; the design is in
its own comments.

There are two independent schedules — `nextLogic` and `nextRender` — and one
`while` loop that services whichever is due, sleeping on
`Thread.Sleep(nextUpdate - now)` when neither is. The interesting parts are the
guards around them:

- **`MaxLogicTicksBehind = 250` ms.** If logic has fallen more than a quarter
  second behind, `nextLogic` is snapped to now and the backlog is *abandoned*
  rather than caught up. The comment explains why: a temporary 100 ms hitch on a
  10 ms tick otherwise leaves the clock permanently chasing.
- **`renderBeforeNextTick`.** During normal play, every logic tick sets a flag
  forcing at least one render before the next one. So the game will not run two
  simulation steps without drawing.
- **`MinReplayFps = 10`** and `forcedNextRender`. During replays, logic is
  allowed to outrun rendering — but never by more than 100 ms, so a fast-forward
  still shows something.
- **The render is skipped when the window is suspended**, but SDL is still
  pumped through a `NullInputHandler` so a restore event can arrive.

**[SRC]** The `TODO` in that comment block is the one D.O.R.F. inherits:

> *"It would be nice to separate the world rendering from the UI rendering so
> that we can update the UI more often than the world… It's not possible at the
> moment because the render buffer is cleared before rendering and we don't keep
> the last rendered world buffer."*

**[inferred]** That is a real architectural constraint, not laziness: §4's
compositor blits the world buffer into the screen buffer every frame and then
draws UI on top, so "reuse last frame's world" would need the world buffer to
survive a frame it did not render into. It is a five-line change and a large
correctness argument, which is why it has not been made.

**Tick rate is mod data, not code.** **[YAML]** `mods/ra/mod.yaml`:

| Speed | `Timestep` (ms) | `OrderLatency` (frames) | Input delay |
|---|---:|---:|---:|
| slowest | 80 | 2 | 160 ms |
| slower | 50 | 3 | 150 ms |
| **default** | **40** | **3** | **120 ms** |
| fast | 35 | 4 | 140 ms |
| faster | 30 | 4 | 120 ms |
| fastest | 20 | 6 | 120 ms |

**The right-hand column is the point.** `OrderLatency` is not a constant; it is
tuned per speed so that *wall-clock* input delay stays at 120–160 ms across a 4×
range of tick rates. Faster game speed does not buy a more responsive game — it
buys more simulation per second at the same felt latency. **[inferred]** Anyone
copying a lockstep delay should copy this shape: the tunable that matters is
milliseconds, and the frame count is derived.

`World.Tick()` **[SRC]** is then trivially simple — increment `WorldTick`, tick
every actor, tick every `ITick` trait, tick effects, then drain a
`frameEndActions` queue. There is no fixed/variable split, no interpolation in
the simulation, and no substepping. All smoothing lives in the render layer.

---

## 2. Everything is an order, and the engine enforces it

This is the single organising idea, and it is enforced at runtime rather than by
convention.

**[SRC]** `Sync.RunUnsynced(world, fn)` computes `world.SyncHash()` before
running `fn`, runs it, and throws
`InvalidOperationException("RunUnsynced: sync-changing code may not run here")`
if the hash changed. Everything that is *not* the simulation is wrapped in it:

```csharp
Sync.RunUnsynced(world, Ui.Tick);                                  // Game.cs
Sync.RunUnsynced(world, orderManager.TickImmediate);
Sync.RunUnsynced(world, () => world.OrderGenerator.Tick(world));
Sync.RunUnsynced(world, () => world.TickRender(worldRenderer));
Sync.RunUnsynced(world, () => Ui.HandleInput(input));              // InputHandler.cs
Sync.RunUnsynced(world, () => Ui.HandleKeyPress(input));
```

**So input handling, UI, order generation and render-tick code are all
structurally forbidden from touching simulation state, and the violation is
caught the first time it runs** — with a nesting counter so re-entrant calls
only check at the top level, and a settings flag
(`Debug.SyncCheckUnsyncedCode`) to disable the cost in release.

**[inferred]** This is the most directly stealable idea in the engine, and it is
not specific to lockstep. Any engine with a "simulation state" and a "view
state" can afford a debug-build hash of the former around every entry point of
the latter. The bug it catches — a renderer or a UI handler mutating game state,
which in a networked game surfaces as a desync with no stack trace hours later —
is otherwise one of the hardest classes of bug there is.

### 2.1 The sync hash is IL emitted per type

**[SRC]** `Sync.cs`. Fields and properties tagged `[Sync]` are collected per
type and a hash function is **compiled at runtime with `System.Reflection.Emit`**
— one `DynamicMethod` per `ISync` type, XORing each member's hash, cached in a
`ConcurrentCache<Type, Func<object, int>>`. Custom hashes exist for the engine's
fixed-point types (`WPos`, `WVec`, `WAngle`, `WRot`, `WDist`, `CPos`, `CVec`,
`int2`) and for `Actor`/`Player`/`Target`, which hash by `ActorID` rather than
by contents.

`World.SyncHash()` **[SRC]** then folds together, each weighted by an
incrementing counter and by `ActorID` so ordering matters:

1. every actor (`ActorID`),
2. every `[Sync]` member of every trait implementing `ISync`,
3. every synced effect (projectiles),
4. **`SharedRandom.Last`** — the RNG's own state is part of the hash,
5. per-player `RenderPlayer` unlock status.

**[inferred]** Including the RNG state is the detail that turns desync debugging
from archaeology into bisection: if the hash diverges and the RNG term is the
cause, someone consumed a random number in unsynced code, which is a different
bug from someone computing a different value.

### 2.2 Fixed point everywhere, and no floats in the simulation

**[SRC]** The world coordinate types (`WPos`, `WVec`, `WDist`, `WAngle`, `WRot`,
`CPos`, `CVec`, `MPos`) are integer types. The renderer's types (`float3`,
`Viewport.Zoom`) are float. The boundary is the boundary between §2's two
worlds. **[inferred]** This is the opposite choice from Factorio (see
[`factorio.md`](factorio.md) §9, where floats in the simulation are used deliberately and
the folklore that they cannot be is called out as false) and it is the more
conservative one; OpenRA never has to care what libm a platform ships.

---

## 3. Networking: deterministic lockstep, with the interesting bits

Nothing but orders crosses the wire. No positions, no health, no world state.

### 3.1 Frames, queues and the readiness rule

**[SRC]** `OrderManager` holds `pendingOrders`: one `Queue<(int Frame,
OrderPacket Orders)>` **per client**. The core predicate is one line:

```csharp
bool IsReadyForNextFrame => GameStarted && pendingOrders.All(p => p.Value.Count > 0);
```

**Every client must contribute a packet for every frame, even an empty one** —
the comment says so explicitly: *"We expect every frame to have a queued order
packet, even if it contains no orders, as this controls the pacing of the game
simulation."* A frame number mismatch is a hard `InvalidDataException` rather
than a silent skip: *"so we can crash early instead of desyncing."*

`TryTick()` is the whole flow control:

1. If this is a **net frame** (`LocalFrameNumber % NetFrameInterval == 0`,
   **[SRC]** `NetFrameInterval` defaults to **3**), check whether every *other*
   client already has a packet queued. If not, do not send — *"this prevents us
   sending orders too soon if we are stalling"*.
2. If ready, send this client's orders for `NetFrameNumber`.
3. If every client's queue is non-empty, dequeue one packet each, deserialise
   and apply the orders, emit a sync packet, and `++NetFrameNumber`.
4. Return whether the world may tick.

**[SRC]** `NetFrameInterval` exists so *"the server may request clients to batch
multiple frames worth of orders into a single packet to improve robustness
against network jitter at the expense of input latency"* — one packet per three
simulation frames by default.

### 3.2 The Ack packet — a small, clever bandwidth trick

**[SRC]** `Server.ReceiveOrders`. A client's orders must be applied on the same
world tick by everyone, so the server shifts them into the future by
`OrderLatency` frames and forwards them. But sending a client its **own** packet
back merely to change the frame number would be wasteful, so the server replies
with a 5-byte `Ack(frame, count)` and the client pops its own locally buffered
packet and applies it at that frame:

> *"The Acknowledgement packet is a placeholder that tells us to process the
> first packet in our local sent buffer and the frame at which it should be
> applied. This is an optimization to avoid having to send the (much larger than
> 5 byte) packet back to us over the network."*

`ackCount > 1` is handled by `OrderPacket.Combine`, so several buffered frames
can be collapsed into one application. **[inferred]** This is the mechanism that
would let a dynamic-latency system exist; the `TODO: Replace static latency with
a dynamic order buffering system` appears three times in `Server.cs`, so it is
known to be the next move.

### 3.3 Per-client tick scaling — the part nobody documents

**[SRC]** `Server/OrderBuffer.cs`, 139 lines, and the most unusual thing in the
netcode. In lockstep, one slow client stalls everybody. Rather than only
detecting that, the server continuously *paces* clients:

- Every order packet stamps `gameTimer.ElapsedMilliseconds` for that player.
- Once all players have stamped, each player's delta **relative to a baseline
  player** is pushed into a rolling queue of the last `NumberOfFrames = 20`.
- Every `Interval = 1000` ms, the **median** delta per player is converted to a
  per-tick correction, `tickScale = (timestep + deltaPerTick) / timestep`,
  clamped to `[1, MaxTickScale = 1.1]`, and sent to that client as a
  `TickScale` packet.
- The client feeds it into `OrderManager.SuggestedTimestep`, which
  `Game.Loop()` reads as `logicInterval`.

**So a client that is consistently ahead is told to run up to 10% slower**,
converging the fleet instead of letting the slowest client hard-stall the rest.
Median over 20 samples rather than a mean, so a single jitter spike does not
move it; the baseline is re-elected if that player leaves.

**[inferred]** The design insight generalises well beyond RTS: in any
barrier-synchronised distributed simulation, **it is cheaper to slow the fast
participants smoothly than to stall everyone abruptly when the slow one
arrives** — a 10% ceiling is imperceptible, a stall is not.

### 3.4 Latency measurement that includes your own frame time

**[SRC]** `Connection.Receive` answers the server's ping *on the game thread*,
not on the network receive thread, with a comment saying exactly why:

> *"Note that processing this here, rather than in NetworkConnectionReceive, so
> that poor world tick performance can be reflected in the latency
> measurement."*

The response carries `(byte)orderManager.OrderQueueLength` alongside the
timestamp, so the server sees both network RTT and how deep that client's order
backlog is. **[inferred]** Measuring latency at the socket would report a
healthy 20 ms for a client whose simulation is 400 ms behind — which is the
number that actually matters to everyone else.

### 3.5 Transport, handshake and the protocol surface

**[SRC]** Plain **TCP with `NoDelay = true`**, no UDP path, no reliability
layer of its own — lockstep cannot proceed with a hole in the stream, so
ordered-reliable is what it wants. Connect races all resolved endpoints in
parallel threads (v4/v6) with a **5 s** budget and keeps the first to succeed:
*"such high latency makes the game unplayable anyway."* Receive is a dedicated
background thread doing `len / clientId / payload` framing into a
`ConcurrentQueue`; the game thread drains it inside `TickImmediate`.

Two protocol versions are pinned separately **[SRC]**: `Handshake = 7` (rarely
changes) and `Orders = 21`. `Server.cs` disconnects on either mismatch, and on a
mod-or-version mismatch, with a distinct error each.

Orders come in two flavours **[SRC]**: **immediate** orders (chat, lobby
commands, pause) which are sent with frame 0 and applied on arrival, and
**synced** orders, which are the only things allowed to touch the world.
`OrderPacket` always keeps orders **serialised**, never as objects, with the
reasoning in the comment: *"Orders may refer to actors that no longer exist by
the time that the order is resolved. In order to ensure consistent behaviour
between local and remote clients, it is simplest to always serialize /
deserialize orders"* — so the local client walks the identical code path as a
remote one, including the actor-lookup failure case. **[inferred]** This is the
single most effective anti-desync discipline in the codebase: there is no
"local fast path" to diverge.

The singleplayer case is not a special case either: `EchoConnection` implements
the same `IConnection` and projects orders forward by one frame, injecting an
empty frame 0 *"to fill the gap we are making by projecting forward orders"*.

### 3.6 Replays and saves are the same thing as the network stream

**[SRC]** `ReplayRecorder` is wired into `NetworkConnection.Receive` and writes
**the wire packets**, including the client's own reconstructed ones. `GameSave`
does the same server-side. `ReplayConnection` then implements `IConnection` by
replaying that file.

**[inferred]** So replay, save-game and multiplayer are one mechanism seen three
ways, and a save is an order log rather than a state dump — which is why loading
one means *re-simulating* it (`IsLoadingGameSave` sets `logicInterval = 1` and
`renderInterval = 200` to fast-forward at 5 FPS). That trade is the honest cost:
saves are tiny and always consistent, and loading a long game is slow.

### 3.7 Desync handling

**[SRC]** Every client sends `SendSync(frame, World.SyncHash(), defeatState)`
each net frame. `ReceiveSync` stores the first hash seen for a frame and
compares every later one against it; on mismatch, `OutOfSync(frame)` dumps a
sync report and marks the game unusable — the field comment is blunt: *"The game
cannot reliably continue in this condition and is unusable."* Reports are only
generated when there is someone to compare against and the lobby enabled them,
because *"generating sync reports is expensive"*. `SyncReport` (344 lines) keeps
a ring of recent frames with per-trait hashes and the orders that were applied,
so the dump names the trait that diverged.

**The defeat state is hashed alongside** — a `ulong` bitmask of which players
have lost — which catches divergence in victory conditions specifically, a
category that would otherwise only surface at the end of a match.

---

## 4. Rendering

### 4.1 A dedicated GL thread with a message queue

**[SRC]** `ThreadedGraphicsContext` (836 lines) owns a `Thread` and a
`Queue<Message>`; the game thread never touches GL. Two dispatch verbs:
`Post` (fire and forget) and `Send` (block for the result). Messages and
per-type vertex buffers are pooled to keep the GC out of the frame.

`Present()` is a `Post` — the game thread does not wait for the swap. The
back-pressure is applied at the *other* end of the frame:

```csharp
public void Clear()
{
    // We send the clear even though we could just post it.
    // This ensures all previous messages have been processed before we return.
    // This prevents us from queuing up work faster than it can be processed if rendering is behind.
    Send(doClear);
}
```

**[inferred]** That is the whole frame-pacing policy in one comment: **one frame
of queue depth, enforced by blocking on the first call of the next frame rather
than the last call of this one.** It buys the CPU/GPU overlap without the
unbounded-queue failure mode where input lag grows silently under load.

### 4.2 Two framebuffers, and a pixel-art filter between them

**[SRC]** `Renderer` composites through two render targets:

1. **`worldBuffer`** — the world drawn at 1:1 *world* pixels. `BeginWorld`
   computes an integer `WorldDownscaleFactor`, incrementing it until the
   viewport fits the buffer, so world rendering is always at an integer scale.
2. **`screenBuffer`** — `BeginUI` blits the world buffer into it with
   `EnablePixelArtScaling(true)`, then draws all UI on top at native scale.
3. The screen buffer is blitted to the window in `EndFrame`.

Two details are worth stealing. **Fractional scroll is rounded when the scale is
integral**: `if (float.IsInteger(renderScale)) fractionalOffset = (fractionalOffset *
renderScale).Round() / renderScale;` — sub-pixel camera offsets are quantised so
pixel edges stay sharp, but only when they would otherwise shimmer. And the
"pixel art scaling" itself is not nearest-neighbour but a **bilinear filter
whose interpolation happens in window coordinates**, implemented in
`combined.frag` from the two csantosbh articles cited in the source, with the
sharpness constant `ik = 1.43` and the comment *"set to 1/0.7 because it looks
good"*.

**The framebuffer-size clamp is the paragraph D.O.R.F. should have read:**

> **[SRC]** *"This approach does not scale well to large sizes, first saturating
> GPU fill rate and then crashing when reaching the framebuffer size limits
> (typically 16k). We therefore clamp the maximum framebuffer size to twice the
> window surface size… **Mods that use the depth buffer must instead limit their
> artwork resolution or maximum zoom-out levels.**"*

The clamp is skipped entirely when `depthMargin != 0`. **[inferred]** So a mod
that uses depth sprites gets an unclamped world buffer sized to its maximum
viewport — and D.O.R.F. uses depth everywhere, has sprites several times larger
than RA's, and has an outstanding request to double the zoom-out. That is the
same wall, named in the engine three years before the fork hit it.

### 4.3 One shader for almost everything

**[SRC]** `glsl/combined.vert` + `combined.frag` draw terrain, actors, effects,
UI and text. Per-vertex a single packed `uint` carries: primary channel
behaviour (unused / RGBA / paletted-from-R,G,B,A), secondary channel behaviour
(depth from R,G,B,A), two 3-bit sampler indices, and a 16-bit palette row.

- **Palette lookup is a fragment-stage dependent texture read**:
  `texture(Palette, vec2(dot(x, vChannelMask), vTexPalette))`. Team colour,
  faction colour, cloaking, and per-map tinting are all palette rows.
- **`ColorShift`** does an HSV range-and-shift for RGBA (non-paletted) art —
  the modern replacement for palette swapping.
- **Depth**: `gl_FragDepth = gl_FragCoord.z + DepthTextureScale * dot(y, vDepthMask)`.
- `EnableDepthPreview` renders the depth buffer as greyscale for debugging.

**[YAML]** How much of that depth machinery is actually used upstream is worth
stating precisely, because it changes how [`dorf.md`](dorf.md) §3.3 should be read:
`DepthSprite` appears **88 times across the shipped mods and every one of them
names the same file, `isodepth.shp`, only in the Tiberian Sun mod**, which also
sets `MapGrid: EnableDepthBuffer: True`. Upstream's depth feature is **one
shared generic isometric depth ramp** applied to buildings so units sort
correctly against them — not per-asset baked depth. The *mechanism* D.O.R.F.
uses is inherited; the *practice* of authoring real depth per asset is not.

### 4.4 Batching, and the two ways it breaks

**[SRC]** `SpriteRenderer` accumulates quads into one dynamic vertex buffer and
keeps up to **8 sheets** bound simultaneously. It flushes when:

```csharp
if (s.BlendMode != currentBlend || vertexCount + 4 > renderer.TempVertexBufferSize)
    Flush();
```

…and additionally when a ninth distinct sheet is needed. **[inferred]** Both
failure modes are structural, not tuning: renderables are submitted in depth
order, so additive effects interleave with alpha-blended units by *position* and
shatter the batch; and larger sprites mean fewer per sheet, so the 8-sampler
ceiling arrives sooner. This is the mechanism behind D.O.R.F.'s reported
explosion-heavy frame drops ([`dorf.md`](dorf.md) §3.4).

### 4.5 The sort, and how it avoids allocating

**[SRC]** `WorldRenderer.GenerateRenderables` collects renderables from
on-screen actors, the world actor, the render player, the order generator,
effects and annotations, then sorts by

```csharp
RenderableZPositionComparisonKey = r => r.Pos.Y + r.Pos.Z + r.ZOffset;
renderablesKeysBuffer[i] = ((long)key << 32) + i;
keys.Sort(CollectionsMarshal.AsSpan(renderablesBuffer));
```

— key in the high 32 bits, original index in the low 32, sorted as a `long[]`
that carries the renderables span along with it. **[inferred]** One primitive
sort, no comparator delegate, no allocation, and stability for free from the
index tiebreak. A small, very copyable pattern.

Then `PrepareRender` per renderable (the CPU-side work: voxel rasterisation,
palette resolution), and `Draw()` walks the prepared list. Draw order is fixed:
scissor → enable depth → terrain → flush → actors/effects → **clear depth** →
post-process(AfterActors) → `IRenderAboveWorld` → **clear depth** →
post-process(AfterWorld) → `IRenderShroud` → disable depth → overlays grouped by
type (*"HACK: Keep old grouping behaviour"*) → post-process(AfterShroud).

**[inferred]** The two mid-frame depth clears are the giveaway that the depth
buffer here is a *sorting device per layer*, not a scene-wide Z. Each band gets
a fresh depth range so a shroud quad cannot lose to a building's depth sprite.

### 4.6 Terrain lighting is CPU-side vertex tint

**[SRC]** `TerrainLighting` keeps sources in a `SpatiallyPartitioned<LightSource>`
(`BinSize = 10` cells) and `TerrainSpriteLayer.UpdateTint` samples `TintAt` at
the **four corners of each cell**, writing vertex colours *"to smooth out the
staircase effect"*. There is no normal, no per-pixel evaluation, and it is a
terrain-layer feature. It is enabled by the TS mod. That is the entire lighting
system D.O.R.F. replaced.

There is also a small **post-process pass framework** (`IRenderPostProcessPass`
at three insertion points) which the Cnc mod uses for the chronoshift and sonic
distortion effects, plus a separate `ModelRenderer` for TS/RA2-style voxels with
its own `model.vert/frag` — the one place OpenRA rasterises actual geometry.

---

## 5. Input

**[SRC]** `Sdl2Input.PumpInput` is called from `Renderer.EndFrame` — *after*
`Flush()` and immediately before `Context.Present()`. **[inferred]** So input is
sampled once per rendered frame, and input latency is coupled to the render rate
rather than the logic rate, which is why the loop's "force a render per logic
tick" rule matters for feel as well as for looks.

Three details worth having:

- **Motion coalescing with ordering preserved.** Mouse-motion events accumulate
  into a single `pendingMotion`, delivered once at the end of the pump — *but*
  any pending motion is flushed immediately before a button event is dispatched,
  so a click never arrives at a stale position. The naive version of this
  optimisation (drop all but the last motion) breaks drag-select; this one does
  not.
- **Multi-tap** (`MultiTapDetection`): per-button and per-(key, modifier)
  histories of the last three releases, with a tap counted as continuous if it
  is within **250 ms and 4 px**. Double *and* triple click/tap are first-class,
  and keyboard multi-tap is available for hotkeys.
- **HiDPI**: events arrive in surface coordinates on Windows/X11 and in window
  coordinates on macOS, and `EventPosition` normalises both, rounding fractional
  components away from zero *"to avoid rounding small deltas to 0"* — otherwise
  slow mouse movement on a scaled display stalls entirely.

Mouse buttons 4 and 5 are deliberately routed as *pseudo-keyboard* input so they
can be bound like keys.

From there: `DefaultInputHandler` → `Ui.HandleInput` (widget tree, in
`RunUnsynced`) → if unhandled, the world's `IOrderGenerator`.
**[SRC]** `UnitOrderGenerator.OrderForUnit` asks each selected actor's
`IIssueOrder` traits for candidate orders against the target, picks by priority,
and the result becomes an `Order` — which then goes on the wire and comes back
before anything happens. **Shift is read here**, at order-issue time, as the
`queued` flag.

**[inferred]** The full click-to-effect path is therefore: SDL poll → widget
tree → order generator → serialise → server → `+OrderLatency` frames → all
clients dequeue → `UnitOrders.ProcessOrder` → trait. At default speed that is
120 ms before your own unit acknowledges. Every RTS in this lineage feels the
way it does because of that number, and the "responsiveness" work in modern RTS
is almost entirely about hiding it — audio acknowledgement, cursor feedback and
selection highlights fire locally and immediately, because they are *not*
simulation.

---

## 6. Resource loading

### 6.1 The mount stack

**[SRC]** `FileSystem` is a list of mounted `IReadOnlyPackage`s plus a
`Cache<string, List<IReadOnlyPackage>>` file index. Resolution is
`fileIndex[filename].LastOrDefault(x => x.Contains(filename))` — **last mount
wins**, so override order is literally the order in the manifest. Re-mounting an
already-mounted package bumps it to the end (raising its priority) and
increments a refcount.

**[YAML]** The manifest syntax carries four sigils, all visible in
`mods/ra/mod.yaml`:

| Syntax | Meaning |
|---|---|
| `^EngineDir`, `^SupportDir` | Platform-resolved roots |
| `$ra` | Another *mod* mounted as a package |
| `name\|path` | Mount with an explicit name, addressable as `name\|file` |
| `~` prefix | Optional — failure to mount is swallowed |

And the ordering is commented in the data itself: content `.mix` archives are
mounted first, then *"mod-provided (system) packages that need to be loaded
after the content packages so they can override content assets"*.

`TryOpen` checks explicit mounts first (`ra|rules/vehicles.yaml`), then the
index, then falls back to asking every package in turn — with a `TODO` admitting
that last path is legacy. Packages are pluggable (`IPackageLoader`), with
`ZipFileLoader` always appended and mod-specific ones (`Mix`, `.pak`, …)
declared as `PackageFormats: Mix`.

`RequiredContentFiles` **[YAML]** lists individual files (24 of them for RA)
that prove the expansion content is installed — an integrity check expressed as
data, driving an in-game content installer.

### 6.2 Rules are yaml with inheritance and deletion

**[SRC]** `MiniYaml.Merge` folds every rules file in manifest order, resolving:

- `Inherits:` / `Inherits@tag:` — multiple inheritance from named templates,
  resolved recursively with cycle detection;
- keys prefixed `-` — **remove** an inherited node (`-Explodes:` deletes a trait
  a parent defined);
- everything else — deep merge, later wins.

**[inferred]** That triple (inherit / override / remove) is the minimum a
data-driven entity definition needs to avoid copy-paste, and the "remove" verb
is the one usually missing from home-grown formats. It is why a mod can retune a
shipped unit without editing the file that defines it.

### 6.3 Code is data too

**[SRC]** `ObjectCreator` loads the assemblies named in `Assemblies:`
(**[YAML]** RA: `OpenRA.Mods.Common.dll, OpenRA.Mods.Cnc.dll`) into the process,
caching by content hash, and resolves trait type names from yaml against them.
So a mod ships a `.dll` and the engine has no compile-time knowledge of it.

### 6.4 Sprites: reserve, then resolve in one pass

**[SRC]** `SpriteCache` is two-phase and the reason is packing quality.

- **`ReserveSprites(filename, frames, …)`** during sequence parsing returns a
  token. Nothing is loaded.
- **`LoadReservations`** then walks every reserved *file* once, decodes it,
  and pushes every needed frame into a `pendingResolve` list. Then:

> *"When the sheet builder is adding sprites, it reserves height for the tallest
> sprite seen along the row. We can achieve better sheet packing by keeping
> sprites with similar heights together."*

…so `pendingResolve` is sorted by frame height before packing, and identical
(file, frame, premultiplied, adjust) tuples are deduplicated into one sprite.
Reservation dictionaries are cleared and `TrimExcess`ed afterwards.

**[inferred]** A whole-program pass over declared needs beats incremental
loading here for exactly one reason — global knowledge produces a better shelf
packing — and it is only possible because sequences are *declarative data*. An
engine that loads sprites imperatively cannot do this.

### 6.5 The sheet builder's channel trick

**[SRC]** `SheetBuilder` is a shelf packer (`rowHeight`, 1 px margin, default
sheet **2048²**, fonts and cursors 512²). The clever part is `NextChannel`: for
**indexed** (8-bit paletted) art, when a sheet is full it does not allocate a new
texture — it moves to the **next colour channel of the same texture** and starts
packing again, R → G → B → A. Four sheets' worth of paletted sprites live in one
RGBA texture, which is why the shader carries a channel mask at all. Only when
alpha is exhausted is a new sheet allocated — and the old sheet's CPU-side
buffer is handed to the new one (`ReleaseBufferAndTryTransferTo`) to avoid a GC
allocation.

---

## 7. The trait system, briefly, because everything else hangs off it

**[SRC]** An actor is a name plus a bag of traits declared in yaml.
`ActorInfo.TraitsInConstructOrder()` resolves `Requires<T>` dependencies by
repeated relaxation — pull out everything with no unmet dependency, repeat until
nothing moves — and if anything is left, throws a `YamlException` that lists
both the missing traits and the unresolvable ones by name. Order is cached per
actor type.

`TraitDictionary` **[SRC]** is the query layer, and it is more considered than
"OOP with virtuals" suggests: for each interface type it keeps **two parallel
`List<>`s — actors and traits — sorted by `ActorID`**, with a custom
`BinarySearchMany` for insertion and lookup. So `ActorsWithTrait<T>()` is a
linear walk over a contiguous array and `TraitsImplementing<T>(actor)` is a
binary search. No dictionary hashing in the query path.

**[inferred]** This is worth noting against this project's own rule that the
entity layer stays ordinary OOP ([`CLAUDE.md`](../../../CLAUDE.md), "Where DOD belongs"):
OpenRA reaches the same conclusion and then buys back the *query* cost — which
is exactly where CLAUDE.md says the cost actually is — without becoming an
archetype ECS.

`ScreenMap` **[SRC]** is the screen-space index, and it makes a distinction
worth copying: it keeps **separate partitions for mouse bounds and renderable
bounds** of the same actors (`SpatiallyPartitioned<T>`, `BinSize = 250` px),
because a unit's clickable rectangle and its drawn rectangle are different
rectangles. Fog-of-war remembered actors (`FrozenActor`) get their own partition
**per player**.

---

## 8. What this says about the D.O.R.F. fork

Reading the two together sharpens several claims in [`dorf.md`](dorf.md):

1. **The depth-sprite socket is inherited but almost unused upstream** (§4.3) —
   one shared `isodepth.shp` ramp in one mod. D.O.R.F. authoring real per-asset
   depth is a genuine change of practice on a pre-existing mechanism.
2. **The framebuffer clamp (§4.2) predicts D.O.R.F.'s zoom-out problem
   exactly**, in an engine comment. A depth-using mod opts out of the clamp and
   must instead limit artwork resolution or zoom — which is the constraint
   D.O.R.F. keeps bumping into from the art side.
3. **The blend-mode flush (§4.4) is the named mechanism** behind their reported
   fire/explosion cost.
4. **"Units move at top speed instantly"** — confirmed by the absence of any
   acceleration in `World.Tick`/the mobile traits; D.O.R.F. adding acceleration
   and off-grid movement is a simulation change, not a tuning one.
5. **Netcode "largely neglected since the fork"** is a heavier statement than it
   sounds, given §3: the order model *is* the simulation's contract. A fork that
   changed unit movement, added multi-turret targeting and physics projectiles,
   and did not keep the sync/order layer in step, has been changing the very
   things `SyncHash` covers.
6. **Their Vulkan port has an easier job than it looks** — §4.1's context is
   already an interface (`IGraphicsContext`) behind a message queue, and the
   platform layer is 4,752 lines total. The hard part is not the API, it is
   §4.4's batching, which is where their measured problem is.

---

## 9. What transfers

**[inferred]** throughout, judged against cromwell.

**Take:**

1. **`RunUnsynced`** (§2) — a debug-build hash of simulation state asserted
   around every view-layer entry point. Cheap, catches an entire bug class, and
   needs no networking to be worth having.
2. **Latency as the tuned quantity, frames as the derived one** (§1) — the
   `Timestep`×`OrderLatency` table holding 120–160 ms across a 4× speed range.
3. **Back-pressure at the start of the frame, not the end** (§4.1) — `Post` the
   present, `Send` the clear. One frame of queue depth without an unbounded
   queue.
4. **The packed-key sort** (§4.5) — `(key << 32) | index` in a `long[]`, sorted
   alongside the payload span. Stable, allocation-free, no comparator.
5. **Reserve-then-resolve asset loading** (§6.4) and **height-sorted shelf
   packing**. Declarative asset declarations are what make the global pass
   possible; worth keeping that property when designing the material/asset
   format.
6. **Separate mouse and render bounds in the screen index** (§7).
7. **Motion coalescing that flushes before button events** (§5) — the correct
   version of an optimisation that is usually done wrongly.
8. **Inherit / override / remove in the data format** (§6.2) — especially
   *remove*.
9. **Median-based, clamped pacing of the fast participants** (§3.3) wherever a
   barrier sync exists.

**Do not take:**

- **Palette-indexed rendering** — a Westwood-compatibility artefact, and OpenRA
  itself is migrating to RGBA + `ColorShift`.
- **One übershader with a packed attribute bitfield** — right for 2D sprites
  where the whole material vocabulary is "which channel, which palette row",
  wrong for a PBR engine.
- **A single global sprite/blend sort per frame** — depth-order submission is
  what breaks batching; a 3D renderer should sort opaque by state and only
  transparent by depth.
- **Assemblies loaded from mod manifests** — a real security surface, accepted
  here because mods are the point.

---

## 10. What I did not read

`OpenRA.Mods.Common` (158k lines) beyond the specific traits named above —
pathfinding, the AI (`Bot` traits), Lua scripting, the map editor, and the
widget library are all unexamined here. Audio (`OpenAlSoundEngine`, 669 lines)
and font rendering (`FreeTypeFont`) were noted as present and not read. The
lobby/handshake state machine in `Server.cs` was read only where it touches
game start and order dispatch. Nothing was profiled or run — this is a source
read, not a measurement, and every performance claim above is a claim about
*structure*, marked **[inferred]** where it is mine.
