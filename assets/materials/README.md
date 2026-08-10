# Material textures

Drop texture files in this directory and the matching surface picks them up on
the next run. Nothing else needs to change — no code, no shader, no rebuild.

## Naming

    <kind>_albedo.png     sRGB base colour
    <kind>_normal.png     tangent space normal, OpenGL handedness (green = up)
    <kind>_mrao.png       metalness in R, roughness in G, ambient occlusion in B

`<kind>` is any material name. The built-in ones, created at startup, are:

    floor  road  grass  wall  window  cover  ramp  block  canopy  portal
    ladder  body

Materials are named, not enumerated — a model registers its own under
`<model>#<index>` (see `../models/README.md`), and any other name can be
created on demand.

Every file is optional, individually. A missing albedo or mrao falls back to a
shared 1×1 white; a missing normal falls back to a 1×1 flat normal. So a
surface can be migrated one map at a time, and a kind with no files at all
renders exactly as the untextured placeholder does today.

## How the maps combine

Each scalar in `PbrMaterial` **multiplies** its map, the way glTF defines it:

    metalness = mrao.r × metalnessFactor
    roughness = mrao.g × roughnessFactor
    occlusion = mrao.b × (screen-space SSAO)
    albedo    = albedo texture × vertex colour × material tint

That is why an untextured material is still fully described — with a white
mrao, the factors *are* the values. It is also why authored maps should be
neutral-ish: a roughness map that already bakes in the surface's overall
gloss will double up against the factor. Set the factor to 1.0 for such a map,
or keep the map relative and tune the factor.

`metalness` is close to a binary in a metal/rough workflow. A surface either
conducts or it does not; values in between are for transitions like rust or
worn paint, not for "a bit shiny".

## Channel packing

Files named `_mrao.png` are read as **MRAO** — metalness R, roughness G,
occlusion B, which is Valve's `$mraotexture` convention.

Textures adopted from a **glTF** model are **ORM** instead — occlusion R,
roughness G, metalness B. The shader swizzles on a per-material flag rather
than requiring assets be repacked on import.

Roughness is green in both, which is precisely why getting this wrong survives
a casual look: the surface roughness comes out right and only the metalness is
inverted.

## Transparency

Materials are **opaque by default, and opaque ignores the albedo's alpha
channel outright.** That channel is free real estate and engines fill it with
whatever they like — UE3 assets routinely keep a specular mask there, and read
as coverage it turns solid geometry into a ghost.

Transparency is something a material declares (`AlphaMode::Opaque` / `Mask` /
`Blend`, with a cutoff), the same way glTF's `alphaMode` works.

## Colour space

**Albedo is the only map decoded from sRGB.** Normal and mrao are data, not
colour, and are sampled raw. Authoring a normal map as sRGB — or letting a
tool tag it that way — produces lighting that is subtly wrong everywhere and
very hard to trace back.

## Density and tiling

UVs on the generated box geometry are a **world-space planar projection**: each
face takes its two in-plane world axes directly as (u, v), so a texture runs
continuously across abutting tiles instead of restarting at every box. One
world unit is one tile (96uu, ~1.5 m).

`PbrMaterial::uvScale` sets repeats per tile and is applied in the shader, so
retiling a surface costs no rebake. Textures are loaded with mipmaps, trilinear
filtering and 8× anisotropy, and wrap on both axes — so they must tile
seamlessly.

Authored meshes bring their own UVs and tangents and bypass the projection
entirely; the vertex layout is the same either way.

## What is still placeholder

The world's *colour* currently comes from vertex colours (see `Palette.hpp`),
not from albedo maps. Once a kind has a real albedo texture, its palette entry
becomes a tint — set it to white if you want the texture alone.
