/* WorldTrace.hpp — sweeping a shape through the tile world.
 *
 * SINGLE RESPONSIBILITY: connect cromwell's trace layer to THIS game's
 * geometry. It owns the answer to one question the engine cannot have an
 * opinion about — what solid shapes a tile contains — and delegates everything
 * else.
 *
 * WHAT IT REPLACES. The pickers march a ray in fixed steps, sampling every 3 cm
 * or every 1 cm over 140 metres. That is thousands of samples to cross a few
 * dozen cells, and — the part that actually bites — a step larger than the
 * geometry can jump clean over it, which SurfacePicker's own header admits.
 * This walks the cells the shape genuinely touches, in order, and solves each
 * contact in closed form. No step size, nothing to miss.
 *
 * ==================== WHY THE TRAVERSAL AND THE TEST DIFFER ================
 *
 * THE WALK HAPPENS IN LATTICE SPACE; THE CONTACT IS SOLVED IN WORLD SPACE.
 *
 * A lattice cell is one unit across in x and y and kCellHeight (two thirds) in
 * z, so cells are not cubes and cromwell's grid traversal — which assumes unit
 * cubes, because that is the only assumption that keeps it genre-neutral —
 * cannot walk them directly. Scaling height by kCellHeight makes them cubes and
 * the walk exact.
 *
 * But that scale is ANISOTROPIC, and a sphere in world space is an ellipsoid in
 * lattice space. So the walk uses a lattice-space box that BOUNDS the query
 * shape — a superset of the cells it could touch, which is all a traversal has
 * to be — and every contact is then solved against the cell's real world-space
 * geometry. Conservative walk, exact test: the same shape as every broadphase/
 * narrowphase split, for the same reason.
 *
 * ========================= WHAT A TILE IS MADE OF =========================
 *
 * Six kinds of solid, and each gets a layer so a caller can choose:
 *
 *   FLOOR   the walk surface. A thin slab whose top is the surface height.
 *   RAMP    an inclined plane spanning the tile — NOT a box, and the one shape
 *           here that is not axis-aligned. Solved as a plane, offset by the
 *           query shape's support along the plane normal, which is exact for a
 *           sphere and for a box alike.
 *   MASS    a fully blocked cell, from its base to the top of its column.
 *   WALL    a face between two tiles, as a thin vertical slab at the boundary.
 *   WINDOW  a wall you can see and shoot through. Its own layer precisely so
 *           that a bullet can be told to pass it while a camera probe is not.
 *   CANOPY  the roof plane at the top of a cell.
 *
 * UNITS ARE A SEPARATE LAYER and are tested by walking the roster rather than an
 * index. That is not an oversight: a roster is dozens of entries, each rejected
 * by a bounding-box test that costs less than a spatial-hash bucket walk, and
 * CLAUDE.md is explicit that the entity layer does not get the treatment the
 * spatial query layer does. When there are thousands of bodies, put them in a
 * SpatialHash and use its querySegment — the call exists for exactly this, and
 * the swap is local to one function here.
 *
 * ======================== WHAT THIS DOES NOT REPLACE =======================
 *
 * TilePicker STAYS AS IT IS, and deliberately. It answers a GAMEPLAY question —
 * "which cell is the player pointing at" — using a crossing test over the walk
 * surface that deliberately ignores walls and reports only standable ground.
 * That is not a collision query wearing a different hat; it is movement's own
 * definition of the world, and rewriting it in terms of solids would change
 * which cells are pickable. If it is ever rebuilt on this, that is a change to
 * gameplay and wants its own pass and its own before-and-after.
 *
 * SurfacePicker IS rebuilt on this, because its question — "what geometry is
 * under the cursor, as a point and a normal" — is exactly a trace, and its
 * fixed-step march was the implementation, not the meaning.
 */
#pragma once

#include "cromwell/collision/LayerMatrix.hpp"
#include "cromwell/collision/Shape.hpp"
#include "cromwell/collision/TraceFilter.hpp"
#include "cromwell/collision/TraceHit.hpp"

#include "game/world/World.hpp"

#include <optional>

