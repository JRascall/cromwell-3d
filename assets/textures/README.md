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
