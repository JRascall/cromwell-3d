#include "cromwell/render/RenderScene.hpp"

#include "cromwell/collision/Frustum.hpp"
#include "cromwell/diag/Logger.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/material/IMaterialQuery.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {
namespace {

/* THE WORLD BOX OF A TRANSFORMED LOCAL BOX, by transforming all eight corners
 * and taking their extent.
 *
 * WHY NOT THE THREE-LINE TRICK. There is a well-known shortcut — take the
 * absolute value of the rotation part and multiply the half-extents — which is
 * fewer operations and is correct only for an affine transform with no
 * projection row. This engine's transforms are affine today and the shortcut
 * would be right today; the reason it is not used is that the failure when one
 * is not is silent and shaped like a culling bug, and this runs when a
 * renderable MOVES rather than per pixel or per cell. Eight matrix-vector
 * products on a cold path is not a cost worth a caveat.
 *
 * THE RESULT IS LOOSE, and that is inherent rather than a shortcoming: the
 * axis-aligned box around a rotated box is bigger than the rotated box. A
 * renderable at forty-five degrees is therefore culled slightly less often than
 * it could be, which is the conservative direction — see Frustum.hpp on why a
 * culler that errs must err this way. */
Aabb transformedBounds(const Aabb& local, const Mat4& transform)
{
    const Vec3 lo = local.min;
    const Vec3 hi = local.max;

    Vec3 minimum{ 0.0f, 0.0f, 0.0f };
    Vec3 maximum{ 0.0f, 0.0f, 0.0f };

    for (int corner = 0; corner < 8; corner++) {
        const Vec3 point{
            (corner & 1) ? hi.x : lo.x,
            (corner & 2) ? hi.y : lo.y,
            (corner & 4) ? hi.z : lo.z,
        };
        const Vec3 world = transform.transformPoint(point);

        if (corner == 0) {
            minimum = world;
            maximum = world;
            continue;
        }
        minimum.x = std::min(minimum.x, world.x);
        minimum.y = std::min(minimum.y, world.y);
        minimum.z = std::min(minimum.z, world.z);
        maximum.x = std::max(maximum.x, world.x);
        maximum.y = std::max(maximum.y, world.y);
        maximum.z = std::max(maximum.z, world.z);
    }
    return Aabb{ minimum, maximum };
}

/* WHICH SIZE BUCKET A BOX FALLS IN, with ZERO meaning "the biggest".
 *
 * Source's render groups lead with `OPAQUE_STATIC_HUGE` and
 * `OPAQUE_ENTITY_HUGE` so the largest things draw first, occlude, and let the
 * depth test reject more of what follows. This is that idea with the tuning
 * taken out of it.
 *
 * IT IS THE BINARY EXPONENT OF THE LARGEST EXTENT, INVERTED — one octave per
 * bucket — AND THAT CHOICE MATTERS. Source classifies against absolute world
 * units, which means a threshold somebody picked, in a unit somebody's game
 * defined, that stops being right when a project's scale differs. An exponent
 * is scale-free: it says "this is roughly twice the size of that", which is the
 * only claim the ordering actually rests on. A game in metres, a game in tiles
 * and a game in centimetres all get the same relative order with nothing to
 * tune, and cromwell is meant to carry an RTS, an FPS and a third-person game.
 *
 * COARSE ON PURPOSE. A bucket per octave means the sort still groups by
 * MATERIAL within a size class, which is what cuts state changes. Ranking by
 * exact size instead would order every draw by a number nobody cares about and
 * scatter the material batches to do it. */
std::uint8_t sizeRankOf(const Aabb& worldBounds)
{
    const Vec3 span = worldBounds.size();
    const float extent = std::max(span.x, std::max(span.y, span.z));

    /* A degenerate or empty box sorts LAST rather than first. It is the "I do
     * not know how big this is" answer, and putting an unknown at the front of
     * an early-z ordering would let it occlude nothing while claiming the slot
     * a genuinely huge object wanted. */
    if (!(extent > 0.0f)) return 255;

    /* One octave per bucket, clamped to a range that covers everything from a
     * millimetre to a hundred kilometres in any sane unit. Outside it the clamp
     * merely stops distinguishing, which is the harmless end of the failure. */
    constexpr int kLowest = -16;
    constexpr int kHighest = 47;

    const int octave = std::clamp(std::ilogb(extent), kLowest, kHighest);

    /* INVERTED, so ascending sort order puts the biggest first. */
    return static_cast<std::uint8_t>(kHighest - octave);
}

/* THE OPAQUE ORDER, IN ONE INTEGER: size class, then material, then mesh.
 *
 * SIZE LEADS because that is the early-z win and it is worth more than the
 * state changes it costs — a huge wall drawn before the props in front of it
 * rejects their fragments outright, where the reverse order shades both.
 * MATERIAL NEXT because a material change is a uniform buffer bind, and MESH
 * LAST because a mesh change is a vertex binding, which is the cheaper of the
 * two on every backend this engine targets.
 *
 * The material id is masked to 24 bits and the mesh handle keeps its full 32.
 * Two ids that collide here draw adjacently in an arbitrary order, which costs
 * a redundant bind and nothing else — see DrawItem on why this key may collide
 * where a cache key may not. */
std::uint64_t opaqueSortKey(std::uint8_t sizeRank, MaterialId material, rhi::MeshHandle mesh)
{
    return (static_cast<std::uint64_t>(sizeRank) << 56)
         | (static_cast<std::uint64_t>(material.value & 0x00FFFFFFu) << 32)
         | static_cast<std::uint64_t>(mesh.id);
}

}  // namespace

