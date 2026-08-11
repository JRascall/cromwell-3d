# Console porting — what the boundaries have to be, and what they are today

A design note, not research. The closest sibling in this directory is
[`nav_architecture.md`](nav_architecture.md): it reads the platform facts against
what `cromwell` actually contains and says what should be built, in what order,
and what should deliberately not be built yet.

> **Sourcing is weaker here than anywhere else in `study/`, and the reason is
> structural: every console SDK is behind an NDA.** There is no primary source to
> read, no shipped binary on this machine to disassemble, and no equivalent of
> the Source SDK to check a claim against. So the grading matters more than
> usual:
>
> **[PUBLIC]** — vendor documentation, published specifications, or press
> material anyone can read. Reliable.
> **[KHRONOS]** — the OpenGL / OpenGL ES / Vulkan specifications. Reliable.
> **[COMMUNITY]** — developer write-ups, conference talks by shipped titles,
> and porting-house public material. Useful for what actually hurts; weaker than
> a primary source.
> **[inferred]** — our reading of this codebase, or reasoning from public
> constraints. Not anybody's word but ours.
>
> **Nothing in this note is under NDA and nothing in it should be.** Where a
> platform detail is genuinely unavailable publicly, it says so rather than
> guessing. That is a real limitation: several cost estimates below are ranges,
> not numbers, and they will stay ranges until somebody signs something.

---

## 1. The one thing that matters most

**"Port to console" is not one project. It is three independent boundaries with
wildly different costs, and the expensive one is not the one people plan for.**

| Boundary | What it costs | When it must be decided |
|---|---|---|
| **Graphics** | months — a new backend | when the second backend is actually in hand |
| **UI** | weeks, or *twice the UI forever* | **before the game's UI is written** |
| **Networking** | weeks, or a full rewrite | **before the first netcode is written** |

Graphics is the biggest number and the *least* urgent, because nothing you do
now makes it much cheaper and nothing you do now makes it much worse. UI and
networking are the opposite: small if the boundary is drawn before the code
exists, and enormous afterwards, because the cost is not porting an
implementation — it is discovering you need a second one.

### 1.1 Containment is not abstraction

This distinction runs through the whole note, so it goes first.

[`cromwell/gpu/GL.hpp`](../src/cromwell/gpu/GL.hpp) is a good piece of design.
Its header states the rule plainly — *"if rlgl wraps it, call rlgl. If it does
not, declare it here with a comment saying which pass needs it. Do not include
glad.h elsewhere."* — and the rule is kept. Every raw GL entry point the
renderer uses is in one file.

**That makes the OpenGL dependency findable. It does not make it replaceable.**
A door with `glMemoryBarrier`, `glDispatchComputeIndirect` and
`GL_SHADER_STORAGE_BARRIER_BIT` written on it is a door to OpenGL specifically.
On a platform with no OpenGL, the door does not open onto a different room; there
is no room.

This is not a flaw. GL.hpp solved the problem it was built for — *"the second and
third exception would each have been argued exactly as well as the first, and
nobody would be counting"* — and solved it well. It is simply worth being clear
that a single owned door and a portable interface are different things, because
the file's existence can otherwise read as "graphics is already abstracted."

The same distinction applies elsewhere and in the other direction:
[`HttpClient.hpp`](../src/cromwell/net/HttpClient.hpp) *is* a portable interface
— it names no platform type at all, and its own header says a POSIX
implementation drops in beside WinHttpClient.cpp without touching a caller.
That is what the graphics layer does not yet have.

---

## 2. The platform facts

### 2.1 There is no OpenGL on any current console **[PUBLIC]**

| Platform | Graphics API | Shading language |
|---|---|---|
| PlayStation 5 | AGC (low-level), GNM | PSSL |
| Xbox Series X\|S | Direct3D 12 via the Microsoft GDK | HLSL |
| Nintendo Switch | NVN | GLSL-derived, compiled offline |
| Nintendo Switch 2 | NVN2; Vulkan also supported | offline-compiled |

Nintendo's platforms are the only ones that have ever offered a portable API:
the Switch SDK exposes Vulkan and OpenGL 4.5 alongside NVN **[PUBLIC]**, and
Switch 2 continues Vulkan support **[COMMUNITY]** — but GL on Switch is the
compatibility path, not the fast one, and shipping titles use NVN or Vulkan.
Sony and Microsoft have never offered GL at any point, on any generation.

