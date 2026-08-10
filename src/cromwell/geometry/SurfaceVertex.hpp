/* SurfaceVertex.hpp — one vertex of lit world geometry.
 *
 * SINGLE RESPONSIBILITY: name the channels a lit, textured vertex carries, so
 * the box emitter and the buffer that receives it agree without a
 * fifteen-argument function signature between them.
 *
 * THE TEXCOORD IS A TEXCOORD AGAIN. It used to smuggle roughness and metalness
 * — a genuinely good trick while every surface was an untextured box, because
 * it made a per-surface material cost no extra attribute and no extra draw
 * call. It cannot survive textures: a normal map has to be sampled somewhere,
 * and that somewhere is the UV. Roughness and metalness moved to the material,
 * where a real workflow keeps them anyway (a scalar factor times an mrao map),
 * and the batching they used to buy is now bought by grouping geometry per
 * material instead.
 *
 * THE TANGENT IS A vec4. xyz is the direction u increases in world space; w is
 * the handedness the bitangent is reconstructed with, cross(N, T) * w. That is
 * the standard packing, and it is what glTF and every normal-map baker emit,
 * so imported meshes will arrive already speaking it.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

struct SurfaceVertex {
    Vector3 position;
    Vector3 normal;
    Vector4 tangent;    /* xyz = u direction, w = bitangent handedness */
    Vector2 uv;
    Color   colour;
};

}  // namespace cromwell