RenderScene::RenderScene(rhi::IRenderDevice& device, const IMaterialQuery& materials)
    : materials_(materials), probes_(device)
{
}

RenderScene::~RenderScene() = default;

bool RenderScene::initialise()
{
    /* THE PROBE ARRAY, HERE RATHER THAN ON THE PIPELINE. It describes a world —
     * where the rooms are and what they reflect — so it is brought up with the
     * world. See the ownership note at the top of this header. */
    return probes_.create();
}

void RenderScene::refreshDerived(Record& record)
{
    record.worldBounds = transformedBounds(record.localBounds, record.transform);
    record.worldCentre = record.worldBounds.centre();
    record.sizeRank = sizeRankOf(record.worldBounds);
}

RenderableId RenderScene::add(const RenderableDesc& desc)
{
    /* ---- the things that could never draw ------------------------------
     *
     * REFUSED AT REGISTRATION AND SAID OUT LOUD, which is the opposite of what
     * Source does. Its default render group is `RENDER_GROUP_OTHER` —
     * "unclassified, won't get drawn" — so a renderable that never says what it
     * is simply does not appear, with nothing on screen and no error anywhere.
     * That is the worst available failure: the evidence is an absence.
     *
     * Each of these three is a mistake rather than a state. A renderable with
     * no mesh has nothing to draw; one with an empty box is culled by every
     * view including its own; one visible to no viewer cannot be collected by
     * any view that will ever exist. None of them is a thing a caller means. */
    const char* reason = nullptr;
    if (!desc.mesh().valid())            reason = "no mesh";
    else if (desc.localBounds().empty()) reason = "empty bounds";
    else if (desc.viewers() == 0)        reason = "visible to no viewer";

    if (reason != nullptr) {
        if (!reportedUndrawable_) {
            reportedUndrawable_ = true;
            LOGGER.warn("scene: a renderable was refused - {}. It could never be drawn, "
                        "so it is rejected rather than registered and skipped forever",
                        reason);
        }
        return {};
    }

    /* A MATERIAL IS NOT REQUIRED, and that is not the same kind of mistake. A
     * renderable with no material draws with the pipeline's default block,
     * which is a plausible surface rather than nothing — so it is visible,
     * findable and worth reporting rather than refusing. */
    if (!desc.material().valid() && !reportedUndrawable_) {
        reportedUndrawable_ = true;
        LOGGER.warn("scene: a renderable was registered with no material - it will draw "
                    "with the pipeline's default surface");
    }

    std::uint32_t slot = 0;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(records_.size());
        records_.emplace_back();
    }

    Record& record = records_[slot];

    /* THE GENERATION IS THE SLOT'S, NOT THE RENDERABLE'S, so it survives the
     * assignment below. Everything else is overwritten. */
    const std::uint32_t generation = record.generation;

    record = Record{};
    record.generation = generation;
    record.transform = desc.transform();
    record.localBounds = desc.localBounds();
    record.tint = desc.tint();
    record.mesh = desc.mesh();
    record.material = desc.material();
    record.filterFlags = desc.filterFlags();
    record.viewers = desc.viewers();
    record.castsShadow = desc.castsShadow();
    record.customStencil = desc.customStencil();
    record.visibleInReflections = desc.visibleInReflections();
    record.visible = desc.visible();
    record.translucent = materials_.isTranslucent(desc.material());
    record.alive = true;

    refreshDerived(record);

    liveCount_++;
    boundsDirty_ = true;

    return RenderableId{ slot, generation };
}

