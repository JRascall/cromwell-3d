# The foreground view — engine building blocks for a first-person viewmodel

**A design note, not research** — what `cromwell` needs so that a game built on
it can draw a first-person weapon/hands/cockpit model that renders over the
world without being part of it, animates, and is lit and shadowed credibly.
The research it reads from is [`source_fps_viewmodel.md`](../games/valve/source_fps_viewmodel.md);
section references below (§2, §5, §7…) point into that note.

**Status: not built.** This note is the build plan. Nothing described here
exists in the engine yet — the audit that established that is summarised in §1.

**Scope guard.** These are *engine* blocks. No first-person camera pawn, no
bob/sway, no weapon logic — those are game-side and cheap once the blocks
exist. Skeletal animation (ozz integration, sockets, bonemerge) is the one
genuine prerequisite deliberately **deferred**; see §7.

Named **foreground**, not viewmodel, deliberately: judged against cromwell's
three target genres it serves the FPS weapon and hands, a cockpit interior,
and a third-person held-tool close-up equally. "Viewmodel" is what one genre
calls it.

---

## 1. Where the engine stands today

Source's answer is five independent decisions (§1 of the study). Mapped onto
cromwell as it stands:

| Source decision | cromwell today |
|---|---|
| Weapon lives outside the world's spatial index | Fine already — drawing is retained-mode free; the game submits what it wants per pass. Nothing to build. |
| Drawn in a second 3D view over the finished scene | **Missing.** `Camera` renders offscreen to its *own texture* (`renderingToTexture`); there is no in-frame sub-view composited into the same target. Pass orchestration lives game-side in `FrameRenderer`. |
| That view has its own FOV and near plane | Precedent exists — `ModelPreview` fits its own near plane via `rlSetClipPlanes` — but no derivation from a live main camera, and no reconciliation when the world FOV changes. |
| Depth remapped into the front 10% of the buffer | **Missing entirely.** Nothing in the tree touches `glDepthRange`; rlgl does not wrap it. (The `setDepthRange` in `TexturePreviews` and the shadow uniforms are unrelated quantities that share the name.) |
| Lit at the player's body, not its own position | **Missing.** `PbrShader` evaluates every positional lighting input — shadow test, lightmap, probe selection — at the fragment's world position, which for a model glued to the camera is meaningless. |

Skeletal animation: `cromwell/model/` is `ModelAsset` + `ModelPreview` and
nothing else. No bones, no clips, no sockets. Deferred, not forgotten — §7.

The four blocks below close the render-side gaps. They are ordered by
dependency; each is buildable and testable on its own.

---

## 2. Block 1 — the GL door: depth range

**Where:** `cromwell/gpu/GL.hpp` / `GL.cpp`.

`GL.hpp` is the one permitted door to raw OpenGL, with a standing rule: if
rlgl wraps it, call rlgl; if not, declare it here with a comment naming the
pass that needs it. `glDepthRange` is exactly that case — core GL since 1.0,
absent from rlgl, needed by one named pass.

Two additions:

```cpp
/* Remaps clip-space depth into a sub-range of the buffer. The foreground
 * pass (camera/ForegroundPass.hpp) writes [0, 0.1] so its model out-ranks
 * every world pixel while still depth-testing against itself. */
void depthRange(float nearMapped, float farMapped);

/* RAII: restores [0, 1] on destruction. The restore is the part that must
 * not be forgettable — a leaked depth range breaks every later pass, and
 * silently. */
class DepthRangeScope { ... };
```

**Why the remap and not depth-test-off** — restated here because it is the
single most misdescribed part of the technique (§1 of the study): the model is
still depth-tested and depth-written *normally*, against itself. Only the
mapping from clip space to buffer values changes. Every world pixel already in
the buffer is numerically further away, so the model wins against the world —
while the barrel still occludes the hand and the hand the sleeve. A
depth-test-off hack loses that self-occlusion; this does not.

---

## 3. Block 2 — `ForegroundProjection`: the math, no GL

**Where:** `cromwell/camera/ForegroundProjection.hpp` / `.cpp`. A value type,
no GL calls, so every part of it is unit-testable without a context.

