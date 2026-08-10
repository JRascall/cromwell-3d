#include "game/light/SunBaker.hpp"

#include "game/los/RayCaster.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace game {

namespace {

/* Fallback length, only used if the exit solve degenerates. */
constexpr float kMaxRayLength = 200.0f;

/* Lift the ray's origin off its own surface. Without it every texel's first
 * ray hits the floor it is standing on and the whole map bakes black — the
 * bake's equivalent of shadow acne, except here it is total rather than
 * speckled. */
constexpr float kSurfaceEpsilon = 0.01f;

struct Vec3 { float x, y, h; };

Vec3 normalise(Vec3 v)
{
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.h * v.h);
    if (length < 1e-6f) return Vec3{ 0.0f, 0.0f, 1.0f };
    return Vec3{ v.x / length, v.y / length, v.h / length };
}

/* HOW FAR THE RAY HAS TO GO — which is only as far as the grid, and getting
 * this wrong is expensive rather than incorrect. A ray that hits something
 * stops early; one that escapes to open sky walks its whole length. With a
 * fixed 200-unit ray that meant ~300 DDA steps per unobstructed sample, and
 * unobstructed samples are most of an outdoor bake. Solving the slab
 * intersection against the lattice bounds instead cuts that to the handful of
 * steps actually inside the map. */
float distanceToGridExit(const Lattice& lattice, Vec3 origin, Vec3 dir)
{
    const float maxX = static_cast<float>(lattice.width());
    const float maxY = static_cast<float>(lattice.height());
    const float maxH = Lattice::cellBaseHeight(lattice.depth());

    float exit = kMaxRayLength;

    const auto slab = [&](float o, float d, float low, float high) {
        if (std::fabs(d) < 1e-6f) return;
        const float toLow  = (low  - o) / d;
        const float toHigh = (high - o) / d;
        const float far = toLow > toHigh ? toLow : toHigh;
        if (far > 0.0f && far < exit) exit = far;
    };

    slab(origin.x, dir.x, 0.0f, maxX);
    slab(origin.y, dir.y, 0.0f, maxY);
    slab(origin.h, dir.h, 0.0f, maxH);

    /* A whisker past the boundary, so the last cell is genuinely visited. */
    return exit + 0.05f;
}

/* Any unit vector not parallel to `axis`, for building a basis. */
Vec3 perpendicular(Vec3 axis)
{
    const Vec3 seed = (std::fabs(axis.h) < 0.9f) ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                 : Vec3{ 1.0f, 0.0f, 0.0f };
    return normalise(Vec3{ axis.y * seed.h - axis.h * seed.y,
                           axis.h * seed.x - axis.x * seed.h,
                           axis.x * seed.y - axis.y * seed.x });
}

}  // namespace

SunBaker::SunBaker(const World& world, int texelsPerTile, int raysPerTexel)
    : world_(world),
      texelsPerTile_(texelsPerTile < 1 ? 1 : texelsPerTile),
      raysPerTexel_(raysPerTexel < 1 ? 1 : raysPerTexel),
      texelsPerPatch_(texelsPerTile_ * texelsPerTile_)
{
}

/* One patch per visible cell face. No atlas packing, no unwrap: on an
 * axis-aligned lattice every surface is a unit quad owned by one cell and one
 * direction, so the patch index IS the parameterisation. */
