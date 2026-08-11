#include "game/picking/SurfacePicker.hpp"

#include "cromwell/math/RaylibInterop.hpp"

#include "game/picking/WorldTrace.hpp"

#include <cmath>

namespace game {

namespace {

/* WHAT A DECAL MAY STICK TO, taken from the project's layer table rather than
 * restated here. The rule that windows are excluded — a mark inside a 6%-opaque
 * pane is a smear hanging in the opening, and pbr.fs.glsl refuses decals on
 * blended surfaces anyway — now lives in defaultLayerMatrix() with every other
 * response, where it can be read beside the ones it has to be consistent with.
 *
 * Built once. It is configuration, and rebuilding the table per pick would put a
 * few hundred bytes of setup in front of every frame's cursor. */
const cromwell::TraceFilter& paintFilter()
{
    static const cromwell::TraceFilter filter =
        defaultLayerMatrix().filterFor(layer::kPaint);
    return filter;
}

}  // namespace

std::optional<SurfaceHit> SurfacePicker::pick(const cromwell::Ray& ray, int maxStorey) const
{
    /* THE FIXED-STEP MARCH IS GONE, and with it the reason this file used to
     * carry a kStep constant an order of magnitude finer than TilePicker's. That
     * step existed because sampling can walk past a 9 cm wall, and the answer was
     * always to sample harder — fourteen thousand times over a 140 m ray — which
     * made it slower without ever making it exact.
     *
     * The trace visits the cells the ray genuinely crosses and solves each
     * contact in closed form. There is no step, so there is nothing to tune and
     * nothing to miss. See game/picking/WorldTrace.hpp. */
    WorldTrace trace(world_);

    WorldTrace::Params params;
    params.start = ray.origin;
    params.direction = ray.direction;
    params.maxDistance = kMaxDistance;
    params.filter = paintFilter();
    params.maxStorey = maxStorey;

    const std::optional<cromwell::TraceHit> found = trace.single(params);
    if (!found) return std::nullopt;

    SurfaceHit hit;
    hit.point = cromwell::toRaylib(found->point);
    hit.normal = cromwell::toRaylib(found->normal);
    hit.cell = Cell{ found->cellX, found->cellY, found->cellZ };

    /* Vertical means "a wall face", which is a question about the normal rather
     * than about which layer answered — a ramp steep enough to be a wall would
     * want the shallow projection too. Half is the 45-degree line, which is also
     * where the tile data stops calling something a ramp (see Constants.hpp). */
    hit.vertical = std::abs(found->normal.y) < 0.5f;

    /* The point comes back ON the surface, which is where the trace put it; the
     * old march reported the wall plane and then pushed out by half a wall's
     * thickness by hand. The slab the trace tests already has that thickness, so
     * the contact is on the visible face by construction. */
    return hit;
}

}  // namespace game
