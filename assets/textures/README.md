# Effect textures

Standalone textures that are not material maps. A material map belongs in
`../materials/`, where the naming convention (`<kind>_albedo.png` and friends)
means a surface picks it up automatically. Nothing in here is auto-loaded —
each file is referenced explicitly by whatever consumes it.

The files themselves are gitignored (`**/assets/**/*.png`); this README is what
a fresh checkout gets, and it is meant to say what is supposed to be here.

## Contents

| file | size | consumed by | status |
|---|---|---|---|
| `caustics.png` | 256² RGB | *nothing yet* | **placeholder** |
| `cromwell.png` | 1920×1080 RGB | `SplashPass` | in use |
| `cromwell_water_mask.png` | 1920×1080 | `SplashMaps` | in use |
| `cromwell_water_depth_mask.png` | any | `SplashMaps` | in use |
| `cromwell_water_chop_mask.png` | any | `SplashMaps` | **optional**, off if absent |
| `cromwell_sky_mask.png` | 1920×1080 | `SplashMaps` | in use |
| `cromwell_sky_depth_mask.png` | any | `SplashMaps` | in use |

## `cromwell.png`

The splash screen: the cromwell engine wordmark over a moonlit Westminster
painting. Brought across from the cromwell Godot project (`E:\Repos\cromwell`,
`assets/cromwell.png`), which uses the same image for the same purpose.

Drawn cover-fitted behind the `SplashScreen` UI state, for the two seconds
`Application::kSplashMinimumSeconds` holds it, under `../shaders/splash.fs.glsl` —
which ripples the river, glitters the sun's track on it and hazes the light
around the silhouette. **Its absence is not an error** — like everything else
under `assets/`, it is gitignored, so a fresh checkout has no copy and the
front end falls back to the text splash it replaced. Drop the file in and the
image comes back with no rebuild.

The path is probed through `ShaderLibrary::rootContaining`, which lists the
game's asset roots ahead of the engine's — so a project-specific splash placed
here overrides an engine-wide one at `src/cromwell/assets/textures/`.

### Replacing it means re-measuring it

The shader is told where the water starts and where the sun is; it does not
find them. Those two numbers live in `SplashPass.hpp` (`kSunU`/`kSunV`,
`kWaterLine`) and they belong to *this* painting. A different image with the
same constants puts the glitter track in the sky.

Re-measure with the method recorded in that header — the sun as the brightest
**warm** patch with the wordmark masked out, and the waterline by eye off a
brightened crop. The wordmark is the trap: it is pure white and the brightest
thing in the frame, so an unmasked search finds the text every time.

To look at the result without watching two seconds go past:

    xcom --splash --no-steam                          sit on it and tune
    xcom --shot s.png --shot-frame 40 --splash        capture one frame

`--splash` exists only for that: it suppresses the timeout and makes a scripted
run open on the splash, which one otherwise skips. **F5 reloads the shader and
the water map** without restarting, so tuning is edit, save, look.

## The masks

**Five files, none of them individually required.** They tell the splash shader
where the river and the sky are and how far away each part of them is.
`SplashMaps` reads them at load, fills in whatever was not supplied, and packs
them into the two textures the shader samples.

**WHITE IS NEAR** in every depth mask, and that convention is the important one.
A river is nearest at the bottom of the frame; a sky is nearest overhead. The
shader takes the reciprocal to get distance, and distance is what makes motion
behave — near things cross the eye quickly and far things crawl. A depth mask
painted upside down does not look subtly wrong, it looks like the river is
flowing away from you at the wrong speed.

They are separate greyscale images rather than one packed RGB file because that
is how masks are actually painted — you paint white where the water is and save
it, and painting the next one is a separate job or no job at all. The packing is
arithmetic and belongs in code.

Without them the shader guesses, and the guesses are visibly worse: a straight
tilted line for the shoreline, which runs through the bridge and ripples the
moored barges, and a brightness threshold for the sky, which on this painting
cannot separate a hazy tower from the air behind it — measured, the silhouette
is 0.146 against a sky of 0.200, so no threshold divides them.

| file | what it says | if absent |
|---|---|---|
| `cromwell_water_mask.png` | **Is this water?** White where the river shows. | the tilted-line formula, for all three water masks |
| `cromwell_water_depth_mask.png` | **How near.** White at the near bank, black at the far end. | derived from the water mask |
| `cromwell_water_chop_mask.png` | **Chop** — extra agitation at piers, wakes, mid-channel. | calm everywhere |
| `cromwell_sky_mask.png` | **Is this sky?** White where the sky shows, cut around the silhouette. | a brightness guess, which barely works |
| `cromwell_sky_depth_mask.png` | **How near.** White overhead, black at the horizon. | derived from the sky mask |

