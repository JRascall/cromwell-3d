/* IMaterialQuery.hpp — the one thing the scene has to ask a material.
 *
 * SINGLE RESPONSIBILITY: answer, for a MaterialId, whether the surface belongs
 * in the blended pass. Nothing else.
 *
 * ============ WHY AN INTERFACE FOR A SINGLE BOOLEAN ======================
 *
 * Because RenderScene needs exactly this one bit and nothing else from the
 * material system, and naming the dependency as one method rather than as a
 * whole table buys two concrete things.
 *
 * IT KEEPS THE SCENE HEADLESS. DeviceMaterials' implementation reaches
 * PbrMaterial, which names raylib types, so it is compiled into the renderer
 * target and cannot be linked by a test binary that has no window. A scene that
 * called it directly would drag culling, filtering and sorting behind that same
 * wall — and those are pure arithmetic over boxes and integers, which is
 * precisely the code most worth being able to assert about without opening a
 * window. See the headless discipline note at the top of CMakeLists.txt.
 *
 * IT KEEPS §4.7 OPEN. The material system is being rewritten — authored `.mat`
 * files, per-material shaders, shading models, instances — and the scene must
 * not have to change when it is. One method is a seam that survives that; a
 * reference to today's table is not.
 *
 * WHY BLEND MODE AND NOT A PASS NAME. `window.mat` says `blend translucent` and
 * that is the ONLY thing that puts a surface in the transparent pass — no C++
 * names the surface, so water is a material rather than a feature. The scene
 * asks what a material IS and derives which bucket that implies; it never asks
 * which pass something is drawn in, because the answer to that is the
 * pipeline's and changes with the pipeline.
 */
#pragma once

#include "cromwell/material/MaterialId.hpp"

namespace cromwell {

class IMaterialQuery {
public:
    virtual ~IMaterialQuery() = default;

    /* SOMETHING YOU CAN SEE THROUGH, which must be drawn after the opaque scene
     * because it reads what is already in the colour buffer.
     *
     * AN INVALID ID MUST ANSWER `false` rather than assert. A renderable whose
     * material was never set still draws — with the pipeline's default block —
     * and drawing it in the opaque pass is the safe half of that: an unknown
     * surface that landed in the blended pass would be drawn without depth
     * writes and composite over whatever happened to be behind it, which looks
     * like a transparency bug rather than like a missing assignment. */
    virtual bool isTranslucent(MaterialId material) const = 0;
};

}  // namespace cromwell