void SunBaker::collectPatches()
{
    patches_.clear();

    const Lattice& lattice = world_.lattice();
    const float step = 1.0f / static_cast<float>(texelsPerTile_);
    const float heightStep = kCellHeight / static_cast<float>(texelsPerTile_);

    for (int z = 0; z < lattice.depth(); z++) {
        const float cellBase = Lattice::cellBaseHeight(z);

        for (int y = 0; y < lattice.height(); y++)
        for (int x = 0; x < lattice.width(); x++) {
            const Tile& tile = world_.at(lattice.index(x, y, z));

            /* the up-facing floor slab */
            if (tile.hasFloor && !tile.blocked) {
                Patch patch;
                patch.cell = Cell{ x, y, z };
                patch.face = 0;
                patch.originX = static_cast<float>(x);
                patch.originY = static_cast<float>(y);
                /* The height the slab is DRAWN at, artDrop included — bake and
                 * render have to agree about where the surface is, or the
                 * occlusion is measured somewhere the player never sees. */
                patch.originH = cellBase + tile.floorOffset - tile.artDrop;
                patch.uX = step;  patch.uY = 0.0f;  patch.uH = 0.0f;
                patch.vX = 0.0f;  patch.vY = step;  patch.vH = 0.0f;
                patch.normalX = 0.0f; patch.normalY = 0.0f; patch.normalH = 1.0f;
                patches_.push_back(patch);
            }

            /* Wall faces, BOTH SIDES. The geometry emitter builds a wall box
             * once, from the canonical side, but the two faces of that box
             * belong to different rooms and are lit completely differently —
             * one can be in full sun while the other is the inside of a
             * building. So a patch is allocated per (cell, direction) using the
             * EFFECTIVE edge, which is mirrored into both neighbours. */
            for (Dir d : kAllDirs) {
                if (world_.effectiveEdge(x, y, z, d).cover == Cover::None) continue;

                Patch patch;
                patch.cell = Cell{ x, y, z };
                patch.face = 1 + toIndex(d);
                patch.originH = cellBase;
                patch.vX = 0.0f; patch.vY = 0.0f; patch.vH = heightStep;

                switch (d) {
                    case Dir::North:
                        patch.originX = static_cast<float>(x);
                        patch.originY = static_cast<float>(y) + 1.0f;
                        patch.uX = step; patch.uY = 0.0f; patch.uH = 0.0f;
                        patch.normalX = 0.0f; patch.normalY = -1.0f; patch.normalH = 0.0f;
                        break;
                    case Dir::South:
                        patch.originX = static_cast<float>(x);
                        patch.originY = static_cast<float>(y);
                        patch.uX = step; patch.uY = 0.0f; patch.uH = 0.0f;
                        patch.normalX = 0.0f; patch.normalY = 1.0f; patch.normalH = 0.0f;
                        break;
                    case Dir::East:
                        patch.originX = static_cast<float>(x) + 1.0f;
                        patch.originY = static_cast<float>(y);
                        patch.uX = 0.0f; patch.uY = step; patch.uH = 0.0f;
                        patch.normalX = -1.0f; patch.normalY = 0.0f; patch.normalH = 0.0f;
                        break;
                    default:   /* West */
                        patch.originX = static_cast<float>(x);
                        patch.originY = static_cast<float>(y);
                        patch.uX = 0.0f; patch.uY = step; patch.uH = 0.0f;
                        patch.normalX = 1.0f; patch.normalY = 0.0f; patch.normalH = 0.0f;
                        break;
                }
                patches_.push_back(patch);
            }
        }
    }

    visibility_.assign(patches_.size() * static_cast<std::size_t>(texelsPerPatch_), 1.0f);

    slotOf_.assign(static_cast<std::size_t>(lattice.cellCount()) * kFacesPerCell, -1);
    for (std::size_t p = 0; p < patches_.size(); p++) {
        const Patch& patch = patches_[p];
        slotOf_[static_cast<std::size_t>(lattice.index(patch.cell)) * kFacesPerCell
                + static_cast<std::size_t>(patch.face)] = static_cast<int>(p);
    }

    patchesValid_ = true;
}

/* Patches packed back to back, plus an index texture saying where each
 * (cell, face) landed. See SunLightmapLayout for why this beats the
 * world-aligned layout it replaced. */
