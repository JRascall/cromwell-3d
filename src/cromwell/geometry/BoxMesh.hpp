/* BoxMesh.hpp — one unit cube on the GPU, drawn wherever a box is needed.
 *
 * SINGLE RESPONSIBILITY: own a 1x1x1 box mesh and draw it under a transform.
 *
 * WHY UNITS ARE NOT IMMEDIATE MODE. Bodies used to be built per frame out of
 * rlVertex3f calls, which was fine when a vertex was a position and a colour.
 * A lit, textured vertex also needs a normal, a tangent and a UV, and rlgl's
 * immediate batch has no channel for most of those — so the choice was to
 * re-upload a mesh every frame or to upload one cube once and move it. This is
 * the second.
 *
 * It costs a handful of extra draw calls (four for a vehicle, one for a
 * soldier) and buys the same shader, the same shadowing and the same material
 * model the baked world gets, which is what stops units reading as stickers on
 * top of a lit scene.
 *
 * The cube's UVs come from the same world-space planar projection the world
 * uses, so a body texture tiles at the same density as everything else. When
 * real character meshes arrive they bring their own UVs and this goes away.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class BoxMesh {
public:
    BoxMesh();
    ~BoxMesh();

    BoxMesh(const BoxMesh&) = delete;
    BoxMesh& operator=(const BoxMesh&) = delete;

    /* Albedo comes from the material's diffuse colour (colDiffuse) times the
     * albedo map, not from the mesh — the cube's own vertex colours are
     * white. */
    void draw(const Material& material,
              float centreX, float centreY, float centreZ,
              float sizeX, float sizeY, float sizeZ) const;

private:
    Mesh mesh_ = { 0 };
    bool built_ = false;
};

}  // namespace cromwell
