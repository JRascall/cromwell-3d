#include "game/picking/WorldTrace.hpp"

#include "cromwell/collision/GridTrace.hpp"
#include "cromwell/collision/Intersect.hpp"

#include "game/query/BlockedMass.hpp"
#include "game/query/Terrain.hpp"
#include "game/units/kinds/Unit.hpp"
#include "game/units/roster/UnitRoster.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using cromwell::Aabb;
using cromwell::LayerId;
using cromwell::LayerMask;
using cromwell::LayerMatrix;
using cromwell::Response;
using cromwell::SweepContact;
using cromwell::TraceFilter;
using cromwell::TraceHit;
using cromwell::TraceHits;
using cromwell::TraceShape;
using cromwell::Vec3;

namespace {

/* Half a full wall's thickness. MATCHES StoreyGeometryEmitter and the value
 * SurfacePicker used, and it has to: a trace that disagreed with the mesh would
 * put a decal floating in front of the wall or sunk behind it. */
constexpr float kWallHalfThickness = 0.045f;

/* How deep a floor slab is, under its walk surface. It exists only so a floor
 * is a volume rather than a plane — a shape sweeping upward from below has to
 * meet something. The top face is the surface height and is what everything
 * actually reads. */
constexpr float kFloorThickness = 0.06f;

/* The roof plane at the top of a cell, same reasoning. */
constexpr float kCanopyThickness = 0.06f;

/* WORLD SPACE TO LATTICE SPACE. The lattice's y is the world's z, and its z is
 * the world's height divided into cells two thirds of a unit tall. Dividing that
 * height out is what makes a cell a unit CUBE, which is the only thing
 * cromwell's traversal knows how to walk. See the header. */
Vec3 toLattice(Vec3 world)
{
    return Vec3{ world.x, world.z, world.y / kCellHeight };
}

/* The world-space box of a cell's full volume, for the pieces that fill it. */
Aabb cellBounds(int x, int y, int z, float lowHeight, float highHeight)
{
    return Aabb{ Vec3{ static_cast<float>(x), lowHeight, static_cast<float>(y) },
                 Vec3{ static_cast<float>(x) + 1.0f, highHeight, static_cast<float>(y) + 1.0f } };
}

/* The inclined plane of a ramp tile, as a normal and a point on it.
 *
 * A RAMP IS THE ONE PIECE OF TILE GEOMETRY THAT IS NOT AXIS-ALIGNED, which is
 * why it gets a plane rather than a box. The tile data models it as a single
 * flat incline (see Constants.hpp — the visible treads are art), so a plane is
 * not an approximation of it; it IS it. */
void rampPlane(const Tile& tile, int x, int y, Vec3& normal, Vec3& pointOn)
{
    const float rise = tile.rampRise;

    /* Uphill direction, in world axes: North is +z, East is +x. The normal is
     * the gradient of (height - base - rise * fraction), which tilts away from
     * the uphill direction by the rise. */
    switch (tile.rampDir) {
        case Dir::North: normal = Vec3{ 0.0f, 1.0f, -rise }; break;
        case Dir::South: normal = Vec3{ 0.0f, 1.0f, rise }; break;
        case Dir::East:  normal = Vec3{ -rise, 1.0f, 0.0f }; break;
        case Dir::West:  normal = Vec3{ rise, 1.0f, 0.0f }; break;
    }
    normal = normal.normalised();

    /* The downhill corner, where the surface is exactly rampBaseHeight. */
    const float cornerX = (tile.rampDir == Dir::West) ? static_cast<float>(x) + 1.0f
                                                      : static_cast<float>(x);
    const float cornerZ = (tile.rampDir == Dir::South) ? static_cast<float>(y) + 1.0f
                                                       : static_cast<float>(y);
    pointOn = Vec3{ cornerX, tile.rampBaseHeight, cornerZ };
}

/* Sweeps a shape onto a plane, clamped to a tile's footprint.
 *
 * The shape's SUPPORT along the plane normal is what turns a plane test into a
 * volume test: a box touches the plane when its centre is `dot(|n|, h)` away, a
 * sphere when it is `r` away. Both are one dot product, which is why the
 * inclined case costs no more than the boxes do. */
SweepContact sweepOntoPlane(const TraceShape& shape, Vec3 start, Vec3 direction,
                            float maxDistance, Vec3 normal, Vec3 pointOn,
                            float minX, float maxX, float minZ, float maxZ)
{
    SweepContact contact;

    const float approach = cromwell::dot(direction, normal);
    if (approach > -1.0e-6f) {
        /* Travelling away from the face, or exactly along it. A ramp has no back
         * side worth reporting — the cell below it is solid or empty on its own
         * terms — so this is a miss rather than a hit from underneath. */
        return contact;
    }

    /* THE SHAPE'S SUPPORT along the plane normal — how far its centre must stay
     * from the plane. A box's is the dot of the normal's magnitude with its
     * half-extents; a sphere's is its radius from every direction; a capsule's
     * is its radius PLUS however much of its vertical segment the normal picks
     * up, which is the same identity the capsule sweep is built on. Getting the
     * capsule case wrong by using its half-extents would make it stand a full
     * radius too high on every ramp. */
    const Vec3 h = shape.halfExtents();
    float support = 0.0f;
    switch (shape.kind()) {
        case TraceShape::Kind::Sphere:
            support = shape.radius();
            break;
        case TraceShape::Kind::Capsule:
            support = shape.radius() + std::abs(normal.y) * shape.segmentHalf();
            break;
        default:
            support = std::abs(normal.x) * h.x + std::abs(normal.y) * h.y
                      + std::abs(normal.z) * h.z;
            break;
    }

    const float above = cromwell::dot(start - pointOn, normal);
    if (above < support) {
        /* Already at or through the surface. Reported as penetration for the
         * same reason a box sweep does — see TraceHit.hpp. */
        contact.hit = true;
        contact.startPenetrating = true;
        contact.normal = normal;
        contact.end = start;
        contact.point = start - normal * above;
        return contact;
    }

    const float distance = (above - support) / -approach;
    if (distance < 0.0f || distance > maxDistance) return contact;

    const Vec3 end = start + direction * distance;
    const Vec3 point = end - normal * support;

    /* The plane is infinite; the ramp is one tile of it. */
    if (point.x < minX || point.x > maxX || point.z < minZ || point.z > maxZ) return contact;

    contact.hit = true;
    contact.distance = distance;
    contact.normal = normal;
    contact.end = end;
    contact.point = point;
    return contact;
}

}  // namespace

