/* SplashMaps.hpp — turns painted masks into the textures the splash samples.
 *
 * SINGLE RESPONSIBILITY: read the painted masks, fill in whatever was not
 * supplied, and pack them into as few textures as the shader needs.
 *
 * WHY SEPARATE FILES RATHER THAN PRE-PACKED CHANNELS. Because that is how masks
 * are actually painted. Nobody opens an image editor and thinks in channels —
 * they paint white where the water is, save it, and paint the next one if they
 * need to. Asking for a pre-packed RGB file makes the easy case as much work as
 * the hard one, and every reload another chance to have packed a channel into
 * the wrong slot. The packing is arithmetic; the shader wants few fetches, so it
 * happens here, once, at load.
 *
 *     textures/cromwell_water_mask.png        REQUIRED for any water.
 *                                             White where the river is.
 *     textures/cromwell_water_depth_mask.png  optional. WHITE IS NEAR, black is
 *                                             the far end of the river.
 *     textures/cromwell_water_chop_mask.png   optional. White where the surface
 *                                             is more agitated.
 *
 *     textures/cromwell_sky_mask.png          optional. White where the sky is.
 *     textures/cromwell_sky_depth_mask.png    optional. WHITE IS NEAR — the sky
 *                                             overhead — black at the horizon.
 *
 * TWO TEXTURES, NOT ONE, and not four. Water and sky are packed separately
 * because they are sampled under different conditions and because the sky
 * briefly rode in the water map's spare alpha channel, which worked and read
 * appallingly: a function called "the water map" cannot hold the sky without
 * every later reader having to be told. Splitting them costs one sampler in a
 * shader that already takes over a hundred fetches a pixel.
 *
 * DEPTH IS THE SAME IDEA IN BOTH, and the convention is worth stating once:
 * WHITE IS NEAR. It is the reciprocal of that value which gives distance, and
 * distance is what makes motion behave — near things move quickly across the
 * eye and far things crawl, which is the whole difference between a river and a
 * scrolling texture. See riverCoord in the shader.
 *
 * WHAT IT FILLS IN. A depth ramp is the tedious mask to paint and the easiest
 * to guess: the coverage mask already says where the thing is, so "how near is
 * this" can be approximated by how far down its own column it sits. That guess
 * is decent for a river receding up the frame and wrong the moment one bends
 * away from the viewer, so it is a fallback and not a plan.
 *
 * IT ALSO FEATHERS, and that part is not optional. A painted mask is a hard
 * edge — the brush either covered a pixel or it did not — and a hard edge in a
 * displacement mask is a seam: ripple on one side, still paint on the other,
 * with nothing in between. A few pixels of blur is the difference between water
 * meeting a hull and water stopping at a line.
 */
#pragma once

#include "raylib.h"

namespace game {

class SplashMaps {
public:
    /* Both packed images. Either may be empty — check with `valid` — and an
     * empty one is an ordinary outcome meaning "nobody painted that", which
     * sends the shader to its procedural fallback for that feature.
     *
     * The caller owns both images and must UnloadImage each valid one. */
    struct Result {
        Image water{};   /* R coverage, G depth (white near), B chop */
        Image sky{};     /* R coverage, G depth (white overhead)     */
    };

    static Result build();

    static bool valid(const Image& image) { return image.data != nullptr; }
};

}  // namespace game