void SunBaker::refreshAtlas()
{
    if (!patchesValid_) collectPatches();

    const Lattice& lattice = world_.lattice();

    int perRow = 1;
    while (perRow * perRow < static_cast<int>(patches_.size())) perRow++;
    if (perRow < 1) perRow = 1;
    const int rows = (static_cast<int>(patches_.size()) + perRow - 1) / perRow;

    layout_.texelsPerTile = texelsPerTile_;
    layout_.gridWidth   = lattice.width();
    layout_.gridHeight  = lattice.height();
    layout_.gridDepth   = lattice.depth();
    layout_.patchesPerRow = perRow;
    layout_.width  = perRow * texelsPerTile_;
    layout_.height = (rows < 1 ? 1 : rows) * texelsPerTile_;
    layout_.indexWidth  = lattice.width() * kFacesPerCell;
    layout_.indexHeight = lattice.height() * lattice.depth();

    /* Fully lit by default: a surface with no patch must never sample black. */
    atlas_.assign(static_cast<std::size_t>(layout_.width) *
                  static_cast<std::size_t>(layout_.height), 255);

    for (std::size_t p = 0; p < patches_.size(); p++) {
        const int slotX = static_cast<int>(p % static_cast<std::size_t>(perRow)) * texelsPerTile_;
        const int slotY = static_cast<int>(p / static_cast<std::size_t>(perRow)) * texelsPerTile_;
        const std::size_t base = p * static_cast<std::size_t>(texelsPerPatch_);

        for (int j = 0; j < texelsPerTile_; j++)
        for (int i = 0; i < texelsPerTile_; i++) {
            const float value = visibility_[base
                + static_cast<std::size_t>(j) * static_cast<std::size_t>(texelsPerTile_)
                + static_cast<std::size_t>(i)];
            atlas_[static_cast<std::size_t>(slotY + j) * static_cast<std::size_t>(layout_.width)
                 + static_cast<std::size_t>(slotX + i)] =
                static_cast<unsigned char>(value * 255.0f + 0.5f);
        }
    }

    /* 0xFFFF everywhere means "no patch", which the shader reads as not baked
     * and resolves on the shadow map instead. */
    indexMap_.assign(static_cast<std::size_t>(layout_.indexWidth) *
                     static_cast<std::size_t>(layout_.indexHeight) * 4, 255);

    for (std::size_t p = 0; p < patches_.size(); p++) {
        const Patch& patch = patches_[p];
        const int ix = patch.cell.x * kFacesPerCell + patch.face;
        const int iy = patch.cell.z * lattice.height() + patch.cell.y;
        const std::size_t at =
            (static_cast<std::size_t>(iy) * static_cast<std::size_t>(layout_.indexWidth)
             + static_cast<std::size_t>(ix)) * 4;

        indexMap_[at + 0] = static_cast<unsigned char>(p & 0xFF);
        indexMap_[at + 1] = static_cast<unsigned char>((p >> 8) & 0xFF);
        indexMap_[at + 2] = 0;
        indexMap_[at + 3] = 255;
    }
}

int SunBaker::slotOf(const Cell& cell, int face) const
{
    if (slotOf_.empty()) return -1;
    const std::size_t key =
        static_cast<std::size_t>(world_.lattice().index(cell)) * kFacesPerCell
        + static_cast<std::size_t>(face);
    return key < slotOf_.size() ? slotOf_[key] : -1;
}

/* Rebuild the patch list, then move every surviving patch's texels to its new
 * slot. The key is (cell, face), which does not move when a neighbouring wall
 * is destroyed — so a re-bake only has to touch what actually changed. */
void SunBaker::refreshGeometry()
{
    if (!patchesValid_) { collectPatches(); return; }

    const Lattice& lattice = world_.lattice();

    const std::vector<int>   oldSlotOf     = slotOf_;
    const std::vector<float> oldVisibility = visibility_;
    collectPatches();   /* resets visibility_ to fully lit */

    for (std::size_t p = 0; p < patches_.size(); p++) {
        const Patch& patch = patches_[p];
        const std::size_t key =
            static_cast<std::size_t>(lattice.index(patch.cell)) * kFacesPerCell
            + static_cast<std::size_t>(patch.face);

        const int old = oldSlotOf[key];
        if (old < 0) continue;   /* a surface that did not exist before */

        const std::size_t from = static_cast<std::size_t>(old) * static_cast<std::size_t>(texelsPerPatch_);
        const std::size_t to   = p * static_cast<std::size_t>(texelsPerPatch_);
        for (int i = 0; i < texelsPerPatch_; i++)
            visibility_[to + static_cast<std::size_t>(i)] =
                oldVisibility[from + static_cast<std::size_t>(i)];
    }
}