namespace game {

class UnitRoster;

/* THE PROJECT'S LAYERS. Declared here rather than in the engine, for the reason
 * Layer.hpp gives at length: what a layer means is a game's decision, and an
 * engine that shipped a "Wall" layer would have made it for every project that
 * embedded it. */
namespace layer {

inline constexpr cromwell::LayerId kFloor{ 0 };
inline constexpr cromwell::LayerId kRamp{ 1 };
inline constexpr cromwell::LayerId kMass{ 2 };
inline constexpr cromwell::LayerId kWall{ 3 };
inline constexpr cromwell::LayerId kWindow{ 4 };
inline constexpr cromwell::LayerId kCanopy{ 5 };
inline constexpr cromwell::LayerId kUnit{ 6 };

/* The tracers — what a query IS, so its rules can be looked up rather than
 * restated. See LayerMatrix::filterFor. */
inline constexpr cromwell::LayerId kCursor{ 8 };   /* what the mouse points at   */
inline constexpr cromwell::LayerId kSight{ 9 };    /* line of sight              */
inline constexpr cromwell::LayerId kShot{ 10 };    /* a bullet                   */
inline constexpr cromwell::LayerId kBody{ 11 };    /* a moving body's own volume */
inline constexpr cromwell::LayerId kPaint{ 12 };   /* where a decal may stick    */

}  // namespace layer

/* Every surface a trace can report, as a mask. The shorthand a hand-built filter
 * starts from. */
cromwell::LayerMask allSurfaces();

/* THE GAME'S COLLISION RULES, in one function. Unity's Layer Collision Matrix
 * and Unreal's channel responses live in a settings screen; this project has no
 * settings screen yet, so they live here — one place, readable, and the thing
 * to edit when "why does a shot stop at a window" comes up.
 *
 * Built once and held; it is configuration, not something to rebuild per
 * query. */
cromwell::LayerMatrix defaultLayerMatrix();

class WorldTrace {
public:
    /* `roster` may be null, for a trace that only cares about geometry. Passing
     * one does not by itself make units block anything — the filter decides. */
    explicit WorldTrace(const World& world, const UnitRoster* roster = nullptr)
        : world_(world), roster_(roster) {}

    /* ONE-SHOT DATA CARRIER — everything a trace needs, so the call site reads
     * as a description rather than as eight positional arguments. */
    struct Params {
        cromwell::Vec3 start;

        /* Need not be unit length; it is normalised on the way in, and
         * `maxDistance` is always in metres regardless. A caller with a start
         * and an end point uses `between` below rather than doing that
         * arithmetic itself. */
        cromwell::Vec3 direction;
        float maxDistance = 0.0f;

        cromwell::TraceShape shape = cromwell::TraceShape::ray();
        cromwell::TraceFilter filter = cromwell::TraceFilter::blockAll();

        /* The floor-isolation ceiling. Cells above this storey are not there as
         * far as the trace is concerned — which is what makes a cursor pick
         * agree with what the cutaway is showing. */
        int maxStorey = 1 << 20;

        /* One id the trace passes straight through, for the body that fired it.
         * See TraceFilter.hpp on why this is a single id and not a list. */
        int ignoreId = -1;
    };

    /* Start and end points instead of a direction and a length. */
    static Params between(cromwell::Vec3 from, cromwell::Vec3 to);

    /* The nearest BLOCKING contact, or nothing.
     *
     * Overlaps are not reported here — a single trace has one answer and an
     * overlap is by definition not it. Use `multi` when the things passed
     * through matter. */
    std::optional<cromwell::TraceHit> single(const Params& params) const;

    /* Everything along the line, nearest first, stopping at the first blocking
     * contact — which is itself included as the last hit.
     *
     * `out` is CLEARED and refilled. It bounds the result: check
     * `out.overflowed()` if the count could matter. */
    void multi(const Params& params, cromwell::TraceHits& out) const;

    /* Is the straight line between two points clear of everything the filter
     * blocks? The line-of-sight question, and cheaper than `single` because it
     * stops at the first blocker rather than ranking them. */
    bool clearBetween(cromwell::Vec3 from, cromwell::Vec3 to,
                      const cromwell::TraceFilter& filter, int maxStorey = 1 << 20) const;

private:
    /* Walks the cells and calls back for every contact the filter cares about,
     * in slab order. The one implementation both public traces run on — see the
     * ordering note in cromwell/collision/GridTrace.hpp for why a caller must
     * not stop at the first contact it is handed. */
    template <typename OnHit>
    void sweep(const Params& params, OnHit&& onHit) const;

    const World& world_;
    const UnitRoster* roster_ = nullptr;
};

}  // namespace game