RenderScene::Record* RenderScene::find(RenderableId id)
{
    return const_cast<Record*>(static_cast<const RenderScene*>(this)->find(id));
}

const RenderScene::Record* RenderScene::find(RenderableId id) const
{
    if (!id.valid()) return nullptr;
    if (id.index >= records_.size()) return nullptr;

    const Record& record = records_[id.index];

    /* THE GENERATION IS THE WHOLE GUARD. Without it a stale id addresses
     * whatever took the slot, and the symptom is one object moving when another
     * was told to — which reads as a gameplay bug in whichever system happened
     * to be holding the old id. */
    if (!record.alive || record.generation != id.generation) return nullptr;
    return &record;
}

void RenderScene::remove(RenderableId id)
{
    Record* record = find(id);
    if (record == nullptr) return;

    record->alive = false;
    record->mesh = {};

    /* BUMPED ON RELEASE, so every id already issued for this slot stops
     * matching immediately — not when the slot is next reused. A generation
     * bumped on REUSE instead would leave a window in which a stale id is
     * still accepted, which is exactly the window a deferred cleanup runs in. */
    record->generation++;

    freeSlots_.push_back(id.index);
    liveCount_--;
    boundsDirty_ = true;
}

void RenderScene::clear()
{
    for (Record& record : records_) {
        if (!record.alive) continue;
        record.alive = false;
        record.mesh = {};
        record.generation++;
    }

    freeSlots_.clear();
    freeSlots_.reserve(records_.size());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(records_.size()); i++)
        freeSlots_.push_back(i);

    liveCount_ = 0;
    boundsDirty_ = true;
}

void RenderScene::setTransform(RenderableId id, const Mat4& transform)
{
    Record* record = find(id);
    if (record == nullptr) return;

    record->transform = transform;
    refreshDerived(*record);
    boundsDirty_ = true;
}

void RenderScene::setVisible(RenderableId id, bool visible)
{
    Record* record = find(id);
    if (record != nullptr) record->visible = visible;
}

void RenderScene::setFilterFlags(RenderableId id, FilterFlags flags)
{
    Record* record = find(id);
    if (record != nullptr) record->filterFlags = flags;
}

void RenderScene::setCustomStencil(RenderableId id, std::uint8_t value)
{
    Record* record = find(id);
    if (record != nullptr) record->customStencil = value;
}

void RenderScene::setViewers(RenderableId id, ViewerMask viewers)
{
    Record* record = find(id);
    if (record != nullptr) record->viewers = viewers;
}

void RenderScene::setTint(RenderableId id, Vec4 tint)
{
    Record* record = find(id);
    if (record != nullptr) record->tint = tint;
}

void RenderScene::setMaterial(RenderableId id, MaterialId material)
{
    Record* record = find(id);
    if (record == nullptr) return;

    record->material = material;

    /* RE-ASKED HERE, which is the other half of caching it. A material change
     * that left the bucket alone would put a newly translucent surface in the
     * opaque pass, where it draws solid — and the material file would look
     * correct while the picture did not. */
    record->translucent = materials_.isTranslucent(material);
}

void RenderScene::setMesh(RenderableId id, rhi::MeshHandle mesh, const Aabb& localBounds)
{
    Record* record = find(id);
    if (record == nullptr) return;

    record->mesh = mesh;
    record->localBounds = localBounds;
    refreshDerived(*record);
    boundsDirty_ = true;
}

const Aabb& RenderScene::worldBounds() const
{
    if (boundsDirty_) rebuildWorldBounds();
    return worldBounds_;
}