Each coverage mask sets the resolution for its own group; the depth and chop
masks are resampled to match, so one painted at quarter size is fine — they are
smooth fields, not pictures. Coverage
is read as the **minimum of luminance and alpha**, so white-on-black and
white-on-transparent both work without saying which, and the brightest pixel
present is normalised to full — a mask whose white is 235 is not quietly scaling
every effect it gates by 0.92.

**The mask is what protects the painting.** Black over everything solid sitting
in or over the water: the bridge and its arches, the moored barges, the boats,
the wharf clutter. Those are the things that would otherwise ripple. A hard
brush edge is fine — the builder feathers it by about four pixels, because a
hard edge in a displacement mask is a seam with ripple on one side and still
paint on the other.

**The ramp is DEPTH, not direction.** Black is far and calm, white is near and
lively: it sets how large the waves are and how densely the surface sparkles,
and nothing else. Which way the river runs is a separate thing — `kFlowDir` at
the top of `splash.fs.glsl`, a screen-space direction pointing downstream, along
which the crests travel and across which they lie.

Those started out as one field and it did not work. A ramp that brightens
downwards can only produce waves that travel downwards, so a river running
across the frame was unexpressible no matter how the ramp was painted. Two
fields, one job each: **paint the ramp for distance, rotate `kFlowDir` for
direction.**

Keep the ramp smooth — noise in it becomes visible in the wave spacing.

**What the derived ramp cannot do**, when there is no painted one: it assumes
near is *down*, and takes each column's water extent from its topmost to its
bottommost water pixel, spanning the holes so crests run straight past a hull
rather than stepping around it. That is a fair approximation of a river receding
up the frame and wrong the moment one bends away from the viewer. Paint a flow
mask when the derived one is visibly wrong, not before.

### The loop

Paint, save, press **F5** in a running `xcom --splash --no-steam`. The masks are
rebuilt along with the shader, so a mask can be painted while watching it work,
and a file that did not exist at launch is picked up when it appears.

The wave scale in `splash.fs.glsl` (`swell`, the 26/17/10 coefficients) is tied
to `kFlowDir`, so **turning the river towards the vertical will want a second
look at them.** The displacement is vertical, so a river running across the
frame puts almost none of its phase change into the axis that stretches the
paint, and one running down the frame puts all of it there. Water tearing into
horizontal slabs means the coefficients are too big; a flat-looking river means
too small. The reasoning is written out where they are.

## `caustics.png`

A seamless underwater caustics tile — the bright filament network cast on a
surface by light refracting through a wavy water plane. Verified tileable:
opposite-edge difference is 8.4/6.2 against an interior-neighbour baseline of
9.5, so the seams are as continuous as any interior pixel pair.

**It has no consumer.** There is no water, ocean or underwater rendering in the
project — this is here ahead of the system that will use it, deliberately, as a
stand-in for stills.

### Two things to fix before it ships

**1. It is a single static frame, and caustics have to move.** A fixed tile
under moving water reads as a painted decal; the shimmer is most of the effect.
UNIGINE's equivalent (`core/textures/water_global/caustics.texture`) is a
512×512×**64** R8 volume whose Z axis is animation time, not depth — the water
shader samples `float3(uv.xy, water_time * caustics_animation_speed)` with a
default speed of 0.3, looping. Whatever consumes this will want an animated
loop, seamless in x, y **and** time, not one frame.

**2. Its provenance is unknown.** Downloaded, with no author, licence or
software tag in the PNG metadata. Fine for a local prototype — and it never
enters git either way — but it needs replacing with something whose origin is
known before any build leaves this machine. `study/README.md` records the rule
this project already follows for XCOM's `MovementBorder_Line`: reconstruct the
asset, do not ship someone else's.

Both are solved by the same job: generate the tile procedurally. Caustics
generate well — march a jittered wavefront through an animated Perlin surface
and accumulate where rays converge, which is the actual physics and produces the
brightened nodes at filament crossings for free.

### On brightness, when the time comes

This tile is authored to carry the look on its own: mean 32/255, median 16, so
there is an ambient floor everywhere and the filaments are wide and soft.

UNIGINE's is the opposite — mean 9.4, median **0**, thin blown-out filaments on
pure black — because it is *added* to ground that is already lit, scaled by
`caustic_brightness` (default 1.0, max 2.0) and a distance fade. A soft glowy
tile applied that way washes the surface out.

So the two are not interchangeable, and which one is right depends on whether
the caustics contribute the lighting or decorate it. Decide that first, then
author to match.