std::int64_t SunBaker::bakePatch(const RayCaster& caster, const Patch& patch,
                                 std::size_t firstTexel, const SunSample& sun)
{
    const Vec3 toSun = normalise(Vec3{ -sun.directionX, -sun.directionZ, -sun.directionY });
    const Vec3 normal{ patch.normalX, patch.normalY, patch.normalH };

    /* A surface turned away from the sun is shadowed by its own geometry, and
     * no ray is going to discover otherwise. This is most of the map's faces
     * and it is the single biggest saving in the whole bake. */
    const float facing = normal.x * toSun.x + normal.y * toSun.y + normal.h * toSun.h;
    if (facing <= 0.0f) {
        for (int i = 0; i < texelsPerPatch_; i++)
            visibility_[firstTexel + static_cast<std::size_t>(i)] = 0.0f;
        return 0;
    }

    /* A disc of directions around the sun centre — this is what gives a
     * penumbra that widens with distance from the caster, rather than the
     * uniform blur a shadow map's filter kernel produces. */
    const Vec3 tangent = perpendicular(toSun);
    const Vec3 bitangent{ toSun.y * tangent.h - toSun.h * tangent.y,
                          toSun.h * tangent.x - toSun.x * tangent.h,
                          toSun.x * tangent.y - toSun.y * tangent.x };
    const float discRadius = std::tan(sun.angularRadius);

    std::int64_t rays = 0;

    for (int j = 0; j < texelsPerTile_; j++)
    for (int i = 0; i < texelsPerTile_; i++) {
        const float u = static_cast<float>(i) + 0.5f;
        const float v = static_cast<float>(j) + 0.5f;

        const float px = patch.originX + patch.uX * u + patch.vX * v
                       + normal.x * kSurfaceEpsilon;
        const float py = patch.originY + patch.uY * u + patch.vY * v
                       + normal.y * kSurfaceEpsilon;
        const float ph = patch.originH + patch.uH * u + patch.vH * v
                       + normal.h * kSurfaceEpsilon;

        int reached = 0;
        for (int r = 0; r < raysPerTexel_; r++) {
            /* Golden-angle spiral: deterministic, evenly spread, and no noise
             * texture or RNG state to carry around. */
            const float t = (static_cast<float>(r) + 0.5f) / static_cast<float>(raysPerTexel_);
            const float radius = std::sqrt(t) * discRadius;
            const float phi = static_cast<float>(r) * 2.39996323f;
            const float ox = std::cos(phi) * radius;
            const float oy = std::sin(phi) * radius;

            const Vec3 dir = normalise(Vec3{ toSun.x + tangent.x * ox + bitangent.x * oy,
                                             toSun.y + tangent.y * ox + bitangent.y * oy,
                                             toSun.h + tangent.h * ox + bitangent.h * oy });

            const float length =
                distanceToGridExit(world_.lattice(), Vec3{ px, py, ph }, dir);

            if (caster.cast(px, py, ph,
                            px + dir.x * length,
                            py + dir.y * length,
                            ph + dir.h * length)) reached++;
            rays++;
        }

        const std::size_t texel = firstTexel
                                + static_cast<std::size_t>(j) * static_cast<std::size_t>(texelsPerTile_)
                                + static_cast<std::size_t>(i);
        visibility_[texel] = static_cast<float>(reached) / static_cast<float>(raysPerTexel_);
    }

    return rays;
}

/* Patches write to disjoint texel ranges, so this parallelises by simply
 * splitting the list — no locking, no atomics beyond the per-thread ray
 * tallies. Each worker builds its own RayCaster rather than sharing one,
 * which costs a couple of small query objects and removes any question about
 * their thread safety. */
std::int64_t SunBaker::bakeSlots(const std::vector<std::size_t>& slots, const SunSample& sun)
{
    const unsigned hardware = std::thread::hardware_concurrency();
    unsigned workers = hardware ? hardware : 1u;
    if (workers > slots.size()) workers = static_cast<unsigned>(slots.size());
    if (workers < 1) workers = 1;

    std::vector<std::int64_t> tally(workers, 0);
    std::vector<std::thread>  threads;
    threads.reserve(workers);

    const auto run = [&](unsigned index) {
        /* Sunlight rules, not sight rules: a sandbag wall is opaque even
         * though a soldier can shoot over it. */
        const RayCaster caster(world_, RayRules::Sunlight);
        const std::size_t first = slots.size() * index / workers;
        const std::size_t last  = slots.size() * (index + 1) / workers;

        std::int64_t rays = 0;
        for (std::size_t i = first; i < last; i++) {
            const std::size_t p = slots[i];
            rays += bakePatch(caster, patches_[p],
                              p * static_cast<std::size_t>(texelsPerPatch_), sun);
        }
        tally[index] = rays;
    };

    for (unsigned i = 1; i < workers; i++) threads.emplace_back(run, i);
    run(0);
    for (std::thread& thread : threads) thread.join();

    std::int64_t rays = 0;
    for (std::int64_t count : tally) rays += count;
    return rays;
}

