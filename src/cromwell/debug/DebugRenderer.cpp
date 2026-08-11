#include "cromwell/debug/DebugRenderer.hpp"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"
#include "cromwell/math/RaylibInterop.hpp"

#include "raylib.h"
#include "rlgl.h"

namespace cromwell {

namespace {

/* UiColor is linear float RGBA; raylib wants eight-bit sRGB. The scene is
 * tone-mapped, so encoding back the way UiColor decoded is what makes a debug
 * red look like the red that was asked for rather than a washed-out pink. */
Color toRaylibColour(const DebugColour& colour)
{
    const auto encode = [](float linear) {
        const float clamped = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
        const float srgb = clamped <= 0.0031308f
                               ? clamped * 12.92f
                               : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
        return static_cast<unsigned char>(srgb * 255.0f + 0.5f);
    };

    return Color{ encode(colour.r), encode(colour.g), encode(colour.b),
                  static_cast<unsigned char>((colour.a < 0.0f ? 0.0f
                                              : colour.a > 1.0f ? 1.0f : colour.a)
                                             * 255.0f + 0.5f) };
}

void drawPass(const DebugDraw& queue, bool depthTested)
{
    for (const DebugSegment& segment : queue.segments()) {
        if (segment.depthTested != depthTested) continue;
        DrawLine3D(toRaylib(segment.from), toRaylib(segment.to),
                   toRaylibColour(segment.colour));
    }
}

}  // namespace

void DebugRenderer::draw(const DebugDraw& queue) const
{
    if (!visible_ || queue.empty()) return;

    CW_PROFILE_ZONE_N("debug draw");
    CW_GPU_ZONE("debug draw");

    /* Occluded first. */
    drawPass(queue, /*depthTested=*/true);

    /* Then the x-ray pass, over the top. rlDrawRenderBatchActive between the
     * two: rlgl batches by state, and flipping the depth test without flushing
     * would apply the new state to lines already queued under the old one —
     * which shows up as the first few x-ray lines being occluded, apparently at
     * random. */
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    drawPass(queue, /*depthTested=*/false);
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
}

}  // namespace cromwell
