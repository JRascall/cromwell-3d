# How a renderer owns its scene — Source's client leaf system, read from the source

Written 2026-08-14, to settle the design of cromwell's render scene
(`src/cromwell/rhi/MIGRATION.md` §4.12) before any of it is built.

**The question.** cromwell's device renderer currently calls back into the game
through `IGeometrySource::submit(encoder, pass)`: the engine owns the frame
sequence, the game owns the draws. That has to invert — the game should REGISTER
what exists and the engine should own the list, cull it, sort it and draw it.
The question is what the registered thing IS, and what the engine is allowed to
know about it.

**The source.** Valve's Source SDK 2013, read directly at
`Tools/source-sdk-2013-master`. This is shipped C++, not a talk or a blog, so
everything tagged **[SDK]** below has a file and a line behind it. Source 1 is
old, and it is quoted here because the client leaf system is the canonical
worked example of exactly this inversion, still visibly the ancestor of what
Source 2 does.

---

## 1. What the game registers — and it is NOT a data struct **[SDK]**

```cpp
virtual void AddRenderable( IClientRenderable* pRenderable, RenderGroup_t group ) = 0;
```
`public/engine/IClientLeafSystem.h:139`

The game hands over **a pointer to an interface and a group**. It does not hand
over a mesh, a material or a transform. The engine then asks the object for what
it needs, per frame:

```cpp
virtual Vector const&  GetRenderOrigin( void ) = 0;
virtual QAngle const&  GetRenderAngles( void ) = 0;
virtual bool           IsTransparent( void ) = 0;
virtual void           GetRenderBounds( Vector& mins, Vector& maxs ) = 0;
virtual int            DrawModel( int flags ) = 0;
```
`public/iclientrenderable.h:73, 74, 76, 118, 87`

**THIS IS THE FINDING THAT MATTERS MOST, AND IT CONTRADICTS THE OBVIOUS DESIGN.**
Source's engine owns WHICH things are drawn and WHEN — the list, the culling,
the sorting, the batching into render lists — and hands the actual draw back to
the object through `DrawModel`. It is a hybrid, not the pure "the game submits
data, the engine draws it" that the phrase "the game registers renderables"
suggests.

What that buys: a renderable can be anything. A studio model, a brush model, a
sprite, a rope, a beam and a portal are all `IClientRenderable`, and none of them
had to be expressible as mesh-plus-material-plus-transform for the engine to
schedule it. The engine never learns what a rope is.

What it costs: the engine cannot batch or instance across renderables, because
it does not know that two of them share a mesh. Every draw is a virtual call
into game code.

## 2. What the engine keeps per renderable **[SDK]**

```cpp
struct RenderableInfo_t
{
    IClientRenderable*  m_pRenderable;
    int                 m_RenderFrame;   // which frame did I render it in?
    int                 m_RenderFrame2;
    int                 m_EnumCount;     // added to a particular shadow yet?
    int                 m_TranslucencyCalculated;
    unsigned int        m_LeafList;      // what leafs is it in?
    unsigned int        m_RenderLeaf;    // what leaf do I render in?
    unsigned char       m_Flags;
    unsigned char       m_RenderGroup;
    unsigned short      m_FirstShadow;   // first shadow caster that cast on it
    short               m_Area;          // -1 if it spans multiple areas
    signed char         m_TranslucencyCalculatedView;
};
```
`game/client/clientleafsystem.cpp:244`

Almost all of it is **bookkeeping the engine needs and the game must not see**:
frame stamps to avoid drawing a thing twice when it is in several visible
leaves, which leaves it sits in, which shadows fall on it.

Two fields are worth copying outright:

- **`m_RenderFrame` / `m_RenderFrame2`.** A renderable spanning several visible
  leaves would otherwise be collected once per leaf. Stamping it with the frame
  number is the whole de-duplication.
- **`m_TranslucencyCalculatedView`.** Translucent sorting is cached PER VIEW, and
  the record remembers which view the cached answer belongs to. Split-screen is
  why: two panes see the same object at different depths.

## 3. The groups are a sort key, and they encode SIZE **[SDK]**

```cpp
RENDER_GROUP_OPAQUE_STATIC_HUGE  = 0,   // Huge static prop
RENDER_GROUP_OPAQUE_ENTITY_HUGE  = 1,   // Huge opaque entity
RENDER_GROUP_OPAQUE_STATIC = ... ,
RENDER_GROUP_OPAQUE_ENTITY,             // smallest size, or default
RENDER_GROUP_TRANSLUCENT_ENTITY,
RENDER_GROUP_TWOPASS,                   // opaque and translucent in two passes
RENDER_GROUP_VIEW_MODEL_OPAQUE,
RENDER_GROUP_VIEW_MODEL_TRANSLUCENT,
RENDER_GROUP_OPAQUE_BRUSH,
RENDER_GROUP_OTHER,                     // unclassified. Won't get drawn.
```
`public/engine/IClientLeafSystem.h:32`

Three things to take from this:

1. **Opaque is bucketed by SIZE, not just by material.** Huge props are drawn
   first so they occlude, which makes the depth test reject more of what
   follows. That is a free early-z win from ordering alone.