SunBakeStats SunBaker::bakeAll(const SunSample& sun)
{
    if (!patchesValid_) collectPatches();

    std::vector<std::size_t> slots(patches_.size());
    for (std::size_t p = 0; p < patches_.size(); p++) slots[p] = p;

    const auto start = std::chrono::steady_clock::now();
    const std::int64_t rays = bakeSlots(slots, sun);
    const auto finish = std::chrono::steady_clock::now();

    SunBakeStats stats;
    stats.patches = static_cast<int>(patches_.size());
    stats.texels  = static_cast<int>(patches_.size()) * texelsPerPatch_;
    stats.rays    = rays;
    stats.milliseconds =
        std::chrono::duration<double, std::milli>(finish - start).count();
    return stats;
}

/* The changed cells, plus everything they were shading. With one sun the
 * second set is a line swept from each changed cell, not a hemisphere — which
 * is the entire reason an incremental re-bake is affordable. */
void SunBaker::collectShadowShaft(const SunSample& sun, const Cell& centre, float radiusTiles,
                                  std::vector<std::uint8_t>& affected) const
{
    const Lattice& lattice = world_.lattice();
    affected.assign(static_cast<std::size_t>(lattice.cellCount()), 0);

    const int reach = static_cast<int>(std::ceil(radiusTiles));
    const Vec3 travel = normalise(Vec3{ sun.directionX, sun.directionZ, sun.directionY });

    std::vector<Cell> seeds;
    for (int y = centre.y - reach; y <= centre.y + reach; y++)
    for (int x = centre.x - reach; x <= centre.x + reach; x++) {
        if (!lattice.inBounds(x, y)) continue;
        const float dx = static_cast<float>(x - centre.x);
        const float dy = static_cast<float>(y - centre.y);
        if (dx * dx + dy * dy > radiusTiles * radiusTiles) continue;

        /* A grenade edits the whole column it can reach, not one z slice. */
        for (int z = 0; z < lattice.depth(); z++) {
            if (Lattice::storeyOfZ(z) != Lattice::storeyOfZ(centre.z)) continue;
            affected[static_cast<std::size_t>(lattice.index(x, y, z))] = 1;
            seeds.push_back(Cell{ x, y, z });
        }
    }

    /* March each seed down-sun. Half-cell steps so a shaft cannot tunnel
     * between diagonally adjacent cells. */
    for (const Cell& seed : seeds) {
        float x = static_cast<float>(seed.x) + 0.5f;
        float y = static_cast<float>(seed.y) + 0.5f;
        float h = Lattice::cellBaseHeight(seed.z) + kCellHeight * 0.5f;

        for (int step = 0; step < 512; step++) {
            x += travel.x * 0.5f;
            y += travel.y * 0.5f;
            h += travel.h * 0.5f;

            const int cx = static_cast<int>(std::floor(x));
            const int cy = static_cast<int>(std::floor(y));
            const int cz = static_cast<int>(std::floor(h / kCellHeight));
            if (!lattice.isValid(cx, cy, cz)) {
                if (h < 0.0f || !lattice.inBounds(cx, cy)) break;
                continue;
            }
            affected[static_cast<std::size_t>(lattice.index(cx, cy, cz))] = 1;
        }
    }
}

SunBakeStats SunBaker::bakeRegion(const SunSample& sun, const Cell& centre, float radiusTiles)
{
    if (!patchesValid_) collectPatches();

    std::vector<std::uint8_t> affected;

    const auto start = std::chrono::steady_clock::now();
    collectShadowShaft(sun, centre, radiusTiles, affected);

    const Lattice& lattice = world_.lattice();
    std::vector<std::size_t> slots;
    for (std::size_t p = 0; p < patches_.size(); p++)
        if (affected[static_cast<std::size_t>(lattice.index(patches_[p].cell))])
            slots.push_back(p);

    const std::int64_t rays = slots.empty() ? 0 : bakeSlots(slots, sun);
    const auto finish = std::chrono::steady_clock::now();

    const int touched = static_cast<int>(slots.size());
    SunBakeStats stats;
    stats.patches = touched;
    stats.texels  = touched * texelsPerPatch_;
    stats.rays    = rays;
    stats.milliseconds =
        std::chrono::duration<double, std::milli>(finish - start).count();
    return stats;
}

}  // namespace game