LayerMask allSurfaces()
{
    return LayerMask::of(layer::kFloor) | LayerMask::of(layer::kRamp)
         | LayerMask::of(layer::kMass) | LayerMask::of(layer::kWall)
         | LayerMask::of(layer::kWindow) | LayerMask::of(layer::kCanopy)
         | LayerMask::of(layer::kUnit);
}

LayerMatrix defaultLayerMatrix()
{
    LayerMatrix matrix;

    matrix.nameLayer(layer::kFloor, "floor");
    matrix.nameLayer(layer::kRamp, "ramp");
    matrix.nameLayer(layer::kMass, "mass");
    matrix.nameLayer(layer::kWall, "wall");
    matrix.nameLayer(layer::kWindow, "window");
    matrix.nameLayer(layer::kCanopy, "canopy");
    matrix.nameLayer(layer::kUnit, "unit");

    matrix.nameLayer(layer::kCursor, "cursor");
    matrix.nameLayer(layer::kSight, "sight");
    matrix.nameLayer(layer::kShot, "shot");
    matrix.nameLayer(layer::kBody, "body");
    matrix.nameLayer(layer::kPaint, "paint");

    /* THE CURSOR STOPS AT EVERYTHING IT CAN SEE. A canopy included: pointing at
     * a roof should report the roof, and the storey cut is what removes the ones
     * above the floor being looked at. */
    matrix.setResponseToAll(layer::kCursor, Response::Ignore);
    for (const LayerId surface : { layer::kFloor, layer::kRamp, layer::kMass,
                                   layer::kWall, layer::kWindow, layer::kCanopy,
                                   layer::kUnit }) {
        matrix.setResponse(layer::kCursor, surface, Response::Block);
    }

    /* SIGHT PASSES GLASS AND ROOFS. A window is full cover you can see through —
     * that is the whole point of the flag on the edge — and a canopy is above
     * the line between two soldiers on the same floor, so stopping at one would
     * blind everybody indoors. */
    matrix.setResponseToAll(layer::kSight, Response::Ignore);
    matrix.setResponse(layer::kSight, layer::kMass, Response::Block);
    matrix.setResponse(layer::kSight, layer::kWall, Response::Block);
    matrix.setResponse(layer::kSight, layer::kFloor, Response::Block);
    matrix.setResponse(layer::kSight, layer::kRamp, Response::Block);
    matrix.setResponse(layer::kSight, layer::kWindow, Response::Overlap);

    /* A SHOT STOPS AT SOLIDS AND AT BODIES, and REPORTS the glass it goes
     * through rather than ignoring it — a broken window is a thing that should
     * happen, and it only can if the trace says the round passed through one. */
    matrix.setResponseToAll(layer::kShot, Response::Ignore);
    matrix.setResponse(layer::kShot, layer::kMass, Response::Block);
    matrix.setResponse(layer::kShot, layer::kWall, Response::Block);
    matrix.setResponse(layer::kShot, layer::kFloor, Response::Block);
    matrix.setResponse(layer::kShot, layer::kRamp, Response::Block);
    matrix.setResponse(layer::kShot, layer::kUnit, Response::Block);
    matrix.setResponse(layer::kShot, layer::kWindow, Response::Overlap);

    /* A MOVING BODY IS STOPPED BY EVERY SOLID INCLUDING GLASS. You cannot walk
     * through a window. */
    matrix.setResponseToAll(layer::kBody, Response::Ignore);
    for (const LayerId solid : { layer::kFloor, layer::kRamp, layer::kMass,
                                 layer::kWall, layer::kWindow, layer::kCanopy,
                                 layer::kUnit }) {
        matrix.setResponse(layer::kBody, solid, Response::Block);
    }

    /* PAINT STICKS TO GEOMETRY, NOT TO GLASS OR TO PEOPLE. The shader refuses
     * decals on blended surfaces, so a placement on a window would simply
     * produce nothing — see the note SurfacePicker carried. */
    matrix.setResponseToAll(layer::kPaint, Response::Ignore);
    matrix.setResponse(layer::kPaint, layer::kFloor, Response::Block);
    matrix.setResponse(layer::kPaint, layer::kRamp, Response::Block);
    matrix.setResponse(layer::kPaint, layer::kMass, Response::Block);
    matrix.setResponse(layer::kPaint, layer::kWall, Response::Block);

    return matrix;
}

