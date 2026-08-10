# Study

Notes on how the games this prototype borrows from draw what they draw, written
for understanding the system rather than for dropping into an engine.

| File | What it explains |
|------|------------------|
| [`xcom2_movement_border.hlsl`](xcom2_movement_border.hlsl) | The **movement-coverage ribbon** material: how the band is coloured, how it survives being drawn with no depth test, and how it hides itself above the storey you are looking at. |
| [`source2_rendering.md`](source2_rendering.md) | How **Source 2** (Half-Life: Alyx, CS2) lights a scene, what this renderer has of it, and what it is still missing. Includes the post-mortem on the shelved sun bake and the ranked plan for closing the gap. |
| [`sea_of_thieves_water.md`](sea_of_thieves_water.md) | How **Sea of Thieves** renders its ocean — FFT waves, the fake subsurface scattering, and the foam system — read against CS2's water, and which parts are worth taking. **Also holds the UNIGINE ocean** (§11): Gerstner rather than FFT, read from the SDK's shader source, plus its decoded texture set. |
| [`rdr2_atmospherics.md`](rdr2_atmospherics.md) | How **Red Dead Redemption 2** renders clouds, fog, god rays and sky ambient as *one* participating-medium system — froxel volume near, raymarch far. The detailed blueprint for the froxel system `source2_rendering.md` §13 says this project needs, with formats, resolutions and PS4 costs. |
| [`unigine_clouds.md`](unigine_clouds.md) | How **UNIGINE 2** renders volumetric clouds — coverage × height gradient × noise erosion, an adaptive march that rewinds when it overshoots, and 1-ray-in-N reconstruction. Read from the SDK's own shader source rather than a talk, which makes it the working counterpart to `rdr2_atmospherics.md` §5. |
| [`navigation.md`](navigation.md) | How **Unreal**, **Source 2**, **Left 4 Dead** and **World War Z** answer "where can I walk" and "what is near me" — why those are two systems and not one, where Unreal's navmesh stops scaling (agent count, not world size), why Epic built ZoneGraph instead, and the blueprint for a **swarm** at WWZ scale: coarse mesh, one flow field per destination, steering over the spatial hash. Includes what the GPU really buys (100k agents — simulation only) and measured cell-size and crowd numbers from `xcom_perf`. The one study note about the **simulation** rather than the renderer. |
| [`crowd_scale.md`](crowd_scale.md) | **Assassin's Creed Unity**, **World War Z** and **Left 4 Dead** — three answers to "far more characters than you can afford". AC Unity's 40 real AI behind 10,000 bodies and its 300-vs-11 bone split; WWZ's mass-physics horde; L4D's Director, Flow Distance and Active Area Set (from Valve's own paper). All three built the same architecture: a small expensive set near the player, a large cheap one elsewhere, and machinery to hide the swap. |
| [`battle_scale.md`](battle_scale.md) | **Total War** and **Men of War** — the opposite bet, where every soldier is real. Total War's "logic generates the future whilst the display renders the now", queued projectile collision, matched-pair melee, and the tiled 24-layer terrain renderer; GEM's per-soldier inventory, angle-based armour penetration and destructible terrain. The trade is **count × depth ≈ constant**. |
| [`map_scale.md`](map_scale.md) | **R.U.S.E.** and **Wargame** on Eugen's **IRISZOOM** — the problem that is *extent*, not count. 150 km² maps at ~1000x lower density than a Total War field, seamless strategic-to-ground zoom, and the optics-versus-stealth spotting contest. Why a sparse spatial index suits this and a dense grid does not. **Weakest sourcing in this directory — Eugen have published nothing, so §2 is reasoning from observable constraints and says so.** |
| [`re_engine_rendering.md`](re_engine_rendering.md) | How **Capcom's RE ENGINE** is built around caches and cheap cache repair — the shadow cache, the tetrahedral probe network, relit cubemaps, and the signed distance field that turns out to be far cheaper for a voxel world than it was for Capcom. Ten years of their own CEDEC and Open Conference decks, with costs. |

## Where the XCOM 2 material came from

Unlike From Dust, none of this is guesswork from a disassembly. UE3 ships its materials as
serialised node graphs inside the package, so the graph *is* recoverable — the SDK's packages
are uncompressed and the name, import and export tables read straight out:

```
XCOM 2 SDK/XComGame/Content/XCOM_2/Packages/UI/UI_3D.upk
  -> Tile.MovementBorder                      (UE3 package version 845, licensee 105)
```

which is the material the game is told to use, in
`XCOM 2 SDK/XComGame/Config/XComEngine.ini`:

```
MovementGridMaterialName=UI_3D.Tile.MovementBorder
MovementGridDotsMaterialName=UI_3D.Tile.MovementTile
```

Every constant, connection and material flag in the HLSL is transcribed from the
`MaterialExpression` objects under that material. The two `MaterialExpressionCustom` bodies are
literal — they are the only hand-written HLSL that ships inside a UE3 material, and they are
quoted exactly:

```hlsl
return 1.0-saturate((WorldPos.z - (FadeHeight-FadeDistance))/ FadeDistance);   // "Height Fading"
```

Node defaults that the package leaves unserialised were read from the SDK's own UnrealScript,
`Development/SrcOrig/Engine/Classes/MaterialExpression*.uc` — which matters more than it sounds:
`MaterialExpressionConstantScale` defaults its `Scale` to **0.5**, not 1.

Two more sources fill in the parts the material does not own:

- `XComGame/Config/XComGame.ini`, `[XComGame.XComMovementGridComponent]` — the ribbon's width,
  height offset, length factor and UV tiling distance. These shape the mesh, and the material's
  UVs only mean anything alongside them.
- `XComEngineMaterials.upk` → `MovementGrid_Material` — the engine-default fallback. A different,
  older take on the same idea, and worth reading against the shipping one: it drives its emissive
  from a cursor-proximity glow and reverses the depth comparison. Its differences are noted inline
  in the HLSL.

## What is *not* recovered

`MovementBorder_Line`, the 64×64 BC5 two-channel texture holding the line's cross-section
profiles. The port reconstructs both channels analytically in
[`../assets/shaders/ribbon.fs.glsl`](../assets/shaders/ribbon.fs.glsl) rather than shipping the
game's asset. That is the single deviation, and it is a deviation of asset, not of maths.

## The port

| File | Role |
|------|------|
| [`../assets/shaders/ribbon.vs.glsl`](../assets/shaders/ribbon.vs.glsl) | `WorldPositionOffset` — the 8uu push along the eye ray. |
| [`../assets/shaders/ribbon.fs.glsl`](../assets/shaders/ribbon.fs.glsl) | `EmissiveColor` and `Opacity`, the whole material. |
| [`../assets/shaders/ribbon_glow.fs.glsl`](../assets/shaders/ribbon_glow.fs.glsl) | Not XCOM's. The material is `MLM_Unlit`, so its glow in-game is the scene's bloom acting on a flat emissive; we have no *scene* bloom chain, so the ribbon is re-drawn overbright and blurred back over the frame. |
| [`../src/render/ribbon/`](../src/render/ribbon/) | Strip meshes, the recovered parameters, and the two passes. |

XCOM works in unreal units with z up; this project works in tiles with y up, and a tile is
`WORLD_StepSize` = 96uu. Every recovered constant crosses that boundary through one factor,
`kUnrealUnit`, in [`../src/render/ribbon/RibbonConstants.hpp`](../src/render/ribbon/RibbonConstants.hpp),
so the numbers there can be checked against this study by eye.