Derived **from** a `Camera`, never stored independently:

```cpp
auto fg = ForegroundProjection::from(camera)   // eye transform + world FOV read HERE
    .withFovDegrees(54.0f)
    .withNearPlane(0.05f);
```

Deriving is the contract, and it comes from the study's first hard-won lesson
(§2): Source computes the viewmodel transform **in the same function, from the
same eye transform, in the same frame** as the camera. Compute it anywhere
else and the two disagree by a frame and the gun swims. `from(camera)` makes
the same-frame read the only way to get one.

It owns three pieces of math that must agree with each other, which is why
they live in one class:

1. **FOV reconciliation by delta, not ratio** (§3). When the world FOV moves —
   zoom, user setting — the foreground FOV moves by the *difference*:
   `fgFov = fgBase + (worldFov − worldBase)`. A ratio would over-swing the
   foreground under a world zoom. And the widescreen aspect correction that
   the world clamps is applied **uncapped** for the foreground — Source does
   this so ultrawide players see more weapon, not a stretched one.

2. **The attachment warp** (§6) — reproject a point from foreground-projection
   space into world-projection space:

   ```cpp
   Vec3 warpToWorld(Vec3 foregroundPoint) const;
   ```

   The study marks this **mandatory**, not optional polish: muzzle flashes,
   tracer origins and shell ejects are world-space effects, and the rendered
   barrel tip is not where the world projection thinks it is — the two
   frustums disagree by exactly the FOV and near-plane difference. Without the
   warp every muzzle effect visibly misaligns. Pure matrix math; a round-trip
   unit test pins it.

3. **The near plane** — its own, far closer than the world's (Source uses 1
   unit against the world's 7, §1). Follows the `ModelPreview` precedent of
   fitting the near plane to the content. A documented default, overridable.

---

## 4. Block 3 — `ForegroundPass`: the scoped render slot

**Where:** `cromwell/camera/ForegroundPass.hpp` / `.cpp` (or a sibling
directory if camera/ starts crowding — count units, not files).

The `Push3DView` equivalent: a scoped object the game opens **inside the Main
phase, after the world's lit geometry, in the same HDR target**. It:

- opens its own `BeginMode3D` with a `ForegroundProjection`;
- holds a `DepthRangeScope(0.0f, 0.1f)`;
- restores both on close, in the right order;
- carries its own `CW_PROFILE_ZONE_N("foreground")` and `CW_GPU_ZONE`.

Constraints the header must state, because each is invisible until it bites:

- **Position in the frame:** after the world's lit geometry, **before the
  tone map**. The foreground is drawn in linear radiance and goes through the
  same tonemap and any bloom as the world — Source's ordering (§5: after
  motion blur, before bloom), and what keeps the model looking like it is *in*
  the scene rather than pasted on. It therefore also sits before the Display
  phase; movement ribbons and UI still draw over it.
- **It must not open a render target.** It runs inside the camera's colour
  target, and `Camera.hpp` already documents raylib's non-nesting
  `BeginTextureMode` failure. The pass inherits that rule.
- **GPU zone is a sibling of `lit scene`, never nested** — `GL_TIME_ELAPSED`
  allows one active query. The caller opens the pass outside the lit-scene
  GPU zone; the header says so.
- **Depth readers beware:** any pass sampling scene depth after this one sees
  `[0, 0.1]` values in foreground pixels. Today nothing does (SSAO and decals
  run off the prepass, earlier); the hazard is for a future depth-of-field or
  soft-particle pass, and the header names it so that pass's author finds the
  warning. CPU-side picking (`TilePicker`, `SurfacePicker`) is unaffected.
- **The foreground model is invisible to screen-space effects** — no SSAO, no
  decals, no prepass entry. This matches Source and is correct: an arm should
  not receive a world decal, and its ambient occlusion is baked or nothing.

What the pass is **not**: it does not own a draw list, a model, or any
animation state. The game submits whatever draws it wants inside the scope,
exactly as it does for the world. The engine provides the *slot*, with the
projection and depth semantics that make it a foreground view.