void RenderScene::rebuildWorldBounds() const
{
    /* AN INVERTED BOX IS THE EMPTY ONE — `Aabb::empty()` is exactly this test,
     * so a scene with nothing in it reports empty rather than a point at the
     * origin. A point at the origin would frame the sun's projection around
     * nothing and produce a degenerate matrix; empty is what the shadow pass
     * checks for and skips on. */
    Aabb box{ Vec3{ 1.0f, 1.0f, 1.0f }, Vec3{ -1.0f, -1.0f, -1.0f } };
    bool any = false;

    for (const Record& record : records_) {
        if (!record.alive) continue;

        if (!any) {
            box = record.worldBounds;
            any = true;
            continue;
        }
        box.min.x = std::min(box.min.x, record.worldBounds.min.x);
        box.min.y = std::min(box.min.y, record.worldBounds.min.y);
        box.min.z = std::min(box.min.z, record.worldBounds.min.z);
        box.max.x = std::max(box.max.x, record.worldBounds.max.x);
        box.max.y = std::max(box.max.y, record.worldBounds.max.y);
        box.max.z = std::max(box.max.z, record.worldBounds.max.z);
    }

    worldBounds_ = box;
    boundsDirty_ = false;
}

bool RenderScene::relevantTo(const Record& record, const View& view)
{
    if (!record.visible) return false;

    /* HIDE IF ANY FLAG MATCHES — see Renderable.hpp on why this sense and not
     * the obvious one. It is the whole cutaway in one AND, and it composes
     * storeys with facings correctly where "show if any match" does not. */
    if ((record.filterFlags & view.hiddenFlags()) != 0) return false;

    /* SHOW IF ANY VIEWER MATCHES. The opposite sense, deliberately, because
     * ownership is a positive statement. */
    if ((record.viewers & view.viewerMask()) == 0) return false;

    /* THE ENGINE'S TWO QUESTIONS, ASKED OF THE RENDERABLE AND NOT OF THE PASS.
     * This is where "the game never learns what a shadow pass wants" actually
     * happens: the sun's view does not know what glass is, it knows that this
     * object says it does not cast. */
    switch (view.kind()) {
        case ViewKind::Sun:         return record.castsShadow;
        case ViewKind::ProbeFace:   return record.visibleInReflections;
        case ViewKind::CustomDepth: return record.customStencil != 0;
        case ViewKind::Camera:      break;
    }
    return true;
}

void RenderScene::collect(const View& view, SceneDrawList& out) const
{
    CW_PROFILE_ZONE_N("scene collect");

    out.clear();
    out.culled = 0;

    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection());
    const Vec3 eye = view.position();

    for (const Record& record : records_) {
        if (!record.alive) continue;
        if (!relevantTo(record, view)) continue;

        /* THE EXPENSIVE TEST LAST. Everything above is an integer compare; this
         * is six dot products over a box. CLAUDE.md's rule about culling
         * cheaply before testing expensively, applied to the culler itself. */
        if (!frustum.intersects(record.worldBounds)) {
            out.culled++;
            continue;
        }

        DrawItem item;
        item.transform = record.transform;
        item.tint = record.tint;
        item.mesh = record.mesh;
        item.material = record.material;
        item.customStencil = record.customStencil;

        if (record.translucent) {
            const Vec3 offset = record.worldCentre - eye;

            /* SQUARED, because the sort only compares distances and a square
             * root is monotonic — it would change every number and no ordering.
             *
             * THE CENTRE, NOT THE NEAREST POINT, and it is worth knowing this
             * is an approximation rather than a correct sort. Two translucent
             * surfaces that INTERSECT cannot be ordered by any single number,
             * and two long thin ones can straddle each other's centres. The
             * honest fix for those is per-triangle sorting or an
             * order-independent scheme, both of which cost far more than this
             * project's two overlapping window panes are worth. What this does
             * fix is the bug §4.12 calls the limitation most likely to force
             * the issue: today the transparent pass draws in BUCKET order,
             * which is not even an attempt. */
            item.viewDistanceSquared = dot(offset, offset);
            out.translucent().push_back(item);
        } else {
            item.sortKey = opaqueSortKey(record.sizeRank, record.material, record.mesh);
            out.opaque().push_back(item);
        }
    }

    /* BIGGEST FIRST, THEN BY MATERIAL. See opaqueSortKey. */
    std::sort(out.opaque().begin(), out.opaque().end(),
              [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });

    /* BACK TO FRONT: furthest first, because a blended surface reads what is
     * already in the colour buffer and everything behind it has to be there
     * before it draws. */
    std::sort(out.translucent().begin(), out.translucent().end(),
              [](const DrawItem& a, const DrawItem& b) {
                  return a.viewDistanceSquared > b.viewDistanceSquared;
              });
}

}  // namespace cromwell
