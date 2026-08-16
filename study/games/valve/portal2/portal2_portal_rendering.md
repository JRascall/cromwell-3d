# Portal 2 — drawing a portal

How Valve draw a hole in a wall that you can see through, walk through, and see
*yourself* through, at 60 Hz, on a 2011 console, four levels deep, in
split-screen. Written for the mechanism rather than for a copy-paste, because
the interesting part is not "render the scene from the other camera" — everyone
gets that in an afternoon — it is the twenty things that break afterwards.

Companion notes: [`portal2_portal_gameplay.md`](portal2_portal_gameplay.md)
(placement, teleport, the physics of a hole) and
[`portal2_gels.md`](portal2_gels.md) (paint).

---

## 0. Provenance, and the one caveat that governs everything below

Portal 2's source code is not public and its engine branch has no SDK
(**[VDC]**). Four kinds of evidence are used here and every claim is tagged:

| Tag | What it is |
|---|---|
| **[VALVE-SHADER]** | Valve's own HLSL, shipped in the Source SDK 2013 tree on this machine (`src/materialsystem/stdshaders/portal_refract*`, `portal*.fxc`, `portalstaticoverlay*`). Real Valve source, real shipping shaders. |
| **[PORTAL-SRC]** | Valve's **Portal 1** client/server portal code, read from a community source tree ([RubberWar/Portal-2](https://github.com/RubberWar/Portal-2), the Portal code on the Alien Swarm branch). The structure, comments and constants are Valve's; the repository is not. |
| **[VALVE-COMMENTARY]** | Portal 2's in-game developer commentary — Valve engineers describing Portal 2 specifically. Transcript at [theportalwiki](https://theportalwiki.com/wiki/Portal_2_developer_commentary). |
| **[VDC]** | Valve Developer Community — entity keyvalues and console variables dumped from the shipping Portal 2 binaries. Community-maintained, but the *names and defaults* are the game's. |
| **[P2-RETAIL]** | The shipping game, installed at `E:\SteamLibrary\steamapps\common\Portal 2` and read directly: materials out of `pak01_dir.vpk`, and RTTI class names, profiler zone names, cvar names and shader parameter names mined out of `bin\stdshader_dx9.dll`, `portal2\bin\client.dll` and `portal2\bin\server.dll`. |
| **[inferred]** | Our reading. |

**The caveat, and how far §14 closes it:** the algorithm in §2–§11 is read from
**Portal 1-era code**. Portal 2 is a fork of it three years later on a different
engine branch. §14 checks it against the retail 2011 build and the answer is
better than expected — Portal 2's own profiler zone strings are
`Portal_Step0_prestencil_effects`, `Portal_Step1_writestencil` and
`Portal_Step4_restore_depthstencil`, **the same steps with the same numbering as
§2** — but there are real differences, and the ones found are corrected inline
below and listed together in §14.

---

## 1. The one thing that matters most

**A portal view is not a texture.** The scene through the portal is drawn into
**the same back buffer as everything else**, in the same pass order, masked by
the stencil buffer. There is no render target, no resolve, no resolution
mismatch, and no filtering — the pixels behind the portal are as sharp as the
pixels beside it because they *are* the same buffer at the same sample rate.

The recursion is the same trick applied to itself: **the stencil reference value
is the recursion depth.** Draw the portal quad where stencil == 0 and increment
it to 1; clear depth where stencil == 1; render the world from the exit camera,
which will itself find a portal and increment to 2. Coming back out, decrement.
The stencil buffer is literally a per-pixel answer to *"how many portals deep is
this pixel?"* **[PORTAL-SRC]**

That is the whole design. Everything else in this document is repair work for
what that design breaks.

Source *also* ships the obvious render-target implementation (§3) and keeps it
alive for hardware without a usable stencil buffer, and there are two shipping
shaders to prove it. But stencil is the default (`r_portal_use_stencils` defaults
to `1`) and the Xbox 360 build is hard-wired to it — the render-target materials
are `#if !defined( _X360 )`'d out of the precache list, with the comment *"XBox
360 is guaranteed to use stencil mode, and therefore doesn't need texture mode
materials"*. **[PORTAL-SRC]**

## 2. The six-step stencil algorithm

`CPortalRender::DrawPortalsUsingStencils()`, annotated. This is the load-bearing
function of the entire game. **[PORTAL-SRC]**

```
depth limit  = min( r_portal_stencil_depth, MAX_PORTAL_RECURSIVE_VIEWS,
                    (1 << materials->StencilBufferBits()) ) - 1

on first entry (recursion level 0):
    stencil = { func ALWAYS, pass SET_TO_REFERENCE, ref 0 }, clear stencil

for each active portal:
    parent_ref = current recursion level
    this_ref   = parent_ref + 1

    step 0  DrawPreStencilMask()      stencil func EQUAL ref parent_ref, ops KEEP
                                      — effects that must land *outside* the hole

    step 1  DrawStencilMask()         pass op INCREMENT_CLAMP
                                      — wrapped in an occlusion query (§7)

            if the portal was too small on screen last frame: skip to step 5

    step 2  ClearBuffersObeyStencil( colour=false, depth=true ) with ref this_ref
                                      — depth cleared *only inside the hole*

    step 3  RenderPortalViewToBackBuffer()
                                      — recurses; will re-enter this function
                                      — fog state and sky-glow overlays saved/restored around it

    step 4  DrawPostStencilFixes()    — put back the depth and fog we just destroyed (§9)

    step 5  pass op DECREMENT_CLAMP   — restore the parent level's stencil

on exit at level 0: stencil off; then DrawPortal() for every portal (§11)
```