**Consequence for this codebase:** the GL 4.3 context, the compute paths, the
`GL_TIME_ELAPSED` timer queries in [`GL.hpp`](../src/cromwell/gpu/GL.hpp), the
`samplerCubeArray` reflection probes and every `.glsl` file under
`src/cromwell/assets/shaders/` are desktop-only artefacts. Not one of them
survives a console port unchanged.

### 2.2 raylib has no console backend, and cannot publicly have one **[COMMUNITY]**

raylib ships backends for desktop GL 3.3/4.3, GLES 2/3 on Android, WebGL, and
DRM on Linux SBCs **[PUBLIC]**. There are no console backends in the public
repository and there structurally cannot be: publishing one would mean
publishing NDA'd SDK headers. Private console forks of raylib are known to
exist **[COMMUNITY]**, which means the option is "obtain or write an NDA'd fork
and maintain it" rather than "there is no path."

GL.hpp already prices the alternative, and the number is the codebase's own:

> *"The alternative was to drop raylib for raw GL. Measured at ~17k lines across
> 77 files."*

That measurement is the honest floor for replacing raylib wholesale. It has
grown since — `src/cromwell` is now ~15.2k lines and `src/game` ~17.2k
**[inferred]**, with 83 of 295 source files including raylib.

### 2.3 Runtime shader compilation does not exist on console **[PUBLIC]**

Consoles require shaders compiled offline into platform bytecode and packaged
with the title. There is no `glCompileShader` at runtime.

[`ShaderLibrary`](../src/cromwell/gpu/ShaderLibrary.hpp) reads GLSL from disk,
splices `#include` directives textually, and compiles at runtime — and F5
re-reads the whole tree without restarting, which is deliberate and good:

> *"Shading is the one part of a renderer that is tuned by LOOKING at it, and a
> rebuild-and-relaunch between every attempt is enough friction to stop anyone
> iterating properly — you settle for the third guess rather than the tenth."*

That workflow is correct and must not be sacrificed to a hypothetical port. The
resolution is an **offline path added beside it**, not a replacement: the splice
step already exists and already produces a single flat source per shader
(`preprocess()` is public precisely so failures can be dumped), so it is already
half of an offline pipeline. What is missing is a build step that runs the
splice, invokes a platform compiler, and packages the result — with the runtime
path kept for desktop development.

**This is genuinely cheap because of a decision already made.** A codebase that
generated shader source at runtime from string concatenation would have no
offline story at all.

### 2.4 There is no filesystem in the PC sense **[PUBLIC]**

Read-only package mounts, save data through a platform API with its own async
model and its own error cases, no arbitrary paths, and no writing next to the
executable.

Three things in this tree assume otherwise:

| Site | Assumption |
|---|---|
| [`ShaderLibrary::rootContaining`](../src/cromwell/gpu/ShaderLibrary.hpp) | probes `assets`, `../assets`, `../../assets` |
| [`Profiler`](../src/cromwell/diag/Profiler.cpp) | writes captures to `builds/win/profiles/` |
| [`Logger`](../src/cromwell/diag/Logger.cpp) | writes a log file beside the exe |

The last two are development tooling and simply do not ship, which is the
correct answer for both. **The total file-access surface is six call sites, three
of them dev-only** **[inferred]** — measured, not estimated. That is small
enough that the filesystem is a day's work and not a refactor, and the thing to
protect is that it stays small.

### 2.5 CEF cannot run on console or mobile **[PUBLIC]**

Chromium Embedded Framework builds for Windows, macOS and Linux. It needs a
multi-process architecture and a JIT, and there is no console build.

