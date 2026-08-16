/* BloomTuning.hpp — the four numbers a bloom is authored with.
 *
 * SINGLE RESPONSIBILITY: carry the knobs, with defaults that look right on a
 * linear pipeline. It decides nothing and names no graphics API; the pass is
 * ScenePipeline's and the panel that moves these is the game's.
 *
 * ============= WHY A STRUCT RATHER THAN FOUR FIELDS ON SceneFrame =========
 *
 * Because a dev panel needs something to hold a REFERENCE to, and a frame
 * description is rebuilt every frame from scratch. `AmbientOcclusion::Tuning`
 * is the same shape for the same reason and this is deliberately its sibling:
 * the renderer owns one, the panel edits it in place, and the values are copied
 * into the frame at submission. A slider writing into a temporary would move
 * and revert on the next frame, which reads as the slider being broken.
 *
 * ==================== THE DEFAULTS ARE FOR A LINEAR SCENE =================
 *
 * All four assume the scene target holds unbounded linear radiance, which is
 * what RGBA16F is there for. A threshold of 1.0 is NOT "the top of the range":
 * a fully lit white wall sits near one, and the sun and anything emissive go
 * well past it. That is exactly the point — the threshold sits where ordinary
 * lit surfaces end and light SOURCES begin, and it only means that on a linear
 * pipeline. Copied onto a display-colour image where everything is clamped to
 * one, the same number turns bloom off entirely.
 */
#pragma once

namespace cromwell {

struct BloomTuning {
    /* WHERE GLOW STARTS, in linear radiance. Above a lit white surface and
     * below anything meant to read as a lamp — see the note above on why 1.0
     * means that here and would mean "off" on a tonemapped image. */
    float threshold = 1.1f;

    /* HOW WIDE THE RAMP INTO IT IS. Zero is a hard cut, which pops visibly as a
     * surface drifts across the threshold under a moving camera; the knee
     * spreads that over a band so the transition is continuous. See
     * bloom_prefilter.fs.glsl, where the curve is. */
    float knee = 0.55f;

    /* HOW MUCH OF THE RESULT REACHES THE SCENE. Applied once, at the composite,
     * rather than per level — the shader's note says what compounding it would
     * do to the tail.
     *
     * SMALL ON PURPOSE. Bloom is added to a frame that is already correctly
     * exposed, so anything much above this reads as fog rather than as glow,
     * and the usual response to that is to lower the exposure — which is
     * dimming the whole image to hide one term. */
    float intensity = 0.06f;

    /* HOW FAR EACH UPSAMPLE REACHES, in texels of the level it reads. One is
     * plain bilinear magnification and the tightest useful glow; larger widens
     * the tail without adding levels. Beyond about three the tent starts to
     * show its own square shape. */
    float radius = 1.0f;
};

}  // namespace cromwell