Four things in that listing are worth more than the rest.

**`ClearBuffersObeyStencil(false, true)` is the whole trick.** Colour is
untouched; depth is cleared only where the stencil says "inside the hole". The
scene behind the portal then depth-tests against a fresh, empty depth buffer
inside an arbitrary silhouette. No second depth buffer, no copy.

**Increment/decrement, not "set".** The mask is restored by decrementing, which
means sibling portals at the same depth are independent and the nesting
survives a portal being visible through a portal through a portal.

**Depth is bounded by the stencil buffer's *value range*, not by memory** —
`1 << StencilBufferBits()`. With eight stencil bits that is 256, so the real
limit is `MAX_PORTAL_RECURSIVE_VIEWS = 11`, and its comment is honest about why:
*"seeing as how 5 is extremely choppy under best conditions and is barely
visible, 10 is a safe limit"*. **The shipping default is
`r_portal_stencil_depth 2`** — you see through a portal, and through the portal
in that view, and the third level is faked (§8).

**Everything is a re-entrant view.** Step 3 calls back into the renderer, which
calls back into this function. The view setup is `memcpy`'d out and back around
it (twice — once for the recursion and once at the end "if we don't restore
this, the view is permanently altered (in mid render of an existing scene)").

## 3. The other path: render targets

> **Portal 2 removed the choice.** `r_portal_use_stencils` does not exist in
> Portal 2's `client.dll` at all; what survives is `r_portalstencildisable`, and
> the render-target materials still ship. **[P2-RETAIL]** Stencil is not the
> default any more — it is the path.

`CPortalRender::DrawPortalsToTextures()` — used when
`r_portal_use_stencils` is 0. Portal views are rendered into `_rt_Portal1` and
`_rt_Portal2` **before** the main view, and the portal quad then samples them
*projectively*: **[PORTAL-SRC]** **[VALVE-SHADER]**

```hlsl
// portal_ps2x.fxc — the "Portal" shader
result.rgb = tex2D( PortalSampler, i.vPortalTexCoord.xy / i.vPortalTexCoord.z ).rgb;
```

The projection uses `$usealternateviewmatrix` / `$alternateviewmatrix`, a matrix
material var the game writes per frame. The same shader mixes in "static"
(`$staticamount`, `$staticblendtexture`) for an unlinked portal, and carries an
`$alphamasktexture` "for odd shaped portals". **[VALVE-SHADER]**

The costs of this path are the reason the other one exists: the portal view is
at render-target resolution rather than back-buffer resolution; it is one frame
of latency away from being wrong if you reuse it; recursion means N render
targets or a re-render; and the projective lookup drifts at grazing angles,
which is exactly why the fixed-function fallback subdivides the portal quad into
**8 × 6 sub-quads** (`DrawComplexPortalMesh`) instead of drawing two triangles.
**[PORTAL-SRC]**

Keep it in mind as the *shape* of the cheap answer, though: it is the one that
survives on hardware with no stencil, and it is what Portal 2 falls back to for
distant and deeply-nested portals in split-screen (§12).

## 4. The exit camera

No oblique projection matrix. Source does the two obvious things and one
non-obvious one. **[PORTAL-SRC]**

**The transform** is built once per link and cached as
`m_matrixThisToLinked`:

```cpp
// prop_portal_shared.cpp
MatrixInverseTR( localToWorld, matPortal1ToWorldInv );
matRotation.Identity();  matRotation.m[0][0] = -1.0f;  matRotation.m[1][1] = -1.0f; // 180° about up
*pMatrix = remoteToWorld * matRotation * matPortal1ToWorldInv;
```

World → entry-portal space, spin 180° (you come *out* facing away), portal space
→ world at the far end. Position, angles and velocity all go through it; angles
via `TransformAnglesToWorldSpace` rather than by transforming a rotation matrix,
with the comment *"originally we did a transformation on the angles, but that
doesn't quite work right for gimbal lock cases"*.

**The clip plane** is a user clip plane at the exit portal, not a modified
projection matrix:

```cpp
fCustomClipPlane[3] = vRemotePortalForward.Dot( ptRemotePortalPosition
                                              - (vRemotePortalForward * 0.5f) );
//moving it back a smidge to eliminate visual artifacts for half-in objects
```

Half a unit of slack, deliberately, so that an object *straddling* the exit
portal is not sliced by the clip plane before its ghost (§10) can cover the seam.

**The non-obvious one:** `portalView.zNear` is clamped to a minimum of 1.0, and
the FOV, width, height and aspect are copied unchanged from the parent view.
The portal view is the same camera, moved.

## 5. Culling through the hole

The naive version renders the entire exit room. Source builds a **frustum shaped
like the visible part of the portal**, and it is the best piece of geometry in
the file. `CalcFrustumThroughPortal()`: **[PORTAL-SRC]**

1. Take the portal's four corners and clip that polygon against the *current*
   frustum — which is itself a portal-shaped frustum if we are already one level
   deep. Output is a convex polygon of up to `4 + N` vertices.
2. For each edge of that polygon, build a plane through the edge and the camera
   origin. Rotate each plane through `m_matrixThisToLinked`.
3. Near plane = the **exit portal's own plane**. Far plane = the parent's far
   plane, transformed.
4. Store the result in `m_RecursiveViewComplexFrustums[level+1]` — an array of
   **complex frustums, one per recursion level**, so the next level's clip
   starts from the real silhouette rather than from six planes.

Step 5 is the part nobody expects: the engine's `Frustum` type holds exactly six
planes, so if the clipped polygon has more than four sides, it **collapses the
shortest edges and bridges their neighbours until four remain**, projecting the
merged vertex from the two surviving lines. Accuracy is traded for a fixed-size
frustum, deliberately, and the fallback if the merge maths degenerates is
`return false` with an assert that reads:

```cpp
AssertMsgOnce( fNormalDot != 0.0f,
    "Tell Dave Kircher if this pops up. It won't interfere with gameplay though" );
```

The *complex* frustum (all N planes) is kept for the recursion; the *reduced*
four-sided one is what the renderer's culling actually uses.

Visibility gets the same treatment from the other end. `AddToVisAsExitPortal()`
adds **the exit portal's four corners as vis origins**, then forces the vis
cluster and view leaf to the exit portal's — so PVS and area-portal culling
behave as though the camera were standing in the doorway, which it effectively
is. **[PORTAL-SRC]**

## 6. Deciding not to draw

Two independent gates, both of which can cancel a whole recursion level:

**By view** — `ShouldUpdatePortalView_BasedOnView()`: no linked partner, the
portal is the one we just came out of (`pCurrentPortal == m_pRenderingViewExitPortal`
— you never render the portal you are looking out of), backface, or nothing
survives the frustum clip.

**By pixels** — the mask draw in step 1 is wrapped in an occlusion query:

```cpp
pRenderContext->BeginOcclusionQueryDrawing( node->occlusionQueryHandle );
pCurrentPortal->DrawStencilMask();
pRenderContext->EndOcclusionQueryDrawing( node->occlusionQueryHandle );
```

and **last frame's** result — `fScreenFilledByPortalSurfaceLastFrame_Normalized`
— decides whether this frame bothers with steps 2–4. A portal that occupied a
handful of pixels last frame does not get a recursive scene render this frame.

The honesty in the code is worth copying: in queued (multithreaded) material
mode the check is **disabled**, with the comment *"queued mode makes us pass the
barrier of just noticeable difference when using a previous frame's occlusion as
a draw skip check"* — the data is another frame older, and they measured that it
showed. **[PORTAL-SRC]**

There is a subtlety this creates and Valve handle it explicitly: when the player
*walks through* a portal, every portal's screen coverage changes discontinuously,
so `CPortalRender::EnteredPortal()` re-roots the whole view-ID tree and then
**invalidates all pixel-visibility results** — *"we can get cases where a certain
portal wasn't visible last frame, but takes up most of the screen this frame"*.

### The view-ID tree

Each recursion path needs its own identity, because everything view-dependent in
Source is keyed by view ID — pixel visibility, glow occlusion, water reflection
state. `PortalViewIDNode_t` is a tree: the head node is the main view, and each
node has one child slot per portal in the world. Walking "main → orange → blue"
allocates and reuses one node per path, and the node holds the occlusion query
handle and last frame's coverage. Nodes are freed as soon as a portal stops
being visible, so the tree costs what is actually being drawn. **[PORTAL-SRC]**

Walking through a portal transplants the tree: the entered portal's subtree
becomes the new head, and a fresh node holding main's old children is hung off
the exit portal's slot — *"imagine entering a portal walking backwards"*. Pixel
visibility results are shifted between view IDs rather than thrown away.

## 7. The depth doubler

At the deepest allowed level, instead of recursing once more or showing a flat
grey, the portal is drawn with **last frame's front buffer**, projected using
**the view matrix from the frame it was captured in**:

```cpp
if ( m_bUsableDepthDoublerConfiguration && GetRemainingPortalViewDepth() == 1 )
{
    // save the view matrix for usage with the depth doubler.
    // It's important that we do this AFTER using the depth doubler this frame to
    // compensate for the fact that the front buffer is 1 frame behind the current
    // view matrix; otherwise we get a lag effect when the player changes their angles
    pRenderContext->GetMatrix( MATERIAL_VIEW, &m_DepthDoublerTextureView );
}
```

`WillUseDepthDoublerThisDraw()` requires: stencil mode, a "usable configuration",
remaining depth 0, recursion level > 1, and not the portal we exited from. The
material is `models/portals/portal_depthdoubler`, and the saved matrix is stuffed
into its `$alternateviewmatrix`. **[PORTAL-SRC]** The shipped Portal 2 material
confirms it and names its own render target: **[P2-RETAIL]**

```
Portal { $basetexture _rt_DepthDoubler   $alphamasktexture "models/portals/portal_mask"
         $usealternateviewmatrix 1
         $alternateviewmatrix "[ 1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1 ]"   $model 1 }
```

— so in Portal 2 the doubler samples a **dedicated `_rt_DepthDoubler` target**
rather than the raw front buffer, and it is the *render-target* `Portal` shader
(§3) doing the projective lookup. The one path that survived the stencil-only
decision is this one, which is exactly the use the render-target shader is good
at: a static image re-projected by a matrix.

**It doubles the apparent recursion depth for the price of one quad.** Level 2's
image contains level 1's image from last frame, which contained level 0's from
the frame before — so a stencil depth of 2 reads as four or five levels of
"portal in a portal" to a player who is not standing still comparing frames. The
one-frame lag is hidden by re-projecting with the matching old view matrix, and
the comment above is the entire reason that line sits where it does rather than
at the top of the function.

## 8. Everything the hole broke, and the patch for each

`DrawPostStencilFixes()` is four lines and each one repairs a different system:

```cpp
pRenderContext->ClearBuffersObeyStencil( false, true );   // 1. depth, again
RenderFogQuad();                                          // 2. fog
DrawSimplePortalMesh( m_WriteZ_Model, 0.0f );             // 3. depth = portal plane
DrawRenderFixMesh( m_WriteZ_Model, 0.0f );                // 4. …and at the near plane
```

**Depth.** The recursive view wrote depth values from a *different* camera into
the shared depth buffer. Anything drawn afterwards in the parent view — the
portal's own rim, transparents, the view model — would depth-test against
nonsense. So the hole's depth is cleared again and then re-written with the
portal *surface's* depth, which is the depth the parent view expects there. The
comment on the clear is blunt: *"fast clipping may have hosed depth, reset it"*.

**Fog.** Source's fog is per-pixel from the *current* view's parameters, and the
recursion changed them. `RenderFogQuad()` draws the portal quad with the
translucent vertex-colour material, computing the fog factor **per corner** on
the CPU from the current view-projection matrix and pushing `1 - fogAmount` into
vertex alpha. It is a hand-rolled fog patch over the hole, and it is skipped
entirely when `GetFogMode() == MATERIAL_FOG_NONE`. Around the recursion itself,
fog mode, colour, start, end and Z are saved and restored by hand, and so are
sky-glow overlay parameters, **per recursion level**
(`CGlowOverlay::BackupSkyOverlayData( m_iViewRecursionLevel )`). **[PORTAL-SRC]**

There is a matching hook for the *entry* side, `ShiftFogForExitPortalView()`,
because fog is distance-from-camera and the camera just teleported: the fog
origin has to be shifted to the exit portal or the far room fogs as though it
were adjacent to the near one.

**Water** gets four dedicated hooks — `WaterRenderingHandler_PreReflection` /
`PostReflection` / `PreRefraction` / `PostRefraction` — plus
`ShouldForceCheaperWaterLevel()`, which downgrades reflections inside portal
views on the stated grounds that *"it's a good idea to force cheaper water when
the ratio of performance gain to noticability is high"*. A reflection inside a
portal view is a third camera; Valve simply refuse to pay for it. **[PORTAL-SRC]**

## 9. The near-plane problem

The one that kills naive implementations. As you walk into a portal, the portal
plane crosses the camera's near clip plane; the quad gets clipped away; for one
or two frames you see the wall the portal is painted on, or worse, the void.

Source's answer is a second, separate mesh drawn **at the near plane in view
space**: **[PORTAL-SRC]**

```cpp
vOrigin = CurrentViewOrigin() + vForward * (view->GetViewSetup()->zNear + 0.05f);
WorkVertices[0..3] = vOrigin ± vRight*40 ± vUp*40;
ClipFixToBoundingAreaAndDraw( WorkVertices, pMaterial );
```

An 80 × 80 unit quad, 0.05 units in front of the near plane, **clipped against
the portal's own bounding planes** so it only covers the part of the screen the
portal would occupy. Guards: only at recursion level 0 (*"a render fix should
only ever be necessary in the primary view"*), only when the camera is behind or
within one unit of the portal plane, and only within `PORTAL_HALF_HEIGHT` of the
portal centre. Getting that close also snaps `m_fStaticAmount` to 0 so an
opening portal cannot be caught mid-fade in your face.

The same mesh appears in three places with three materials: as part of the
stencil mask (so the mask reaches the screen edge), in the post-stencil depth
fix (`WriteZ`), and as the portal surface itself in the main draw. There is even
a material parameter for its cousin problem — `$renderfixz`, *"special depth
handling, intended for rendering bug workarounds for extremely close
polygons"*. **[VALVE-SHADER]**

## 10. The portal surface, shader by shader

Three shaders ship, and together they are the entire visual identity of a portal.
All three are real Valve HLSL from the SDK 2013 tree. **[VALVE-SHADER]**

### `portal_refract` — one shader, three stages

`// STATIC: "STAGE" "0..2"`. The game builds three materials from it and draws
them at three different points in §2.

**Stage 1 — the hole.** Four lines, and it is the mask:

```hlsl
float2 vStretchVector    = ( i.vUv0.xy * 2.0f ) - 1.0f;
float  flDistFromCenter  = length( vStretchVector );
float  flStencilCutout   = step( flDistFromCenter, flPortalOpenAmountSquared );
result.rgb = 0.0f;  result.a = flStencilCutout;
```

Alpha test at 0.5, depth writes off, alpha writes off. **The portal's oval is not
geometry — it is `length(uv*2-1)` on a quad**, and the *radius* of that oval is
`portalOpenAmount²`, so the same expression that shapes the portal also animates
it open. `$portalopenamount` is driven from the client at
`m_fOpenAmount += frametime * 2.0f`, i.e. **half a second to fully open**.
**[PORTAL-SRC]**

**Stage 0 — the warp.** Drawn *before* the hole is cut (`DrawPreStencilMask`),
and only while `0 < openAmount < 1`. It samples the full-screen frame buffer
texture and pushes the sample point outward along the stretch vector, so the wall
around a portal appears to be sucked into it as it opens:

```hlsl
float2 vTangentRefract = -vStretchVectorNormalized * flPortalOpenAmountSquared
                       * ( 1.0f - pow( saturate( flDistFromCenter ), 64.0f ) );
vTangentRefract *= smoothstep( flPortalOpenAmount * 1.5f, flPortalOpenAmount, flDistFromCenter );
```

The screen-space direction to offset in is derived by projecting the *world
tangent and binormal* through the view-projection matrix in the pixel shader and
subtracting the unrefracted position — a per-pixel tangent basis in screen space,
because the portal is a model and the warp has to follow its orientation. The
comment admits the limit of doing it this way: *"this works well perpendicular to
the surface, but because the projection is non-linear, it's refracty very edge
on"*, and the fudge is to use `{32, 32}` for the radius when the portal is really
32 × 54, *"this reduces the artifacts from the comment above"*. A darkening ring
is added on top, explicitly *"to help it stand out on plain walls"*.

**Stage 2 — the rim.** The fire. Two scrolling samples of a noise texture, each
distorting the other's lookup, combined with the inner and outer border masks,
then used to index a **1D colour ramp** — which is how one shader gives a blue
portal and an orange portal without a branch:

```hlsl
float4 cNoiseTexel1 = tex2D( g_tPortalNoiseSampler, i.vNoiseTexCoord.xy );
float4 cNoiseTexel2 = tex2D( g_tPortalNoiseSampler, i.vNoiseTexCoord.wz - cNoiseTexel1.rg*0.02 );
       cNoiseTexel1 = tex2D( g_tPortalNoiseSampler, i.vNoiseTexCoord.xy - cNoiseTexel2.rg*0.02 );
...
float4 cFlameColor = tex1D( g_tPortalColorSampler, pow(flNoise,0.5) * flBottomToTopBrightnessShift * flTransparancy );
cFlameColor.rgb *= g_flPortalColorScale; // Brighten colors to make it look more emissive
```

Alpha blended, alpha test at 1/255, fogged, tonemapped. The noise UVs are built
in the vertex shader **divided by the open amount** — so the flames appear to be
pinned to the portal's edge as it grows rather than scaling with it — and scroll
in opposite directions along U (`o.vNoiseTexCoord.zw` is fetched as `.wz`
specifically *"to avoid matching layers"*). There is a vertical brightness ramp,
`pow(abs(uv.y),1.5)*0.8+0.2`, so a portal is dimmer at the bottom than the top.

The shader is also a museum of the tuning that got it there — eight commented-out
alternatives with notes like *"More broken up flames and crazier"* vs *"More
solid flames and calmer"*, and *"this will thicken the border but also darken the
alpha blend in ugly ways. Leaving this here for experiments later."* That is what
a shipped effect shader looks like.

> **Correction from the retail build.** In Portal 2, `portalstaticoverlay_1.vmt`
> and `_2.vmt` — the materials the code in §2 draws on *every* portal, every
> frame — are **`PortalRefract` with `$Stage 2`**, not the `PortalStaticOverlay`
> shader their names suggest. **[P2-RETAIL]** So the thing permanently painted on
> a Portal 2 portal is the *flame rim*, and stage 2 is not an opening effect at
> all; it is the portal's entire visual identity. The colour comes from
> `$PortalColorTexture "models/portals/portal-blue-color"` (or `-orange-`) with
> `$PortalColorScale 4.0`, and the DX8 fallback block halves it to 1.4 for blue
> and 1.25 for orange. Three material proxies drive it: `CurrentTime`,
> `PortalOpenAmount`, `PortalStatic`.
>
> Portal 2 also **extended the shader**. `stdshader_dx9.dll` carries parameters
> the SDK 2013 version has no trace of — `$PortalColorGradientDark` /
> `$PortalColorGradientLight` (a two-colour gradient *instead of* a 1D ramp
> texture) and four co-op colours in one material: **[P2-RETAIL]**
>
> ```
> $PortalCoopColorPlayerOnePortalOne  "[0.125 0.500 0.824]"
> $PortalCoopColorPlayerOnePortalTwo  "[0.075 0.000 0.824]"
> $PortalCoopColorPlayerTwoPortalOne  "[1.000 0.705 0.125]"
> $PortalCoopColorPlayerTwoPortalTwo  "[0.225 0.010 0.010]"
> ```
>
> That is `portalstaticoverlay_tinted.vmt`, and it is the whole reason co-op can
> show four distinguishable portals: **the rim shader gained a colour input per
> (player, portal) pair**, so one material serves all four.

### `portalstaticoverlay` — the disconnected state

A grey static texture, alpha = `$staticamount`, optionally masked. Drawn over the
portal whenever it has no partner, when it is being viewed *inside* another
portal's recursion beyond the depth limit, and briefly on the *other* portal
whenever this one moves — the client sets `pRemote->m_fStaticAmount = 1.0f` on
placement and decays it at 1.0/second. **The visual language of "this portal's
link is broken" is a full shader path, not a texture swap.**
**[VALVE-SHADER]** **[PORTAL-SRC]**

In the retail game this shader survives in a different job: the `_noz` materials
(`portalstaticoverlay_noz`, `_1_noz`, `_2_noz`) are `PortalStaticOverlay` with
`$ghostoverlay 1` or `2`, `$Additive 1`, `$NoCull 1`, `$staticamount 0.3` and a
flat `dummy-gray` / `dummy-blue` / `dummy-orange` texture — and
`PORTALGHOSTOVERLAY` is a shader combo in `stdshader_dx9.dll`. **[P2-RETAIL]**
That is **the tint on a ghosted object** (§7 of
[`portal2_portal_gameplay.md`](portal2_portal_gameplay.md)): a flat additive wash
in the portal's colour, at 30%, with depth off. The "static" shader ended up
being the shader for *things half-through a portal*, which is the same idea —
"this is not quite really here" — pointed at a different noun.

### `Portal` — the render-target path

§3. Projective sample plus the same static blend.

### And what `DrawPortal()` actually calls

The dispatch at the end of §2 is worth reading as a table, because it is the
complete answer to "what does a portal look like from here":

| Where you are | What is drawn on the portal |
|---|---|
| main view, portal linked | nothing (you are seeing the stencil hole) + `WriteZ` fix mesh |
| main view, portal unlinked | `portal_refract` (if opening) + `portalstaticoverlay` |
| inside a portal view, depth remaining | as above |
| inside a portal view, **depth exhausted, level > 1** | **depth doubler** |
| inside a portal view, depth exhausted, level ≤ 1 | `portalstaticoverlay` |
| you are looking *out of* this portal | nothing at all |

## 11. What Portal 2 changed

Portal 1 shipped this renderer on a 2007 PC. Portal 2 shipped it on Xbox 360 and
PS3, in **split-screen co-op**, which means two players × N portal views ×
water reflections in one 16 ms frame. Two documented changes:

**World imposters.** From the commentary, Gary McTaggart — the single most useful
Portal 2 rendering quote there is: **[VALVE-COMMENTARY]**

> Since the Portal 2 world geometry is relatively simple, we automatically build
> a version of the world geometry that has a single texture that combines both
> lighting and surface shading along with another texture with just surface
> shading. The latter texture is used when rendering dynamic lights. These
> textures are at the same spatial resolution as the light maps and get packed
> into a single large atlased texture per level. Drawing this simple world
> imposter model takes very little CPU time, which was a limited resource on
> Portal 2 due to the many portal views, split-screen views, and water reflection
> views that we needed to render. We initially planned on using the world
> imposters only for water reflection rendering, but we ended up using it to
> improve performance in split-screen mode where it is used for **rendering
> distant portals and portals that are two levels deep**.

Read that twice. **The bottleneck was CPU, not GPU** — the cost of a portal view
is *submitting the world again*, and their fix was a pre-baked single-draw-call
version of the level with lighting baked into the albedo. It is a LOD for
"the world", triggered by view importance rather than by distance. The second
texture (shading without lighting) exists so dynamic lights still work on it.

**Improved shadow mapping.** `env_projectedtexture` gained higher default
resolution, better filtering, and **caching of shadows from static objects** in
the Portal 2 branch. **[VDC]** In a renderer that draws the world four times per
frame, "the static half of this shadow map did not change" is the same economy as
the imposters.

Two more branch features are portal-adjacent: **`linked_portal_door`**, a
general **world portal** — a rectangular seamless link between two places, using
the same renderer, which is what Valve built the whole game's layout with before
removing them (*"Once the game settled down we were able to finalize our path and
remove all of the world portals. There's only one impossible space left in the
whole game"* **[VALVE-COMMENTARY]**); and **blob particles** (`render_blobs`),
which is the gel — see [`portal2_gels.md`](portal2_gels.md).

## 12. Cost model

There is no published frame breakdown, so this is structural, not measured
**[inferred]**:

| Cost | Scales with |
|---|---|
| scene submission | **number of portal views**, which is `portals visible × recursion depth`, capped at 2 by default |
| stencil mask draws | portals visible, per level — trivial |
| depth clears | pixels inside the mask, per level — cheap, and stencil-bounded |
| state save/restore | per recursion level: fog (5 values), sky overlays, view setup memcpy, frustum memcpy |
| occlusion queries | one per (portal, path), read a frame late |
| the fixes | one quad each for fog, depth, near plane |

The dominant term is the first, and it is *CPU* — which is what the world
imposters answer. Everything else in the algorithm exists to **stop level N+1
from happening**: the complex frustum, the pixel-visibility gate, the depth
doubler, the "never render the portal you came out of" rule.

## 13. What transfers here

This project has no portals and is unlikely to want gameplay ones. Four things
transfer anyway.

**1. "A second view is not free, and the cost is submission."** Every
view-dependent system has to be re-derived per view: frustum, vis, fog, water,
pixel visibility, view ID. Our renderer already has a second view (the shadow
map) and will want more (reflections — [`realtime_reflections.md`](../../../realtime_reflections.md);
a security-camera panel; a minimap). The lesson from §12 is that the answer at
scale was a *cheaper representation of the world*, not a cheaper renderer.

**2. Last frame's occlusion query as a draw gate.** §6 — with Valve's own caveat
that it stops working when the pipeline is queued and the data gets another frame
older. This is a real technique for our reflection and shadow passes, and the
caveat is the part everyone omits.

**3. The depth doubler is the general trick.** *When recursion has to stop, stop
it by reusing last frame's result re-projected with last frame's matrix, not by
showing a constant.* That is a temporal-reprojection LOD, and it applies to
anything with a recursive or expensive tail — reflections of reflections,
probe-lit probes.

**4. Stencil as a per-pixel counter.** We use stencil for nothing today. §1's
"the stencil value *is* the recursion depth" is worth remembering the next time
we need per-pixel "which region is this" state — masked decal application, an
overlay that must not double-blend, a UI cutout in the 3D view.

And one anti-lesson: the portal surface effects (§10) are *three shaders and a
1D colour ramp*, and the entire visual read — colour identity, open animation,
disconnected state, the warp — comes out of two textures and a distance field on
a quad. There is no mesh, no particle system, and no animation asset. When
[`../source2_particles.md`](../source2_particles.md) argues about what a particle
system needs to be, remember that Portal's most recognisable effect is not one.

---

## 14. Verified against the retail install

Read on 2026-08-15 from `E:\SteamLibrary\steamapps\common\Portal 2`: materials
extracted from `pak01_dir.vpk`, and strings — RTTI class names, VPROF zone names,
cvar names, shader parameters — mined from `bin\stdshader_dx9.dll` and
`portal2\bin\{client,server}.dll`. The shader DLL still carries its build path,
`C:\buildworker\portal2_rel_pc_win32\build\src\materialsystem\std…`, so this is
the retail PC build and not a leftover. **[P2-RETAIL]**

### Confirmed

**The algorithm is the same, step for step.** Portal 2's own profiler zone names
are `Portal_Step0_prestencil_effects`, `Portal_Step1_writestencil` and
`Portal_Step4_restore_depthstencil`, alongside `Portal_FillInStencilViews`,
`CPortalRender::DrawPortalsUsingStencils` and
`CViewRender::ViewDrawScene_PortalStencil`. §2's six steps, with Valve's own
numbering, in the 2011 binary.

**The classes survived:** `CPortalRender`, `CPortalRenderTargets`,
`CPortalRenderable_FlatBasic`, `C_PortalGhostRenderable`, `CPortalSimulator`,
`CAutoInitPortalDrawingMaterials`, plus material proxies `CPortalOpenAmountProxy`
and `CPortalPickAlphaMaskProxy`.

**The materials survived, name for name:** `portal_1_dynamicmesh` (`Portal`,
`$basetexture _rt_Portal1`), `portal_1_renderfix_dynamicmesh` (`$renderfixz 1` —
§9's near-plane fix, shipped), `portal_stencil_hole` (`PortalRefract $Stage 1`),
`portal_refract_1` (`$Stage 0`), `portal_depthdoubler`.
`r_portal_use_complex_frustums` (§5) and `r_portal_stencil_depth` (§4) are both
present, and the latter's help text is verbatim the one in the Portal 1 source:
*"When using stencil views, this changes how many views within views we see"*.

### Corrected

1. **No render-target *path* any more.** `r_portal_use_stencils` is absent; only
   `r_portalstencildisable` remains. The `Portal` shader lives on for the depth
   doubler and the fog-compatible variants. (§3, §7.)
2. **`portalstaticoverlay_1/2` are `PortalRefract $Stage 2`** — the always-on
   flame rim, not the legacy static shader. (§10.)
3. **The depth doubler has its own render target**, `_rt_DepthDoubler`. (§7.)
4. **The rim shader gained gradient and co-op colour inputs** —
   `$PortalColorGradientDark/Light` and four `$PortalCoopColorPlayer*Portal*`
   vectors. (§10.)
5. **`PortalStaticOverlay` was repurposed** as the ghost-overlay tint
   (`$ghostoverlay`, `PORTALGHOSTOVERLAY`). (§10.)

### New in Portal 2, from the cvar names

| cvar | What it tells us |
|---|---|
| `r_portal_use_pvs_optimization` | *"Enables an optimization that allows portals to be culled when outside of the PVS"* — §5's vis work made cheap at the top |
| `r_portal_fastpath`, `r_portal_fastpath_max_ghost_recursion` | a cheaper route through the whole thing, with a **separate recursion cap for ghosts** — ghosting through recursive views was expensive enough to need its own limit |
| `r_portal_earlyz` | an early-Z pass for portals — and it is a real function, `CPortalRender::DrawEarlyZPortals`, sitting beside `DrawPortalsUsingStencils`. Portals get their own depth prepass before the recursion, so the expensive scene renders inside each hole are depth-rejected against a buffer that already knows where every portal is |
| `r_portalscissor` | scissor the portal view to its screen bounds — the cheap sibling of §5's frustum work, and the one thing §2's algorithm does *not* do in the Portal 1 code |
| `r_portal_use_dlights`, `portal_transmit_light` | the light-transfer-through-portals feature that Portal 1's code had commented out *"indefinitely"* exists here as shipped toggles |
| `portal_draw_ghosting`, `portal_ghosts_disable`, `portal_ghosts_scale`, `portal_ghost_force_hitbox`, `portal_ghost_use_network_origin`, `cl_portal_ghost_use_render_bound` | six knobs on the ghost system against Portal 1's zero |
| `cl_useoldswapportalvisibilitycode` | §6's `EnteredPortal()` view-ID transplant was rewritten, with the old one kept behind a cvar |

And the architecture change worth the most: the portal is now
**`CPortal_Base2D`** (with `CPortal_Base2D_Shared`), and `CLinkedPortalDoor` /
`CPropLinkedPortalDoor` derive from the same base. **Portal 2's world portals and
its gun portals are one implementation**, which is why §11's `linked_portal_door`
gets the whole renderer for free. `CPortalSimulator` also appears as a
`NetworkVar` member of `CPortal_Base2D`, so the client carries portal simulation
state rather than inferring it.

### Still not verified

Recursion *defaults* (`r_portal_stencil_depth`'s shipped value), the actual pass
order in a frame, and how much of a frame portal views cost. Those need a
RenderDoc capture or a console dump from a running game, not a string mine.

---

## Sources

- **Valve HLSL**, Source SDK 2013 tree (local):
  `src/materialsystem/stdshaders/portal_refract_ps2x.fxc`, `portal_refract_vs20.fxc`,
  `portal_refract_helper.cpp`, `portal_ps2x.fxc`, `portal.cpp`,
  `portalstaticoverlay_ps2x.fxc`.
- **Portal client code** (Valve's, via a community tree):
  [RubberWar/Portal-2](https://github.com/RubberWar/Portal-2) —
  `src/game/client/portal/PortalRender.{h,cpp}`, `portalrenderable_flatbasic.cpp`,
  `C_PortalGhostRenderable.cpp`, `c_prop_portal.cpp`, `portal_render_targets.cpp`.
- **Portal 2 developer commentary**, transcript:
  [theportalwiki.com](https://theportalwiki.com/wiki/Portal_2_developer_commentary)
  — "World Imposters" (Gary McTaggart), "World Portals", "Predicting With Portals"
  (Dave Kircher).
- **Valve Developer Community**: [Portal 2 engine branch](https://developer.valvesoftware.com/wiki/Portal_2_engine_branch),
  [prop_portal](https://developer.valvesoftware.com/wiki/Prop_portal).
- **The retail install**, `E:\SteamLibrary\steamapps\common\Portal 2` —
  `portal2\pak01_dir.vpk` (`materials/models/portals/*.vmt`), `bin\stdshader_dx9.dll`,
  `portal2\bin\client.dll`, `portal2\bin\server.dll`. See §14.
- Community reimplementations, for comparison only:
  [Rendering recursive portals with OpenGL](https://th0mas.nl/2013/05/19/rendering-recursive-portals-with-opengl/) (Thomas Rinsma),
  [Rendering Portals](https://www.cs.rpi.edu/~cutler/classes/advancedgraphics/S21/final_projects/metzlr.pdf) (RPI).
