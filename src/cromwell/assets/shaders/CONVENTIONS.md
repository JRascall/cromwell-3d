# Shader conventions

Every shader in this engine is being moved to one dialect. This file is that
dialect, and the reason for each rule.

Read it before writing a new shader or converting an old one. Converting the
same stage twice because the target moved is the specific waste this exists to
prevent.

## Why this is changing at all

The shaders are `#version 330` GLSL with loose uniforms set by name. That is
correct GL and it is **unusable anywhere else**:

- **SPIR-V has no default uniform block.** A bare `uniform float uExposure;`
  cannot be expressed. Every cross-compiler — glslang, Slang, DXC — rejects it,
  so the choice of toolchain never mattered while the dialect was this one.
- **`rhi::ICommandEncoder` binds by numbered slot**, not by name, because
  `glGetUniformLocation` at draw time is a GL/D3D reflection habit the explicit
  APIs do not have. A shader with no `layout(binding=)` cannot be driven by the
  interface the renderer is moving onto.

So the conversion is a prerequisite for the console port **and** for the render
passes migrating to `IRenderDevice`. It is one piece of work serving both, which
is why it comes first.

## The rules

### 1. `#version 450 core`

Not 330. 450 is what glslang consumes for Vulkan-flavoured SPIR-V, and GL 4.3 —
the engine's floor — accepts everything below through `ARB_explicit_uniform_location`
and `ARB_shading_language_420pack`, both core by 4.3.

### 2. Every input and output has an explicit location

```glsl
layout(location = 0) in  vec3 inPosition;
layout(location = 1) in  vec3 inNormal;
layout(location = 2) in  vec2 inTexCoord;

layout(location = 0) out vec4 outColour;
```

Matching by name is a reflection feature. `rhi::VertexAttribute::location` is
the contract on the C++ side, and these two are read together.

### 3. No loose uniforms. Ever.

Everything scalar goes in a `std140` uniform block, or in push constants.

```glsl
layout(std140, binding = 1) uniform PassBlock {
    mat4  uView;
    mat4  uProjection;
    vec4  uCameraPosition;   /* vec3 padded - see the std140 note below */
    float uExposure;
};
```

**`std140` padding is not optional and it bites.** A `vec3` occupies four floats
of alignment, a `mat4` starts on a 16-byte boundary, and an array of `float` has
each element padded to 16 bytes. A C++ struct that does not match writes garbage
into the fields after the first mismatch — no error, no warning, a plausible
wrong picture. **Declare `vec4` and ignore `.w` rather than declaring `vec3`.**

### 4. Uniform block bindings are allocated by frequency

Slot numbers are global across the engine, so a block can stay bound across
several passes instead of being re-uploaded per draw.

| Binding | Block | Changes |
|---|---|---|
| 0 | `FrameBlock` | once a frame — time, screen size, sun |
| 1 | `PassBlock` | once a pass — view, projection, camera position |
| 2 | `MaterialBlock` | per material — factors, transmission, options |
| 3 | `ObjectBlock` | per object, when not a push constant |

### 5. Samplers get explicit bindings, numbered per pass

```glsl
layout(binding = 0) uniform sampler2D uSceneColour;
layout(binding = 1) uniform sampler2D uSceneDepth;
```

These match `ICommandEncoder::bindTexture(slot, texture, sampler)` exactly.

**A shadow map is sampled with `sampler2DShadow`**, not `sampler2D` plus a
manual compare — that is what `SamplerDesc::compare` creates, and it gets the
hardware's percentage-closer filtering for free instead of four taps by hand.

### 6. Small per-draw values are push constants

```glsl
layout(location = 0) uniform vec4 uPushConstants[8];   /* 128 bytes, the guaranteed floor */
```

`ICommandEncoder::pushConstants` emulates these on GL at uniform location 0.
Cap is 128 bytes because that is what every target guarantees and none much
exceeds. Anything larger belongs in a uniform buffer, where the caller can see
it is paying for one.

### 7. Names are ours, not raylib's

`texture0`, `colDiffuse`, `mvp` and `fragTexCoord` are raylib's conventions and
only mean anything while raylib is binding them. Rename on conversion:
`uSceneColour`, `uBaseColour`, `uModelViewProjection`, `inTexCoord`.

### 8. `#include` still works

`ShaderLibrary::preprocess` splices includes before compilation, and the shared
`common/` files stay as they are. glslang has its own include handling for the
offline path; both read the same files.

## The offline path (planned, not yet built)

```
*.glsl  --glslang-->  *.spv  --SPIRV-Cross-->  GLSL 430 / MSL / console
```

SPIR-V is the interchange rather than the shipped form on PC: the GL backend
cross-compiles back to GLSL because `ARB_gl_spirv` is unevenly implemented
across drivers. Console backends consume their own bytecode, compiled by their
own toolchain from the same SPIR-V.

Authoring stays GLSL. Nobody has to learn a new shading language for the port.

## Converting an existing shader

**A shader, its C++ pass, and the targets it reads and writes all move in one
commit.** This is not tidiness — it is forced, and it is worth understanding
before planning the work.

raylib binds shader inputs *by its own naming convention*: `DrawTexturePro`
supplies `texture0`, `colDiffuse` and `mvp`, and `SetShaderValue` finds uniforms
by name through `glGetUniformLocation`. A shader converted to explicit bindings
and a `std140` block has none of those names and no default uniform block, so
**raylib can no longer drive it at all**. The pass must bind its own textures and
upload its own buffer, which means going through `ICommandEncoder` — and that
means the render target it draws into has to be an `rhi::TextureHandle` rather
than a raylib `RenderTexture2D`.

The tone map is the worked example of why: it reads `HdrTarget`, which the scene
pass writes with raylib. Converting the tone map alone would need `HdrTarget` to
be a device texture, which the raylib scene pass then could not render into.

So the shader conversion **is** the renderer migration; they are one piece of
work, not two that can be sequenced. The practical consequence is that it should
be built as a parallel path — the device-backed renderer alongside the existing
one, selected at startup — so the game keeps running and the two can be compared
directly, rather than an in-place rewrite with a broken week in the middle.

1. Bump to `#version 450 core`, add explicit locations to every in/out.
2. Collect the loose uniforms into the right block by frequency (rule 4).
3. Mirror the block as a C++ struct with `static_assert(sizeof(...) == ...)` and
   explicit padding. **Write the assert before trusting the layout.**
4. Give samplers bindings and engine names.
5. Update the pass to upload the block instead of calling `SetShaderValue`.
6. Run `xcom.exe --device-selftest`, then look at the frame.

## Status

| Stage | Converted |
|---|---|
| all 33 | no |

Nothing is converted yet. This file is the target; the conversion is the work.
