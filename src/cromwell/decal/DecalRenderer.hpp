/* DecalRenderer.hpp — fill the DBuffer.
 *
 * SINGLE RESPONSIBILITY: own the decal program and the projector box, set the
 * render state the DBuffer blend needs, and issue one draw per decal. It owns
 * neither the decals nor the buffer.
 *
 * THE STATE IS THE INTERESTING PART, and all four pieces of it are load-bearing:
 *
 *   BACK FACES ONLY. Front faces vanish the moment the camera enters the box,
 *   which for a decal the size of a road tile is most of the time. Back faces
 *   are always present and always cover the box's full screen extent.
 *
 *   NO DEPTH TEST, NO DEPTH WRITE. The box is a bounding volume, not geometry;
 *   which fragments survive is decided in the shader against the box's own
 *   bounds. Leaving the test on would reject exactly the fragments whose
 *   receiving surface sits in front of the box's far side — which is all of
 *   them.
 *
 *   SEPARATE BLEND FACTORS. rgb over-blends premultiplied while alpha
 *   MULTIPLIES, so the buffer accumulates the decals' colour in rgb and the
 *   base material's surviving fraction in alpha. One equation, any number of
 *   overlapping decals, and the lit shader's decode is a single fused multiply
 *   -add. See DecalBuffer.hpp.
 *
 * ONE DRAW PER DECAL. A tactical board carries tens of these, not thousands,
 * and each is twelve triangles with the depth test off — the cost is fill, not
 * submission. Instancing is what this grows into if a map ever wants hundreds;
 * nothing about the shader would have to change.
 */
#pragma once

#include "raylib.h"

#include "cromwell/decal/DecalBuffer.hpp"
#include "cromwell/decal/DecalSet.hpp"

namespace cromwell {

class DecalRenderer {
public:
    DecalRenderer() = default;
    ~DecalRenderer();

    DecalRenderer(const DecalRenderer&) = delete;
    DecalRenderer& operator=(const DecalRenderer&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0 && cube_.vertexCount != 0; }

    /* Clears the buffer and draws every decal into it. Call after the geometry
     * prepass — whose two attachments are the `sceneDepth` and `sceneNormals`
     * arguments — and before the lit pass, outside any target scope.
     *
     * `camera` must be the one the prepass was rendered with: the shader
     * unprojects that depth buffer, so a mismatched matrix would place every
     * decal on a surface that is not there.
     *
     * `preview` is an optional extra decal drawn LAST, so it composites over
     * every committed one. It is how the dev tool's ghost works, and it is a
     * plain Decal rather than anything special: a preview that went through a
     * different path would be predicting a different thing from the one it is
     * supposed to predict. Its material must be registered on `decals`. */
    void render(const DecalSet& decals, const Camera3D& camera,
                const DecalBuffer& buffer,
                Texture2D sceneDepth, Texture2D sceneNormals,
                const Decal* preview = nullptr) const;

private:
    void draw(const Decal& decal, const DecalSet& decals) const;

    Shader shader_ = { 0 };

    /* The unit cube every decal is drawn as, scaled and oriented by its own
     * transform. One mesh for the whole system — a decal's shape is its
     * texture's business, never its geometry's. */
    Mesh cube_ = { 0 };

    /* Mutated per decal and per frame. Mutable for the same reason
     * RibbonShader's is: swapping a texture into a map slot changes GPU state,
     * not this object's meaning. */
    mutable Material material_ = { 0 };

    int locInverseModel_ = -1;
    int locInverseViewProjection_ = -1;
    int locResolution_ = -1;
    int locTint_ = -1;
    int locFactors_ = -1;
    int locFade_ = -1;
    int locWrap_ = -1;
};

}  // namespace cromwell
