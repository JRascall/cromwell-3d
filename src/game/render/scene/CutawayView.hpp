/* CutawayView.hpp — how much of the world one pass is allowed to draw.
 *
 * SINGLE RESPONSIBILITY: carry the two cuts together — how far up the lattice
 * to draw, and which wall facings to drop — so that a pass takes one argument
 * that means "the camera's view of the world" rather than two that it might
 * get half right.
 *
 * WHY THESE TWO TRAVEL TOGETHER. They are one idea expressed on two axes: the
 * storey cut removes what is ABOVE you, the facing cut removes what is IN
 * FRONT of you, and between them they open a building to the camera. Every
 * pass that wants one wants the other, and a pass that took only the storey
 * would silently draw the near walls back over the interior it had just
 * uncovered.
 *
 * THE DEFAULT IS THE WHOLE WORLD, AND THAT IS THE POINT OF THE TYPE.
 *
 * The bug this design exists to prevent has already happened once. The shadow
 * map used to read the iso level itself, so hiding a storey deleted the roof
 * from the sun's depth pass and the room below it jumped to full sunlight —
 * the lighting became a function of where the camera was cut. The facing cut
 * would have been a worse version of the same fault, because it moves
 * CONTINUOUSLY as the camera rotates: every building's near walls would stop
 * and start casting as you swung round.
 *
 * So the safe value is the one you get for free. A default-constructed
 * CutawayView shows every storey and every facing; a pass has to ASK to cut
 * something. The sun and the probes ask for nothing and are therefore correct
 * by construction rather than by a caller remembering. See FrameRenderer's
 * shadow pass, and ReflectionProbeSet's capture, which had this rule first.
 *
 * WHAT BELONGS HERE AND WHAT DOES NOT. This is the RESULT of the cutaway
 * policy, not the policy. Whether the storey follows the selected unit or a
 * key, and which facings the camera angle implies, are the controller's to
 * decide — see PlayerController::cutawayView. The renderer is handed the
 * answer and never learns the question.
 */
#pragma once

#include "cromwell/style/SurfaceFacing.hpp"

#include <cmath>
#include <limits>

namespace game {

using namespace cromwell;

struct CutawayView {
    /* Storeys at or below this index are drawn. The default reaches past any
     * lattice, so "unset" means "all of them" rather than "storey zero" — a
     * zero default here would have made a forgotten argument silently correct
     * on a one-storey map and catastrophic on a three-storey one. */
    int maxStorey = std::numeric_limits<int>::max();

    /* One bit per SurfaceFacing. Default: every facing drawn. */
    unsigned facings = kAllFacings;

    bool shows(SurfaceFacing facing) const { return (facings & bitOf(facing)) != 0u; }

    /* Explicitly the whole world. Identical to a default-constructed value —
     * it exists so a call site can SAY it means the full lattice instead of
     * passing `{}` and leaving the next reader to work out whether that was
     * deliberate. The sun's pass uses it. */
    static CutawayView whole() { return CutawayView{}; }
};

/* WHICH FACINGS SURVIVE A GIVEN VIEW DIRECTION.
 *
 * A wall is worth removing when it stands between the camera and whatever it
 * encloses — that is, when the camera is on its outward side looking inward,
 * which is exactly when its outward vector opposes the view direction. So a
 * facing is dropped when dot(outward, forward) is negative, and the camera at
 * a tactical three-quarter angle drops the two facings nearest it: the same
 * two sides of every building open at once, which is what XCOM does and why
 * rotating ninety degrees swaps which way you can see in.
 *
 * THE DEADBAND IS NOT A ROUNDING FUDGE. A wall seen edge-on has a dot product
 * near zero and contributes almost nothing to the picture either way, but it
 * sits exactly where the sign flips — so without a bias it would cut and
 * uncut on the noise in the camera's forward vector while the player is merely
 * holding a rotation key. Biasing toward KEEPING means the flicker resolves to
 * a wall that stays up, which is the failure everyone would rather have.
 *
 * `None` cannot be dropped: its vector is zero, so its dot product is zero,
 * which never clears a negative threshold. That is by construction and not by
 * a special case — see SurfaceFacing.hpp.
 *
 * TAKES TWO FLOATS RATHER THAN A Vector3 so this header needs no raylib, which
 * is what lets the test suites — which build without it — check the rule
 * directly. The vertical component is dropped on the way in: a wall is cut by
 * where the camera is standing, not by how steeply it is looking down, and
 * feeding a top-down camera's near-vertical forward vector in would have every
 * facing hovering around the threshold at once. */
inline unsigned facingsVisibleFrom(float forwardX, float forwardZ)
{
    constexpr float kEdgeOnDeadband = -0.05f;

    /* NORMALISED FIRST, so the deadband is an ANGLE rather than a distance.
     * Flattening a steeply-pitched camera leaves a short xz vector — a
     * near-top-down view might project to a length of 0.1 — and testing an
     * unnormalised dot against a fixed threshold would silently widen that
     * deadband as the player pitched down, until walls stopped cutting for a
     * reason nothing in the code mentions.
     *
     * Straight down is the degenerate case and it falls out correctly: length
     * zero means no horizontal direction to be in front of, so nothing is cut
     * and the storey cut is left to do the work on its own. */
    const float length = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
    if (length < 1e-4f) return kAllFacings;

    const float nx = forwardX / length;
    const float nz = forwardZ / length;

    unsigned mask = 0u;
    for (int i = 0; i < kSurfaceFacingCount; i++) {
        const SurfaceFacing facing = static_cast<SurfaceFacing>(i);
        const float towards = facingX(facing) * nx + facingZ(facing) * nz;
        if (towards >= kEdgeOnDeadband) mask |= bitOf(facing);
    }
    return mask;
}

}  // namespace game