Today this costs nothing: the surface is pointed at `https://www.google.com`
from [`Application.cpp:455`](../src/game/Application.cpp#L455) and is a
demonstration, gated behind `XC_HAVE_WEB`, in its own CMake subdirectory
*"because CEF's cmake rewrites compiler and linker flags wholesale."* The
containment is already right.

**It stops being free the moment game UI lives there.** See §5.

### 2.6 The hardware floor is the Series S, not the PS5 **[PUBLIC]**

| | CPU | GPU | Memory available to games |
|---|---|---|---|
| PS5 | 8× Zen 2 @ 3.5 GHz | ~10.3 TFLOPS RDNA2 | ~12.5 GB of 16 GB |
| Xbox Series X | 8× Zen 2 @ 3.6/3.8 GHz | ~12.1 TFLOPS RDNA2 | ~13.5 GB of 16 GB |
| Xbox Series S | 8× Zen 2 @ 3.6 GHz | ~4 TFLOPS RDNA2 | ~8 GB of 10 GB |
| Switch 2 | ARM Cortex-A78C ×8 | NVIDIA Ampere-derived | ~9 GB of 12 GB **[COMMUNITY]** |

Two consequences that are not obvious:

**Per-core CPU is slower than a development machine.** Zen 2 at 3.5 GHz against
a modern desktop part is a meaningful single-thread deficit, and this simulation
is essentially single-threaded — `std::thread` appears in exactly two files,
[`SunBaker.cpp`](../src/game/light/SunBaker.cpp) and
[`SteamAvatar.cpp`](../src/cromwell/steam/SteamAvatar.cpp) **[inferred]**. The
perf discipline in CLAUDE.md is the right discipline; console just means the
frame budget is tighter than the machine it was tuned on.

**Parity certification is per-SKU.** A title that ships on Xbox Series X must
also run acceptably on Series S. The Series S is therefore the design target,
and it is roughly a third of the Series X's GPU with two thirds of its memory.

### 2.7 Certification is a real work item **[PUBLIC]**

Sony TRC, Microsoft XR, Nintendo Lotcheck. The categories that generate actual
engineering, as opposed to paperwork:

- **Suspend and resume.** The title is frozen and thawed at arbitrary points,
  including mid-frame and mid-load. GPU resources may need recreating.
- **Controller disconnect and reconnect**, with a mandated dialogue.
- **User account switching** mid-session, including sign-out during play.
- **Safe area and TV overscan** for every piece of UI.
- **No mouse cursor**, anywhere, ever.
- **Load-time limits** on some platforms.

None of these are hard individually. All of them are discovered late by teams
that treated the port as a graphics problem.

---

## 3. What this codebase has today

Measured, not estimated. Every number below came from the tree.

| Boundary | State | Where |
|---|---|---|
| Math types | **abstracted** | own `Vec2`/`Vec3`/`Quat`; [`RaylibInterop.hpp`](../src/cromwell/math/RaylibInterop.hpp) is the only meeting point, and the layouts are `static_assert`ed |
| HTTP | **abstracted** | [`HttpClient.hpp`](../src/cromwell/net/HttpClient.hpp) names no platform type |
| Steam | **contained** | one `.cpp`, `XC_HAVE_STEAM`, stubs when the SDK is absent |
| CEF | **contained** | `XC_HAVE_WEB`, own subdirectory, own CMake scope |
| Raw GL | **contained, not abstracted** | [`GL.hpp`](../src/cromwell/gpu/GL.hpp) — see §1.1 |
| GPU resources | **not abstracted** | **35 of 81** engine headers include `raylib.h` |
| Window / input | **not abstracted** | raylib owns the window; [`FrameInput.hpp`](../src/cromwell/input/FrameInput.hpp) includes `raylib.h` for one `Vector2` |
| UI | **designed, not compiled** | `src/cromwell/ui/` — absent from CMakeLists.txt, nothing includes it |
| Game networking | **does not exist** | the boundary can be drawn first |
| Audio | **does not exist** | same |
| Filesystem | **contained by accident** | 6 call sites, 3 dev-only |

### 3.1 The headline number, and why it is better than it looks

**83 of 295 source files include raylib** — so 72% of the tree is already
platform-agnostic C++ **[inferred]**. That is not luck. It is the direct result
of the rule in CMakeLists.txt:

> *"cromwell may not include, link against, or know the name of anything under
> src/game."*

plus the headless split that puts the entire simulation in `game_core` and
`cromwell_base` with no raylib dependency at all — lattice, occupancy,
pathfinding, LOS, visibility, cover, destruction, the UI state machine.

**The rule was written to make the engine liftable into the next project.
Portability is a second, independent payoff from the same rule**, which makes it
worth more than it looked when it was written. It also means the discipline that
protects the port is already in place and already enforced by
`cmake --build . --target cromwell`.

### 3.2 The shape of the graphics coupling

The 35 headers that expose raylib are not scattered arbitrarily. They are almost
entirely **GPU resource holders**, and they expose the same five types:

```
Shader          ShaderLibrary, PbrShader, PrepassShader, OverlayShader,
                RibbonShader, SkyPass, ToneMapPass, AmbientOcclusion
Texture2D       MaterialLibrary, PbrMaterial, TexturePreviews
RenderTexture   HdrTarget, DepthTarget, GBuffer, CustomDepthStencil, ShadowMap
Mesh            BoxMesh, MeshVertexBuffer, SurfaceVertex, StripSet
Model           ModelAsset, ModelPreview
```

**One shape, repeated.** That is what makes §4's approach viable: it is a
mechanical transformation applied many times, not 35 separate design problems.

---

## 4. The graphics boundary

### 4.1 Do not write an RHI yet

The instinct on reading §2.1 is to design a rendering hardware interface now, so
that a console backend can slot in later. **That is the wrong move, and the
reason is specific rather than a general caution against abstraction.**

An RHI is a set of guesses about what every backend needs in common. Written
against one backend, every guess that matters is unverified — and the guesses
that turn out wrong are exactly the ones in the areas where APIs genuinely
differ: resource binding models, command buffer lifetime, synchronisation,
memory residency, descriptor sets. Those are precisely the parts you cannot
reason about from GL alone.

The result is an abstraction that must be redesigned when the second backend
arrives, having cost a large refactor and bought nothing. This is the same
failure the codebase already documents in a different domain — the `ReachField`
scratch wipe that *"looked like an obvious win"* and measured at 4%. Build the
thing the measurement asks for, not the thing the shape of the problem suggests.

### 4.2 Do this instead: opaque handles

**Generalise the pattern the codebase already uses for maths.**

[`RaylibInterop.hpp`](../src/cromwell/math/RaylibInterop.hpp) solves exactly this
problem one layer down. cromwell owns `Vec3`; raylib's `Vector3` appears in one
file; conversion is free and the layouts are asserted. Its header states the
principle in a sentence:

> *"So the engine owns its types and converts at the boundary."*

Apply it to GPU resources. cromwell declares its own handle types —
`ShaderHandle`, `TextureHandle`, `RenderTargetHandle`, `MeshHandle` — and those
appear in the headers. raylib's `Shader` and `Texture2D` live in the `.cpp`
behind them.

**Why this is not the RHI trap:**

| Writing an RHI | Introducing handles |
|---|---|
| Guesses what backends have in common | Guesses nothing |
| Must be designed before it can be used | Mechanical, one class at a time |
| Wrong in the places that matter | Cannot be wrong — a handle is a handle everywhere |
| One big refactor | Incremental, independently verifiable |
| Bought only when a backend arrives | Pays off immediately: engine headers stop dragging in `raylib.h` |

A handle is an integer or an opaque struct. Vulkan, D3D12, NVN and AGC all
address their resources through handles of some kind, so the concept does not
need to be guessed. **You are not abstracting graphics. You are removing raylib's
type names from your interfaces**, which is a strictly smaller and entirely
verifiable job.

The RHI, when it eventually arrives, slots in behind handles that already exist
— and by then it will be designed against two real backends instead of one
imagined one.

### 4.3 The immediate win, and it is nearly free

[`FrameInput.hpp`](../src/cromwell/input/FrameInput.hpp) includes `raylib.h` for
exactly one reason: two `Vector2` members, `mouseDelta` and `mousePosition`.

Swapping those for cromwell's own `Vec2` removes `raylib.h` from a header that
much of the game includes, and makes the entire input boundary platform-free.
The struct is already correct in every other respect — its header says so:

> *"carry input as data, so Application decides what it MEANS without ever
> calling IsKeyPressed itself."*

That is exactly the right design, and it is also the gamepad seam: adding stick
axes and button actions to a data struct is additive, whereas adding them to
code that calls `IsKeyPressed` inline is not.

**One breach to close alongside it.**
[`PlayerController.cpp:247`](../src/game/controllers/PlayerController.cpp#L247)
calls `GetMousePosition()` and `GetScreenToWorldRay()` directly instead of
reading from `FrameInput`. It is the only leak of its kind and worth closing
next time that file is open — not as a special trip.

### 4.4 Order of attack

Leaves first, hub last, so each step is small and independently checkable.

1. `FrameInput` → `Vec2`, drop `raylib.h`. Minutes.
2. `Shader`, starting at [`ShaderLibrary`](../src/cromwell/gpu/ShaderLibrary.hpp)
   — the hub every pass gets its shaders from, so one conversion reaches eight
   headers.
3. Render targets — `HdrTarget`, `DepthTarget`, `GBuffer`, `CustomDepthStencil`,
   `ShadowMap`. These are already thin wrappers over a raylib type, which is the
   easiest possible case.
4. `Texture2D`, via `MaterialLibrary`.
5. `Mesh` and `Model` last. They are the most entangled with raylib's asset
   loading, and the least urgent, because model loading is the one area where
   raylib is doing genuinely useful work that would have to be replaced rather
   than merely re-typed.

**Steps 2–5 are worth doing gradually rather than as a project.** Each one
independently makes the engine more liftable, which is a benefit the codebase
already values for its own sake.

### 4.5 When it becomes real: Vulkan first, on Windows

The first genuine step toward a console is not a console.

**Write a Vulkan backend on Windows.** No NDA, no dev kit, no approved-developer
status, and — critically — it can be verified against the existing GL path on
the same machine, frame by frame. It is also the point at which the RHI seam
gets designed, with a second real backend in hand, which per §4.1 is the only
way that design comes out right.

Vulkan is additionally the closest public API to what Switch 2 wants natively and
to how PS5 and D3D12 are structured conceptually — explicit synchronisation,
command buffers, descriptor sets, explicit memory. A team that has moved this
renderer to Vulkan has already solved most of the conceptual work of a console
port; what remains is API translation rather than architecture. And it doubles
as the Android path if mobile ever matters.

---

## 5. The UI boundary — already designed, not switched on

**This is the most important finding in the note, and it is good news.**

`src/cromwell/ui/` contains `UiDrawList`, `UiContext`, `UiText`, `UiTheme`,
`UiColor`, `HoverFade`, `Glow` and `Outline`. **It is not referenced in
CMakeLists.txt, and nothing outside the tree includes it** **[inferred]** — five
`.cpp` files that are not compiled.

Its architecture is precisely the boundary this note would otherwise have to
argue for. From [`UiDrawList.hpp`](../src/cromwell/ui/core/UiDrawList.hpp):

> *"accumulate the triangles and text runs the widgets produce, in the order they
> were produced, with the clip rectangle each was produced under. It draws
> nothing and knows nothing about GL."*

and the first of its three stated reasons:

> *"It keeps every widget in cromwell_base. A widget that called rlBegin/rlVertex
> would need raylib, and the whole kit would move to the renderer's side of the
> engine where nothing can be tested without a window."*

**Consequence: swapping the graphics backend costs one new painter and touches
zero widgets.** The widgets emit `UiVertex` and `TextRun` into a buffer; whatever
consumes that buffer is the only platform-aware part. That is the cleanest
boundary in the codebase, and it was designed for testability rather than for
portability — the same coincidence as §3.1, where a rule written for engine
liftability turns out to protect the port.

It is also worth noting that this is the architecture Dear ImGui itself uses —
ImGui produces draw lists and knows nothing about any backend, which is why it
runs on every platform including consoles **[PUBLIC]**. `cromwell/ui` arriving at
the same shape independently is a good sign about the shape.

### 5.1 The one UI decision that is expensive to reverse

**Do not build the game's UI on CEF.**

If the real UI lives in Chromium, every non-desktop target needs a second, full
UI implementation — not a port, a rewrite, in a different language, with
different layout, different input handling and different assets. That cost never
goes away and it is paid on every screen.

If `cromwell/ui` is the real UI, CEF stays what it is today: an optional
development surface behind `XC_HAVE_WEB` that simply does not build for console.
The problem disappears rather than being solved.

Same rule for ImGui, for a different reason: it is portable, but it is a
*developer* tool, mouse-driven and dense. It belongs to the F1 dev panel and
should never carry player-facing UI. It is already correctly scoped — CMakeLists
deliberately does not link it into `cromwell`:

> *"imgui is deliberately NOT here — the dev panel is the game's, and an engine
> that drags a UI toolkit into every project that embeds it has made a choice on
> that project's behalf."*

### 5.2 Open question

`cromwell/ui` being uncompiled is either paused work or abandoned work, and the
note cannot tell which. **If it lives, it should be wired into CMakeLists and
given tests** — a draw list is assertable without a window, which is the whole
point of the design, and untested uncompiled code rots. If it is abandoned, the
UI boundary needs re-deciding from scratch, and §5.1 becomes urgent rather than
theoretical.

---

## 6. The networking boundary — draw it before writing the code

[`net/`](../src/cromwell/net/) is two files. The real networking is entirely
ahead, which is the best possible position: **the boundary costs nothing now and
a rewrite later.**

### 6.1 Transport and matchmaking are two interfaces, not one

This is the whole recommendation, and it is one sentence:

> **Steam's connection/message model ports to console. Steam's lobby and
> matchmaking model ports to nothing.**

| | Steam | PSN / Xbox Live / NSO |
|---|---|---|
| Connections, reliable/unreliable messages, per-connection state | close enough to map | close enough to map |
| Lobbies, matchmaking, invites, presence, sessions | Steam-specific | entirely platform-mandated, entirely different |

If both live behind one `SteamNetworking` facade, a console port rewrites both.
Behind two interfaces, it rewrites one — and the one it rewrites is the smaller,
less performance-sensitive half.

**Steam must be an implementation of the transport interface, not the thing that
defines its shape.** The difference shows up in whether the interface's
vocabulary is Steam's (`CSteamID`, `SteamNetworkingIdentity`, lobby handles) or
the engine's.

### 6.2 The transport choice already helps

[`valve_networking.md`](valve_networking.md) §6 reaches a conclusion that is
directly relevant here: **GameNetworkingSockets is the one layer Valve
open-sourced** — take it. It is BSD-licensed, it is the same transport that sits
under Steam, and it builds outside the Steamworks SDK.

That matters for portability specifically: GNS gives the connection/message
semantics of §6.1's left column **without** dragging in Steam's account,
matchmaking and presence layers. It is the transport interface's first
implementation and it already runs on more platforms than Steamworks does.

### 6.3 What is genuinely hard, and is not a boundary problem

Reconciliation, lag compensation and the pose-recording problem
([`networked_animation_physics.md`](networked_animation_physics.md) §2.3) are
platform-independent. They are hard, but a console port does not make them
harder. Do not conflate "networking is difficult" with "networking is a porting
risk" — only the platform service layer is a porting risk, and it is the shallow
part.

---

## 7. The parts that are not code

### 7.1 Input is mouse-only by design, not just by implementation

`FrameInput` being data is right, and §4.3 makes it raylib-free cheaply. But the
*verbs* are mouse verbs: orbit-drag, wheel zoom, pointer picking, click-to-move,
hover-to-preview.

**The grid is the thing that saves this.** A tile lattice is inherently
gamepad-friendly — a d-pad or stick steps a cursor one tile at a time, and there
is no free-cursor precision problem. Console XCOM shipped exactly this
**[COMMUNITY]**. The things that would genuinely paint you into a corner are
free-aim, drag-box selection, and pixel-precise hover targets, none of which
this design currently needs.

**So the rule is not "add gamepad support now."** It is: do not introduce
interactions that a stick cannot express. That costs nothing and preserves the
option.

The dev tools are a separate matter and need no consideration at all — F1, F5,
F9 and the placement tools do not ship.

### 7.2 Threading

Two `std::thread` users today **[inferred]**. Consoles offer eight cores of
moderate per-core speed, which is a different shape from a fast desktop part:
work that is fine single-threaded on a development machine may not be.

**This is a measurement question, not a design question**, and CLAUDE.md already
says how to handle it — the profiler zones exist, the benchmark exists, and the
answer arrives when the frame is actually over budget on the actual hardware.
Nothing to do now beyond continuing to instrument new systems.

### 7.3 Third-party dependencies, graded for portability

| Library | Console outlook |
|---|---|
| **Jolt** (physics) | excellent — ships in PS5 titles **[COMMUNITY]**, no platform dependencies |
| **ozz-animation** | excellent — pure C++, no platform dependencies |
| **nlohmann/json**, MessagePack | fine — pure C++ |
| **GameNetworkingSockets** | good — see §6.2 |
| **meshoptimizer** | fine — pure C++, and offline anyway |
| **Dear ImGui** | portable, but dev-only here — see §5.1 |
| **miniaudio** | backends are desktop and mobile; console audio needs platform backends **[PUBLIC]** |
| **Steam Audio** | desktop-oriented; needs checking before it is committed to |
| **Steamworks** | Windows/macOS/Linux only — already contained |
| **CEF** | desktop only — see §2.5 |
| **raylib** | see §2.2 — the blocker |

The pattern is worth naming: **the libraries chosen for this project are mostly
portable, and the two that are not are already behind compile-time flags.** The
one genuine problem is raylib, which is also the one that was chosen first and
for the best reasons.

### 7.4 Audio has not been written, so it gets the §6 treatment

Same argument, same cost profile. If audio arrives behind an interface that names
no library type, miniaudio is one implementation and a console backend is
another. If it arrives as miniaudio calls sprinkled through the game, it is a
rewrite. **Cost of deciding now: zero.**

---

## 8. Build order

Grouped by whether the work pays off regardless of whether a console ever
happens. Everything in the first group does.

### Worth doing anyway

| | Work | Cost |
|---|---|---|
| 1 | `FrameInput` → `Vec2`; close the `PlayerController` leak | minutes |
| 2 | Decide `cromwell/ui`'s fate; wire it in and test it if it lives | small |
| 3 | Split transport from matchmaking *before* netcode exists | design only |
| 4 | Audio behind an interface *before* miniaudio is wired in | design only |
| 5 | Opaque handles, `Shader` first via `ShaderLibrary` | incremental |
| 6 | Handles for render targets, then textures, then meshes | incremental |

Items 1–4 are cheap enough that the console justification is almost incidental —
each is defensible purely as engine hygiene, and 3 and 4 are free because the
code does not exist yet.

### Only when a console is real

| | Work | Cost |
|---|---|---|
| 7 | Vulkan backend on Windows, verified against GL | months |
| 8 | Offline shader compilation beside the runtime path | weeks |
| 9 | Filesystem and save data behind an interface | days |
| 10 | Gamepad input scheme and UI safe-area pass | weeks — design work |
| 11 | Dev kit, approved-developer status, NDA'd backend | procurement, then months |
| 12 | Certification pass — §2.7 | weeks, and always longer than planned |

Step 7 is the real project. Steps 11–12 are frequently outsourced to a porting
house **[COMMUNITY]**, which changes what matters: a partner needs a codebase
they can build and read without you. The comment discipline in this tree and the
`cromwell`/`game` split are unusually strong for that — a porting partner can be
handed a boundary rather than a ball of mud.

---

## 9. What not to do

- **Do not write an RHI before a second backend exists.** §4.1.
- **Do not hobble the renderer to stay portable.** `samplerCubeArray` and the GL
  4.3 context are the right call for what they do. Avoiding GL 4.x features to
  protect a hypothetical port trades certain quality now for uncertain effort
  later. The reflection probes are not why the port is hard; raylib is.
- **Do not pre-build the offline shader compiler.** F5 hot-reload is a genuinely
  good workflow and losing it early solves nothing.
- **Do not build gamepad input now.** Just do not design interactions that
  exclude it. §7.1.
- **Do not put game UI in CEF.** §5.1. This is the only irreversible one.
- **Do not treat "port to console" as a graphics task.** §2.7 is where ports
  actually slip.

---

## 10. Weakest parts of this note

Stated plainly, in the spirit of [`map_scale.md`](map_scale.md)'s own warning
about Eugen.

1. **Every console-side claim is from public material.** No SDK has been read.
   The API names in §2.1 are public knowledge; nothing about their actual shape,
   ergonomics or performance characteristics here is first-hand, and the effort
   estimates in §8 are therefore ranges rather than numbers.
2. **§2.2's private-raylib-fork claim is [COMMUNITY].** That such forks exist is
   reported rather than verified, and their quality and availability are unknown.
   If that path turns out to be closed, §4.5's Vulkan backend stops being the
   recommended first step and becomes the only step.
3. **Switch 2 figures are [COMMUNITY]** and should be re-checked before any
   decision leans on them.
4. **§5's read of `cromwell/ui` is a read of headers, not of working code.** It
   is not compiled, so nothing here has been run. The architecture is right; that
   it *works* is untested by construction.
5. **Nothing here has been costed against an actual dev kit**, which is the only
   thing that would turn §8's second table from a plan into a schedule.

---

## 11. The summary in five lines

1. **72% of the tree is already platform-agnostic**, because a rule written for
   engine liftability happens to protect portability too.
2. **The graphics coupling is one shape repeated 35 times** — GPU resource types
   in headers — and the fix is the handle pattern the codebase already uses for
   maths, not an RHI.
3. **The UI boundary is already designed and not switched on.** Decide its fate;
   it is the cleanest seam in the codebase.
4. **Networking and audio do not exist yet, which is the cheapest moment to draw
   their boundaries** — transport apart from matchmaking, interface before
   library.
5. **The only irreversible decision is CEF.** Everything else is a cost; that one
   is a permanent duplicate.
