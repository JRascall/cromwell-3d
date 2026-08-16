/* ObjectPush.hpp — where one drawn thing is, and what colour it is.
 *
 * SINGLE RESPONSIBILITY: be the C++ half of assets/shaders/rhi/object.glsl.
 *
 * ================== INTERNAL. NOT PART OF THE PUBLIC SURFACE ==============
 *
 * IT USED TO LIVE IN IGeometrySource.hpp, on the reasoning that it was the
 * SEAM'S payload: the engine's shaders read it and the game's submitters wrote
 * it, and that interface was where the two met.
 *
 * THE GAME NO LONGER WRITES ONE. A renderable carries a transform and a tint as
 * data, and ScenePipeline::drawItems packs them here on the way to the encoder
 * — one loop, which is why nothing can forget to push and inherit the previous
 * draw's matrix. So this stopped being a shared contract and became an engine
 * detail the moment the scene landed, and it moved out of the dying header
 * rather than being deleted with it.
 *
 * IT IS ONE CONTRACT WRITTEN TWICE, here and in the shader, and there is no
 * reflection on the explicit backends to check the two agree. The check is that
 * both files say so and that the static_assert below pins the size.
 *
 * PUSHED PER DRAW, and cheap enough to: it is eighty bytes into the eighty-byte
 * path every backend reserves for exactly this. Anything bigger, or anything
 * that does not change per draw, belongs in a uniform buffer instead.
 */
#pragma once

#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec3.hpp"

namespace cromwell {

struct ObjectPush {
    /* Column-major, matching Mat4's own layout and GLSL's mat4-from-columns
     * constructor — so the sixteen floats cross unpermuted and nothing
     * transposes anywhere. */
    Mat4 model;

    /* MULTIPLIES the vertex colour. White leaves the mesh's own colours alone,
     * which is what the static world wants; a body's cube is white and takes
     * its whole colour from here. */
    float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    /* ---- WHAT THIS OBJECT IS, for the one pass that asks ------------------
     *
     * x = the custom stencil value, 0-255, as a float. Written by the custom
     * depth pass into a colour channel and unread by every other shader.
     *
     * HERE RATHER THAN IN A UNIFORM BLOCK because it varies PER DRAW, which is
     * exactly what the push path is for — see the note at the top of this
     * header. A block would be an upload between draws inside a pass, which is
     * the stall DeviceMaterials refuses.
     *
     * A FLOAT FOR AN INTEGER, and it is exact: push constants are emulated as a
     * vec4 uniform on GL, values to 2^24 are represented exactly, and 255 is a
     * long way inside that. Adding an int lane to a struct every other shader
     * reads as floats would be a layout for one consumer. yzw spare. */
    float object[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    /* Named constructors, because the two callers are "this mesh is already in
     * world space" and "put this box here" and neither should be building a
     * matrix by hand at the call site. */
    static ObjectPush identity() { return ObjectPush{}; }

    static ObjectPush box(Vec3 centre, Vec3 size, float r, float g, float b)
    {
        ObjectPush push;

        /* SCALE THEN TRANSLATE. The other order scales the translation too, and
         * puts every body somewhere out along the ray from the origin through
         * where it should have been — which reads as "the units are in the
         * wrong place" rather than as a matrix order mistake. */
        push.model = Mat4::translation(centre) * Mat4::scaling(size);

        push.tint[0] = r;
        push.tint[1] = g;
        push.tint[2] = b;
        return push;
    }
};
static_assert(sizeof(ObjectPush) == 96, "rhi/object.glsl reads six vec4s");

}  // namespace cromwell
