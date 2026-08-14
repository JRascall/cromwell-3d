/* MaterialDefinition.hpp — a material's parameters, read from a file.
 *
 * SINGLE RESPONSIBILITY: turn `assets/materials/<name>.mat` into a PbrMaterial.
 *
 * ====================== WHY THIS EXISTS ===================================
 *
 * So that adding a material is a FILE, not a rebuild.
 *
 * The texture half of that was already true — drop `wall_albedo.png` in and the
 * wall picks it up, no code, no shader, no rebuild (see the README beside the
 * textures). The scalar half was not: roughness, metalness, opacity, the
 * Fresnel ramp and the blend mode could only be set by calling a setter from
 * C++, which meant a new kind of glass, a puddle of water or a frosted pane was
 * a code change and a compile.
 *
 * That is the wrong shape and it is not how any modern authoring tool works. A
 * material is an ASSET: a set of PBR inputs and the maps that drive them. What
 * makes water different from a window is the numbers in that asset, not a
 * branch in a renderer.
 *
 * ================ BLEND MODE IS A MATERIAL PROPERTY =======================
 *
 * Which is the part that matters most, and the reason this file can be small.
 * `blend translucent` is what puts a surface in the transparent pass; nothing
 * in C++ decides it and no shader is written for it. One opaque shader and one
 * transparent shader serve every material in the project, exactly as an
 * übershader with a blend-mode switch does elsewhere.
 *
 * So water is a .mat with `blend translucent`, a low roughness and a
 * transmittance colour. Glass is a .mat with a different opacity ramp. Neither
 * is a class, a shader family or a line of code.
 *
 * ====================== ONE PARSER, BOTH RENDERERS ========================
 *
 * It fills a PbrMaterial, which is the description MaterialLibrary already
 * stores per slot and DeviceMaterials already packs from. So a material file
 * means the same thing to the raylib path and the device path by construction
 * rather than by two loaders agreeing — which is the same argument for sharing
 * common/brdf.glsl between their shaders.
 *
 * ========================== FORMAT, AND WHY THIS ONE ======================
 *
 * One `key value` per line, `#` comments, unknown keys warned about and
 * skipped. The same shape assets/models/props.txt already uses, for the same
 * reason its header gives: the point is that an artist can edit it, and a
 * format that needs a schema or a tool to write is a format that gets edited
 * through a programmer.
 *
 * MISSING IS NOT AN ERROR. A material with no .mat file keeps PbrMaterial's
 * defaults — a fairly rough dielectric — which is what every surface in this
 * project had before this existed. Adding a file is opt-in, per material.
 */
#pragma once

namespace cromwell {

struct PbrMaterial;

/* Reads `assets/materials/<name>.mat` into `out`, leaving any field the file
 * does not mention at whatever it already held. Returns false when there is no
 * such file — which is the ordinary case and not a failure; the caller keeps
 * the defaults and says nothing.
 *
 * Anything the file DOES say and gets wrong is warned about by line, because a
 * typo in a material is otherwise a surface that silently looks wrong. */
bool loadMaterialDefinition(const char* name, PbrMaterial& out);

}  // namespace cromwell
