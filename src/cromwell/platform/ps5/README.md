# Porting cromwell to a console

This directory is empty on purpose. It is where a console platform's code goes,
and this file is the contract it has to satisfy.

`ps5` is a stand-in name — the same shape applies to any console. Sibling
directories per platform, never `#ifdef`s inside shared files: a console build
must not **contain** desktop code, not merely avoid calling it, and NDA headers
cannot be committed to a shared repository at all.

## What a port implements

Six interfaces, and one factory. Nothing else in the engine or the game changes.

| Interface | Header | What it owns |
|---|---|---|
| `ISurface` | `platform/ISurface.hpp` | the drawable surface, its size, its safe area |
| `IInput` | `input/IInput.hpp` | pads, and the system keyboard |
| `IClock` | `platform/IClock.hpp` | **already done** — see below |
| `IFileSystem` | `platform/IFileSystem.hpp` | content, save data, settings |
| `IImageDecoder` | `assets/IImageDecoder.hpp` | PNG/JPEG to RGBA8 |
| `rhi::IRenderDevice` | `rhi/IRenderDevice.hpp` | the graphics backend |

Then define `IPlatform::create` in exactly one file here. Two backends linked
together is a duplicate-symbol error, which is the right way to discover it.

`IClock` needs **no** implementation: `platform/FrameClock` is portable
(`std::chrono::steady_clock`) and is the frame clock for every target. Do not
write a console clock — the hitch clamp it carries is the rule whose absence
turns a suspend/resume into a unit teleporting through a wall, and a second copy
is a second chance to leave it out.

## Where the files go

```
platform/ps5/            code with no third-party library
platform/ps5/<sdk>/      code against a specific SDK
rhi/ps5/<api>/           the graphics backend
```

Two axes, platform first and library second — the same as `platform/pc/raylib/`
and `rhi/pc/opengl/`. Then add a block to `CMakeLists.txt` beside the `pc` one
setting `XC_PLATFORM_SOURCES`, `XC_PLATFORM_HEADLESS_SOURCES` and
`XC_RHI_SOURCES`, and build with `-DXC_PLATFORM=ps5`. An unknown platform is
already a `FATAL_ERROR` naming this list.

## How you know when it works

```
xcom.exe --device-selftest
```

`rhi::runRenderDeviceSelfTest` takes an `IRenderDevice`, not a concrete backend,
so it runs the identical sixteen stages against yours — resources, handle
generations, clears verified pixel by pixel, a depth-only pass, a shader, a
fullscreen draw. Work down the failures. It is a list, not a judgement.

The one part that will not run as written is the two shader stages: they are
GLSL string literals, and the shader cross-compilation question (SPIR-V as an
interchange, or a move to a language that targets all of them) is **not settled
yet**. Everything else is API-neutral and runs on day one.

## The things the interfaces already expect of you

These were designed in rather than bolted on, and they are the places a
desktop-first engine usually has to be reworked:

- **`ISurface::safeArea()`** — inset it. Desktop returns the whole surface, so
  every existing HUD call site already honours it at no cost.
- **`ISurface` capabilities** — `resizable`, `cursor`, `clipboard`, `title` may
  all be false. The matching setters must be safe no-ops, not errors; callers
  are not expected to branch on platform.
- **`ISurface::setVisible`** — a no-op is correct. The caller reveals after its
  first present either way.
- **`IInput` buttons are named by POSITION** (`FaceDown`, not `A`), because the
  printed glyph and the layout both differ by vendor. Map position to position.
- **`IInput::axis` returns RAW values.** Do not apply a dead zone; the right one
  differs per use and the game applies its own.
- **Text entry is asynchronous** — `beginTextEntry` raises the system keyboard,
  `textEntryState()` is polled, `Cancelled` is distinct from an empty string.
- **`IFileSystem` names storage by KIND, not by path.** Save data is your
  transactional container; `StorageKind::Asset` writes must fail.
- **`IPlatform::takeLifecycleChange`** — report `Suspending` and `Resumed`. The
  loop already calls `IClock::skipNextDelta` on resume.
- **`PassDesc` load/store actions** — honour them. On a tiler they are the
  difference between a pass costing its own work and also paying a full read and
  write of every attachment. The desktop backend ignores `Discard`; you should
  not.

## What is not ready yet

The renderer still draws through raylib's `rlgl` directly rather than through
`IRenderDevice` — roughly 350 call sites. Until that migration lands, a console
port can bring up the platform layer and pass the device self-test, but cannot
draw the game. That work is tracked as the next phase; nothing in this contract
changes when it completes.