---

## 5. Block 4 — `PbrShader` lighting-origin override

**Where:** `cromwell/lighting/PbrShader.hpp` / `.cpp` and
`cromwell/assets/shaders/pbr.fs.glsl`.

The problem (§7): a model glued to the camera has fragment world positions
that are lighting nonsense — inside whatever wall the player leans on, in no
reflection-probe volume, at no lightmap texel. Source's fix is two lines: light
the weapon at the owner's chest.

The cromwell version, done at the shader rather than the material:

- One uniform, `vec4 uLightingOrigin` (`w` = enabled). When enabled, every
  **positional** lighting input — the sun shadow test, the lightmap sample,
  reflection-probe selection, transmission — is evaluated at that world point
  instead of the fragment's position. **Normals and the BRDF stay
  per-fragment**: the model still shades directionally, still catches the sun
  at a grazing angle. Only "where in the world am I lit from" is overridden.
- API mirrors the existing `setDecalsEnabled` push/restore idiom:

  ```cpp
  pbr.setLightingOrigin(anchorPoint);   // before the foreground draws
  pbr.clearLightingOrigin();            // after
  ```

**This buys something Source never had.** Source view models neither cast nor
receive shadows — §7 is explicit that this fell out of the architecture rather
than being chosen. Here, shadow *receiving* works: the shadow test runs at the
anchor point, so the weapon uniformly darkens when the player walks into
shade, which is most of what shadow-awareness is worth on a first-person
model.

**Shadow *casting* stays deliberately absent.** The foreground model is simply
never submitted to the shadow pass. The honest alternative (§10) is UE 5.5+'s
world-space shadow proxy, which costs a GBuffer bit and assumes a deferred
renderer — it does not fit this forward renderer and is not worth building for
a first-person arm. If a game someday needs the weapon's shadow on a wall, the
cheap trick is a world-space proxy mesh drawn only into the shadow map, and it
can be that game's decision.

---

## 6. Verification

- **Unit tests** (`xcom_tests`, CPU-only) for Block 2: attachment-warp
  round-trip; FOV-delta behaviour under a world zoom; near-plane and aspect
  invariants. These pin the math that, when wrong, presents as "effects feel
  slightly off" and resists debugging by inspection.
- `cmake --build . --target cromwell` — the engine must build alone; the
  game-independence rule is checked, not trusted.
- Full build, `ctest -C Release`, and `./tools/tidy.sh` stays clean.
- **One throwaway visual check:** a lit test mesh drawn through the pass,
  deliberately pushed halfway into a wall. Correct is: reads as in front of
  the wall, self-occludes, lit like the room the anchor point is in. Verified
  by a human looking at it — then the harness code is deleted. No permanent
  game-side feature ships with these blocks.
- Profiler: the `foreground` zone appears as a sibling of `lit scene` from the
  first commit, per the instrumentation rule. One zone — the pass will cost a
  fraction of a percent and has not earned sub-zones.

---

## 7. Deferred, and what "Lets make a first-person player" needs later

The blocks above make the *render* half of a first-person view an engine
capability. The remaining work, in the order it would land:

1. **Skeletal animation** — the real prerequisite, and a whole system: ozz
   integration, GPU skinning, clip playback and blending. A viewmodel without
   animation is a screenshot. Blocks nothing above; blocked by nothing above.
2. **Attachment sockets and bonemerge** — animation-system features (§9: the
   c_model architecture hangs weapon and cosmetics off one arm rig by bone
   name). The attachment warp (Block 2) is already waiting for them: socket
   position in, world-space effect position out.
3. **Game-side, trivial once 1–2 exist:** a first-person camera pawn
   (`OrbitCamera` shows the pattern), bob/sway/lowering as procedural offsets
   (§4 quotes the exact functions, including the eighteen-year-old disabled
   TF2 sway), and the weapon state machine driving clips.

Rough size for the four blocks: 1 and 3 are small and mostly header
commentary; 2 is math plus its tests; 4 is one shader edit plus uniform
plumbing. Days, not weeks — the study already made the design decisions.