WorldTrace::Params WorldTrace::between(Vec3 from, Vec3 to)
{
    Params params;
    params.start = from;

    const Vec3 along = to - from;
    params.maxDistance = along.length();
    params.direction = params.maxDistance > 1.0e-6f ? along / params.maxDistance
                                                    : Vec3{ 0.0f, 1.0f, 0.0f };
    return params;
}

template <typename OnHit>
void WorldTrace::sweep(const Params& params, OnHit&& onHit) const
{
    const Lattice& lattice = world_.lattice();
    const Terrain terrain(world_);
    const BlockedMass mass(world_);

    const Vec3 direction = params.direction.normalised();
    if (direction.lengthSquared() < 0.5f || params.maxDistance <= 0.0f) return;

    /* The traversal runs in lattice space; every contact is solved back in world
     * space. The scale is anisotropic, so distances differ between the two by a
     * single factor — computed once here rather than per cell. */
    const Vec3 latticeDirection = toLattice(direction);
    const float latticeScale = latticeDirection.length();
    if (latticeScale < 1.0e-6f) return;

    const Vec3 latticeStart = toLattice(params.start);
    const Vec3 latticeUnit = latticeDirection / latticeScale;
    const float latticeMax = params.maxDistance * latticeScale;

    /* A box in lattice space that BOUNDS the query shape — see the header on why
     * a bounding walk plus an exact test is the right split. */
    const Vec3 h = params.shape.halfExtents();
    const Vec3 latticeHalf{ h.x, h.z, h.y / kCellHeight };

    /* What has been found so far, so the walk can stop once the remaining cells
     * cannot beat it. Not a "stop at the first hit" — see the ordering guarantee
     * in GridTrace.hpp. */
    float nearestBlock = params.maxDistance;

    const TraceFilter& filter = params.filter;

    const auto consider = [&](const SweepContact& contact, LayerId layer, int x, int y,
                              int z, int id) {
        if (!contact.hit) return;
        if (contact.distance > nearestBlock) return;

        const Response response = filter.responseTo(layer);
        if (response == Response::Ignore) return;

        TraceHit hit;
        hit.response = response;
        hit.distance = contact.distance;
        hit.fraction = params.maxDistance > 0.0f ? contact.distance / params.maxDistance : 0.0f;
        hit.point = contact.point;
        hit.normal = contact.normal;
        hit.end = contact.end;
        hit.startPenetrating = contact.startPenetrating;
        hit.layer = layer;
        hit.id = id;
        hit.cellX = x;
        hit.cellY = y;
        hit.cellZ = z;

        onHit(hit);
        if (response == Response::Block) nearestBlock = std::min(nearestBlock, contact.distance);
    };

    const auto sweepBoxAt = [&](const Aabb& box) {
        return cromwell::sweepShape(params.shape, params.start, direction,
                                    params.maxDistance, box);
    };

    cromwell::traceGrid(latticeStart, latticeUnit, latticeMax, latticeHalf,
                        [&](const cromwell::GridCell& cell) {
        /* Back into world metres. The slab distance is the lower bound the
         * ordering guarantee is about: once it is past the nearest block, no
         * cell from here on can beat it. */
        const float slabWorld = cell.slabDistance / latticeScale;
        if (slabWorld > nearestBlock) return true;

        const int x = cell.x;
        const int y = cell.y;
        const int z = cell.z;
        if (!lattice.isValid(x, y, z)) return false;
        if (Lattice::storeyOfZ(z) > params.maxStorey) return false;

        const float base = Lattice::cellBaseHeight(z);
        const float top = base + kCellHeight;
        const Tile& tile = world_.at(lattice.index(x, y, z));

        /* ---- a fully blocked cell ---------------------------------------- */
        if (tile.blocked) {
            const std::optional<float> massTop = mass.topHeight(x, y, z);
            const float high = massTop ? std::min(*massTop, top) : top;
            if (high > base) {
                consider(sweepBoxAt(cellBounds(x, y, z, base, high)), layer::kMass, x, y, z, -1);
            }
            /* A blocked cell has no floor, no ramp and no canopy of its own. */
        } else if (tile.isRamp()) {
            Vec3 normal;
            Vec3 pointOn;
            rampPlane(tile, x, y, normal, pointOn);
            const SweepContact contact =
                sweepOntoPlane(params.shape, params.start, direction, params.maxDistance,
                               normal, pointOn, static_cast<float>(x),
                               static_cast<float>(x) + 1.0f, static_cast<float>(y),
                               static_cast<float>(y) + 1.0f);

            /* A RAMP OVERSPILLS ITS OWN CELL — a 45-degree flight rises a full
             * unit inside a cell two thirds of one tall (see Constants.hpp). So
             * the contact is accepted only from the cell its HEIGHT falls in,
             * which reports it exactly once no matter which cells the walk
             * passed through. */
            if (contact.hit && lattice.cellOfHeight(contact.point.y) == z) {
                consider(contact, layer::kRamp, x, y, z, -1);
            }
        } else if (tile.hasFloor) {
            const float surface = terrain.surfaceHeightAt(x, y, z, static_cast<float>(x) + 0.5f,
                                                          static_cast<float>(y) + 0.5f);
            consider(sweepBoxAt(cellBounds(x, y, z, surface - kFloorThickness, surface)),
                     layer::kFloor, x, y, z, -1);
        }

        if (tile.canopy) {
            consider(sweepBoxAt(cellBounds(x, y, z, top - kCanopyThickness, top)),
                     layer::kCanopy, x, y, z, -1);
        }

        /* ---- the faces ---------------------------------------------------
         * A CELL TESTS ONLY GEOMETRY THAT IS INSIDE IT, and getting that wrong
         * is the one way a walk like this can miss something it went through.
         *
         * A wall slab STRADDLES the boundary it sits on — kWallHalfThickness
         * either side — so half of every wall lies in each of the two cells it
         * separates. The first version of this gave each face a single owner,
         * the cell at its MINIMUM boundary, and tested the whole 9 cm slab from
         * there. That reports a shared wall exactly once, which was the problem
         * it was solving, but it breaks the walk's only guarantee: the
         * traversal promises to visit the cells the ray CROSSES, and a ray can
         * cross the half of a slab that sits in the non-owning cell and then
         * leave — sideways or into the next storey — without ever reaching the
         * owner. The wall is then never tested and the ray sails through it.
         *
         * The window is only 4.5 cm wide, so it reads as a cursor that
         * intermittently slides off a plain wall onto the ground behind it at
         * particular camera angles. See testWallIsFoundByARayThatLeavesTheCell-
         * InsideTheSlab, which is that ray.
         *
         * SO EACH CELL TESTS ITS OWN HALF OF ALL FOUR of its faces. The halves
         * tile the slab exactly, every one of them is inside the cell that
         * tests it, and the walk's guarantee is enough again.
         *
         * ONE FACE, ONE REPORT, STILL. The two halves meet at the boundary
         * plane, and that seam is not a surface — it is the middle of a wall.
         * A contact there is the ray arriving from the other half, which the
         * other cell has already reported, so it is dropped: `inward` says
         * which way the cell's interior lies, and a contact normal pointing the
         * other way is the seam. End caps, whose normals are perpendicular to
         * both, are kept — a wall that stops mid-run has a real face there.
         *
         * AND AT THE GRID'S EDGE THE OUTER HALF HAS NO CELL TO LIVE IN, so the
         * border keeps the full slab: there is no neighbour to report it twice,
         * and without this a wall on the boundary would lose its outward 4.5 cm
         * and be picked 4.5 cm inside where it is drawn. */
        const auto considerFace = [&](Dir face, bool alongX, float plane, float inward) {
            const Edge edge = world_.effectiveEdge(x, y, z, face);
            if (edge.cover == Cover::None) return;

            const LayerId faceLayer = edge.window ? layer::kWindow : layer::kWall;
            if (filter.ignores(faceLayer)) return;

            const bool shared = lattice.isValid(x + dx(face), y + dy(face), z);
            const float outer = shared ? 0.0f : kWallHalfThickness;

            const float nearSide = plane - inward * outer;
            const float farSide  = plane + inward * kWallHalfThickness;
            const float lo = std::min(nearSide, farSide);
            const float hi = std::max(nearSide, farSide);

            const Aabb slab =
                alongX ? Aabb{ Vec3{ lo, base, static_cast<float>(y) },
                               Vec3{ hi, top, static_cast<float>(y) + 1.0f } }
                       : Aabb{ Vec3{ static_cast<float>(x), base, lo },
                               Vec3{ static_cast<float>(x) + 1.0f, top, hi } };

            const SweepContact contact = sweepBoxAt(slab);
            if (!contact.hit) return;

            const float alongNormal = alongX ? contact.normal.x : contact.normal.z;
            if (shared && alongNormal * inward < -0.5f) return;

            consider(contact, faceLayer, x, y, z, -1);
        };

        considerFace(Dir::West,  true,  static_cast<float>(x),          1.0f);
        considerFace(Dir::East,  true,  static_cast<float>(x) + 1.0f,  -1.0f);
        considerFace(Dir::South, false, static_cast<float>(y),          1.0f);
        considerFace(Dir::North, false, static_cast<float>(y) + 1.0f,  -1.0f);

        return false;
    });

    /* ---- bodies -------------------------------------------------------
     * A LINEAR WALK, and CLAUDE.md says why: this is the entity layer, the
     * roster is dozens of entries, and each is rejected by a swept box test that
     * costs less than a spatial index lookup would. See the header for what to
     * do when that stops being true. */
    if (roster_ == nullptr || filter.ignores(layer::kUnit)) return;

    for (int index = 0; index < roster_->size(); ++index) {
        const Unit& unit = roster_->at(index);
        if (unit.isDead()) continue;
        if (index == params.ignoreId) continue;

        const Cell cell = unit.position();
        if (Lattice::storeyOfZ(cell.z) > params.maxStorey) continue;

        const float unitBase = unit.baseHeight(world_);
        const Aabb box{
            Vec3{ static_cast<float>(cell.x) + unit.body().pickMinHeight(), unitBase,
                  static_cast<float>(cell.y) + unit.body().pickMinHeight() },
            Vec3{ static_cast<float>(cell.x) + unit.body().pickMaxHeight(), unitBase + 0.95f,
                  static_cast<float>(cell.y) + unit.body().pickMaxHeight() }
        };

        consider(sweepBoxAt(box), layer::kUnit, cell.x, cell.y, cell.z, index);
    }
}

std::optional<TraceHit> WorldTrace::single(const Params& params) const
{
    std::optional<TraceHit> best;
    sweep(params, [&](const TraceHit& hit) {
        if (hit.response != Response::Block) return;
        if (!best || hit.distance < best->distance) best = hit;
    });
    return best;
}

void WorldTrace::multi(const Params& params, TraceHits& out) const
{
    out.clear();

    /* Collected as they come — the buffer sorts on insertion — and then cut at
     * the first block. The cut cannot be made during the walk: a swept shape
     * enters several cells at once, so an overlap can arrive before a nearer
     * block from a neighbouring cell in the same slab. See
     * TraceHits::dropBeyondFirstBlock. */
    sweep(params, [&](const TraceHit& hit) { out.add(hit); });
    out.dropBeyondFirstBlock();
}

bool WorldTrace::clearBetween(Vec3 from, Vec3 to, const TraceFilter& filter,
                              int maxStorey) const
{
    Params params = between(from, to);
    params.filter = filter;
    params.maxStorey = maxStorey;
    return !single(params).has_value();
}

}  // namespace game