2. **`RENDER_GROUP_TWOPASS` is a first-class group** — an object that is partly
   opaque and partly translucent is declared as such rather than being drawn
   twice by the game.
3. **`RENDER_GROUP_OTHER` — "unclassified, won't get drawn"** is the default. A
   renderable that never says what it is silently does not appear, which is a
   choice worth arguing with: it fails quiet.

## 4. What a VIEW carries — and what it does not **[SDK]**

```cpp
struct SetupRenderInfo_t
{
    WorldListInfo_t        *m_pWorldListInfo;
    CClientRenderablesList *m_pRenderList;
    Vector m_vecRenderOrigin;
    Vector m_vecRenderForward;
    int    m_nRenderFrame;
    int    m_nDetailBuildFrame;
    float  m_flRenderDistSq;
    bool   m_bDrawDetailObjects : 1;
    bool   m_bDrawTranslucentObjects : 1;
};
```
`game/client/clientleafsystem.h:75`

**THERE IS NO FILTER MASK.** A view is a position, a direction, a distance limit
and two toggles. Everything about "which parts of the world are visible" comes
from `m_pWorldListInfo` — the BSP's visible leaf set, computed from the PVS.

That is the single biggest thing Source does NOT give us. Its visibility answer
is a property of a precompiled BSP; a tile lattice has no PVS, and cromwell's
equivalent question — the storey cutaway, which hides the floors between the eye
and the room being looked into — is not a visibility question at all. It is a
per-view *gameplay* filter, and Source has no mechanism for one because it never
needed one.

## 5. It is a SINGLETON, and that is a hard limit **[SDK]**

```cpp
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CClientLeafSystem, IClientLeafSystem,
                                   CLIENTLEAFSYSTEM_INTERFACE_VERSION,
                                   CClientLeafSystem::s_ClientLeafSystem );
```
`game/client/clientleafsystem.cpp:335`

One leaf system, one world. Source's split-screen renders several VIEWS of that
one world — which is why the per-view caching in §2 exists — but there is no
provision for two players in two DIFFERENT worlds.

**This is where the Source design stops being a model for us.** cromwell has to
support four-player co-op with different worlds, different geometry and
different pipelines, as well as the single-player case with one of each. A
singleton scene cannot express that, so the scene must be an ordinary owned
object with as many instances as there are worlds.

The reference for that shape is Unreal, where the world owns the scene and the
scene owns the primitives, and several worlds coexist routinely (play-in-editor
runs the editor's world and the game's at once). **[inferred]** — stated from
familiarity with Unreal's `FScene`/`FSceneView` split rather than read from its
source in this session, and it should be verified before being relied on.

---

## 6. What this changes in cromwell's design

Written against `MIGRATION.md` §4.12 as it stood before this note.

| §4.12 proposed | After reading Source |
|---|---|
| One `RenderScene` | **One per world.** An owned object, never a singleton — the co-op requirement makes this non-negotiable, and Source's singleton is the thing that would have had to be undone later |
| Renderable = mesh + material + transform + bounds | **Keep it as data anyway**, but knowingly. See below |
| Per-renderable filter key, per-view mask | **Keep.** Source has no equivalent and needs none; we do, and nothing in Source argues against it |
| Cull by frustum | Source culls by PVS leaf. We have no BSP, so frustum plus the tile grid is the honest equivalent |
| Sort translucents back to front | **Confirmed, and cache the answer PER VIEW** — `m_TranslucencyCalculatedView` is exactly that, and it is the field a split-screen build would otherwise discover the hard way |
| — | **Bucket opaque by size before material.** Huge things first occlude the rest. Free, and not something the current design thought of |
| — | **Stamp with a frame number** to de-duplicate a renderable reachable through several visible cells |

### The one deliberate departure

Source registers an interface and calls `DrawModel` back. cromwell should
register **data** — mesh, material, transform — and let the engine issue the
draw itself.

The reason is that cromwell's whole point is a device-backed renderer with
several backends. A `DrawModel` callback puts game code inside a render pass,
holding a command encoder, which is exactly the seam `IGeometrySource` already
is and the one being removed. Source could afford the callback because it has
one backend and the game and the renderer are the same binary.

The cost is real and should be stated: anything that cannot be expressed as
mesh-plus-material-plus-transform needs the engine to grow a component for it,
rather than the game inventing one. Ropes, beams and particles are the usual
examples. That is the trade cromwell is choosing — it is the same trade Unity's
SRP and Godot's `RenderingServer` make, and the same reason both ship a fixed
vocabulary of renderer components.

---

## Sources

- Valve, **Source SDK 2013**, read at `Tools/source-sdk-2013-master`
  (`game/client/clientleafsystem.{h,cpp}`, `public/engine/IClientLeafSystem.h`,
  `public/iclientrenderable.h`). Primary; every **[SDK]** claim above carries a
  file and line.
- Unreal's `FScene`/`FSceneView` split is referenced once, tagged
  **[inferred]**, and is NOT verified in this session.
