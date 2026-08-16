#include "game/render/rhi/RhiOverlays.hpp"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/geometry/BoxEmitter.hpp"
#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/render/RenderScene.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "game/lattice/Constants.hpp"
#include "game/los/VisibilityField.hpp"
#include "game/movement/search/PathPoint.hpp"
#include "game/path/MoveAnimator.hpp"
#include "game/query/cover/CoverModel.hpp"
#include "game/query/Standability.hpp"
#include "game/query/Terrain.hpp"
#include "game/render/FrameView.hpp"
#include "game/render/Palette.hpp"
#include "game/render/DrawLayers.hpp"
#include "game/render/scene/RenderFilter.hpp"
#include "game/state/GameState.hpp"
#include "game/units/kinds/Unit.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

#include <cmath>

namespace game {
namespace {

/* FNV-1a, 64-bit. A hash rather than a checksum because all that is asked of it
 * is "did these bytes change", and all that is required is that a change is
 * seen — a collision costs one skipped rebuild of an overlay, not a corrupted
 * anything. See the header on why this is a hash at all rather than a list of
 * the things that are believed to change the field. */
constexpr std::uint64_t kHashSeed = 1469598103934665603ull;

std::uint64_t hashByte(std::uint64_t hash, std::uint8_t byte)
{
    return (hash ^ byte) * 1099511628211ull;
}

std::uint64_t hashInt(std::uint64_t hash, std::int64_t value)
{
    for (int i = 0; i < 8; i++)
        hash = hashByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    return hash;
}

std::uint64_t hashFloat(std::uint64_t hash, float value)
{
    /* THROUGH AN INT, and quantised. Hashing the raw bits would make a hover
     * plate whose height differs in the last mantissa bit a rebuild — which is
     * every frame, on a value derived from a float divide. A thousandth of a
     * tile is far below anything visible and far above the noise. */
    return hashInt(hash, static_cast<std::int64_t>(std::lround(value * 1000.0f)));
}

/* A HASH OF ONE IS "EMPTY", so that an empty layer is distinguishable from a
 * layer that has never been built. Zero is reserved for the latter. */
constexpr std::uint64_t kEmptyKey = 1;

/* ============ A FLAT STRIP FROM A TO B, LYING ALONG THE SEGMENT ===========
 *
 * WHY THIS EXISTS AND emitBox DOES NOT SERVE: emitBox is axis-aligned, so a
 * box asked to span a DIAGONAL step comes out as the square containing it — a
 * full tile of white plate where a link belongs. That was the first version of
 * the path preview and it is what "it renders multiple squares" looks like.
 *
 * A ribbon needs an orientation, and there are two places one could come from:
 * a rotation on the renderable's transform, or the vertices themselves. THE
 * VERTICES, because the transform route needs a real normal matrix for the
 * lighting to survive a rotation, and `object.glsl` states plainly that it does
 * not carry one — its `mat3(model)` is exact only for the untransformed world
 * and axis-aligned boxes. Rotating through the push constant would have lit the
 * strip by a normal that is not its own, which is a subtler wrong than the
 * squares it replaced. These vertices are built in world space anyway, exactly
 * as the lattice's are, so the orientation costs nothing.
 *
 * TWO QUADS, WOUND OPPOSITE WAYS, and it is not double-drawing. The transparent
 * pipeline culls back faces (`RasterState::cull` defaults to Back), so exactly
 * one of the pair ever rasterises — and which one is whichever side the camera
 * happens to be on. A single quad would be correct for every camera this game
 * has and would vanish silently for the first one that ever looked up at a
 * walkway, which is a bug that costs an afternoon and six vertices to prevent.
 *
 * A DEGENERATE SEGMENT EMITS NOTHING. Two path points at the same place is a
 * legitimate thing for a path to contain — a turn in place, a rounding — and
 * normalising a zero vector produces NaN positions that take the whole
 * renderable's bounds with them, so the mesh vanishes rather than the segment. */
void emitRibbon(cromwell::MeshVertexBuffer& out, Vector3 from, Vector3 to,
                float width, Color colour)
{
    const cromwell::Vec3 a{ from.x, from.y, from.z };
    const cromwell::Vec3 b{ to.x, to.y, to.z };

    const cromwell::Vec3 along = b - a;
    const float length = along.length();
    if (length < 1.0e-5f) return;

    const cromwell::Vec3 direction = along * (1.0f / length);

    /* THE REFERENCE UP, swapped when the segment is itself vertical — a ladder
     * climb is one, and the cross product against a parallel vector is zero.
     * Any perpendicular will do there; the strip has no preferred face when it
     * is standing on end. */
    const cromwell::Vec3 up = std::fabs(direction.y) > 0.99f
                            ? cromwell::Vec3{ 1.0f, 0.0f, 0.0f }
                            : cromwell::Vec3{ 0.0f, 1.0f, 0.0f };

    const cromwell::Vec3 side = cross(up, direction).normalised() * (width * 0.5f);
    const cromwell::Vec3 normal = cross(direction, side).normalised();

    /* THE CORNER ORDER IS emitBox's TOP FACE, with its X axis replaced by
     * `side` and its Z axis by the segment. Copied rather than re-derived
     * because winding is the one thing here with no visible failure mode short
     * of the whole strip disappearing — a back-facing quad is culled, and a
     * culled quad and a quad that was never emitted look identical. */
    const cromwell::Vec3 corners[4] = {
        a - side,
        a + side,
        b + side,
        b - side,
    };

    const auto put = [&](const cromwell::Vec3& p, cromwell::Vec3 n, float u, float v) {
        cromwell::SurfaceVertex vertex;
        vertex.position = Vector3{ p.x, p.y, p.z };
        vertex.normal   = Vector3{ n.x, n.y, n.z };

        /* THE TANGENT IS THE ALONG DIRECTION, which is what a UV laid out down
         * the strip implies. Nothing samples a normal map on an overlay today;
         * it is filled correctly rather than left as garbage, because the day
         * something does the failure would be a lighting artefact on one
         * surface with no obvious cause. */
        vertex.tangent = Vector4{ direction.x, direction.y, direction.z, 1.0f };
        vertex.uv      = Vector2{ u, v };
        vertex.colour  = colour;
        out.vertex(vertex);
    };

    const auto face = [&](int i0, int i1, int i2, int i3, cromwell::Vec3 n) {
        put(corners[i0], n, 0.0f, 0.0f);
        put(corners[i1], n, 1.0f, 0.0f);
        put(corners[i2], n, 1.0f, length);
        put(corners[i0], n, 0.0f, 0.0f);
        put(corners[i2], n, 1.0f, length);
        put(corners[i3], n, 0.0f, length);
    };

    face(0, 1, 2, 3, normal);
    face(3, 2, 1, 0, normal * -1.0f);
}

}  // namespace

RhiOverlays::RhiOverlays(cromwell::rhi::IRenderDevice& device) : device_(device) {}

RhiOverlays::~RhiOverlays() { release(); }

void RhiOverlays::releaseLayer(Layer& layer)
{
    /* THE RENDERABLE FIRST, THEN THE MESH IT NAMES — the mesh lifetime rule,
     * the same order RhiStatics and RhiBodies use. See RenderScene.hpp. */
    if (scene_ != nullptr && layer.id.valid()) scene_->remove(layer.id);
    if (layer.mesh.valid())     device_.destroy(layer.mesh);
    if (layer.vertices.valid()) device_.destroy(layer.vertices);
    layer = Layer{};
}

void RhiOverlays::release()
{
    for (Layer& layer : visibility_) releaseLayer(layer);
    visibility_.clear();

    releaseLayer(cover_);
    releaseLayer(hover_);
    releaseLayer(path_);
    scene_ = nullptr;
}

int RhiOverlays::renderableCount() const
{
    int count = 0;
    for (const Layer& layer : visibility_) count += layer.id.valid() ? 1 : 0;
    count += cover_.id.valid() ? 1 : 0;
    count += hover_.id.valid() ? 1 : 0;
    count += path_.id.valid() ? 1 : 0;
    return count;
}

void RhiOverlays::update(cromwell::RenderScene& scene, Layer& layer, std::uint64_t key,
                         const cromwell::MeshVertexBuffer& built,
                         cromwell::ViewerMask viewer, cromwell::FilterFlags flags)
{
    /* THE FILTER AND THE VIEWER ARE WRITTEN EVERY FRAME, and the mesh only when
     * the hash moved. They are two different rates: a hover plate that has not
     * changed shape can still have moved to a different storey, and both are a
     * single field write on a record rather than an upload. */
    if (layer.id.valid()) {
        scene.setFilterFlags(layer.id, flags);
        scene.setViewers(layer.id, viewer);
    }

    if (key == layer.key) return;
    layer.key = key;

    if (built.empty()) {
        /* HIDDEN, NOT REMOVED. A layer that comes and goes with the cursor
         * would otherwise recycle a scene slot several times a second, and each
         * recycle invalidates the id held here for the sake of freeing one
         * record. Visibility is a bool. */
        if (layer.id.valid()) scene.setVisible(layer.id, false);
        return;
    }

    const std::vector<std::uint8_t> packed = built.interleave();
    const uint32_t vertices = static_cast<uint32_t>(built.vertexCount());

    /* THE OLD PAIR GOES BEFORE THE NEW ONE ARRIVES, but the RENDERABLE stays —
     * which is exactly what setMesh is for. Destroying the mesh while the
     * renderable still names it is safe only because nothing draws between here
     * and the setMesh below: this runs at the top of a frame, outside any pass. */
    if (layer.mesh.valid())     device_.destroy(layer.mesh);
    if (layer.vertices.valid()) device_.destroy(layer.vertices);
    layer.mesh = {};
    layer.vertices = {};

    cromwell::rhi::BufferDesc desc;
    desc.name  = "overlay";
    desc.bytes = packed.size();
    desc.usage = cromwell::rhi::BufferUsageVertex;

    /* REBUILT ON A CHANGE, NOT PER FRAME — so it is written once and read many
     * times, which is the same access pattern the static world has even though
     * the interval is seconds rather than the life of the map. */
    desc.access = cromwell::rhi::BufferAccess::CpuToGpuOnce;

    layer.vertices = device_.createBuffer(desc);
    if (!layer.vertices.valid()) return;

    device_.updateBuffer(layer.vertices, packed.data(), packed.size(), 0);

    layer.mesh = device_.createMesh(cromwell::MeshVertexBuffer::deviceLayout(),
                                    layer.vertices, vertices);
    if (!layer.mesh.valid()) {
        device_.destroy(layer.vertices);
        layer.vertices = {};
        return;
    }

    if (layer.id.valid()) {
        scene.setMesh(layer.id, layer.mesh, built.bounds());
        scene.setVisible(layer.id, true);
        return;
    }

    /* FIRST TIME: register it. Everything that makes an overlay an overlay is
     * on this description and nowhere else — see the header on the viewer mask
     * and on why it neither casts nor reflects. */
    layer.id = scene.add(
        cromwell::RenderableDesc()
            .withMesh(layer.mesh, built.bounds())
            .withMaterial(cromwell::DeviceMaterials::idOf(cromwell::SurfaceKind::Overlay))
            .withViewers(viewer)
            .withFilterFlags(flags)
            .withCastsShadow(false)
            .withVisibleInReflections(false));
}

void RhiOverlays::sync(cromwell::RenderScene& scene, const FrameView& view,
                       cromwell::ViewerMask viewer)
{
    /* THE ONE OVERLAY ZONE, and this is the system most likely to want a split
     * later: the visibility hash walks every standable cell every frame, and a
     * rebuild walks them again emitting boxes. Neither is split yet because
     * neither has been measured to matter - sub-zones are earned by cost, not
     * anticipated by structure. If this row ever grows, the line to split along
     * is hash-versus-rebuild, which is the question a measurement would be
     * asking. */
    CW_PROFILE_ZONE_N("overlay sync");

    if (view.state == nullptr) return;

    if (scene_ != &scene) {
        release();
        scene_ = &scene;
    }

    const GameState& state = *view.state;
    const World&     world = state.world();
    const Lattice&   lattice = world.lattice();

    const Terrain      terrain(world);
    const Standability standability(world);

    cromwell::MeshVertexBuffer buffer;

    /* ---- 1. the visibility field, one mesh per storey -------------------
     *
     * The plate heights come from the terrain and the colours from the grade,
     * exactly as OverlayRenderer::drawVisibility builds them — copied rather
     * than shared while both renderers exist, on the same terms as every other
     * pair in this migration. */
    visibility_.resize(static_cast<std::size_t>(lattice.storeys()));

    const bool showVisibility = state.losMode();

    for (int storey = 0; storey < lattice.storeys(); storey++) {
        buffer.clear();

        std::uint64_t key = kEmptyKey;

        if (showVisibility) {
            key = kHashSeed;
            key = hashInt(key, storey);

            const VisibilityField& visibility = state.visibility();

            for (int i = 0; i < kCellsPerStorey; i++) {
                const int z = Lattice::storeyBaseZ(storey) + i;

                for (int y = 0; y < lattice.height(); y++)
                for (int x = 0; x < lattice.width(); x++) {
                    if (!standability.isStandable(x, y, z)) continue;

                    const Visibility grade = visibility.at(lattice.index(x, y, z));

                    /* THE HASH IS OVER WHAT IS DRAWN, not over the whole field.
                     * A cell nobody can stand on contributes no geometry, so a
                     * change to its grade must not trigger a rebuild — and a
                     * cell that stops being standable changes the sequence
                     * below and therefore does. */
                    key = hashInt(key, static_cast<std::int64_t>(grade));
                    key = hashInt(key, lattice.index(x, y, z));

                    const Color colour =
                        (grade == Visibility::Direct)   ? palette::kVisibleDirect
                      : (grade == Visibility::PeekOnly) ? palette::kVisiblePeek
                                                        : palette::kVisibleNone;

                    cromwell::emitBox(buffer,
                                      static_cast<float>(x) + 0.5f,
                                      terrain.centerHeight(x, y, z) + 0.028f,
                                      static_cast<float>(y) + 0.5f,
                                      0.94f, 0.01f, 0.94f, colour);
                }
            }
        }

        /* ITS OWN STOREY'S BIT, so the iso level hides and shows it with the
         * floor it belongs to and no rebuild happens at all. */
        update(scene, visibility_[static_cast<std::size_t>(storey)], key, buffer,
               viewer, storeyFlag(storey) | layerFlag(drawLayer::kOverlays));
    }

    /* ---- 2. the cover shields -------------------------------------------
     *
     * The selected unit's, and the hovered cell's when it is a different one —
     * the same two calls FrameRenderer::drawOverlays makes, in one mesh because
     * they are one layer that changes together. */
    buffer.clear();
    std::uint64_t coverKey = kEmptyKey;
    int coverStorey = 0;

    if (view.settings != nullptr && view.settings->showCover) {
        const CoverModel cover(world, state.roster());
        const Unit&      selected = state.selectedUnit();

        coverKey = kHashSeed;

        const auto emitShields = [&](const Cell& cell) {
            for (Dir d : kAllDirs) {
                const Cover grade = cover.displayCover(cell, d);
                if (grade == Cover::None) continue;

                const bool  full = (grade == Cover::Full);
                const float height = terrain.centerHeight(cell)
                                   + (full ? 0.45f * kStoreyHeight : 0.34f);
                const float px = static_cast<float>(cell.x) + 0.5f
                               + static_cast<float>(dx(d)) * 0.36f;
                const float py = static_cast<float>(cell.y) + 0.5f
                               + static_cast<float>(dy(d)) * 0.36f;

                const Color colour = full ? palette::kCoverFull : palette::kCoverHalf;
                const bool  northSouth = (d == Dir::North || d == Dir::South);

                coverKey = hashInt(coverKey, static_cast<std::int64_t>(grade));
                coverKey = hashInt(coverKey, static_cast<std::int64_t>(d));
                coverKey = hashFloat(coverKey, px);
                coverKey = hashFloat(coverKey, py);
                coverKey = hashFloat(coverKey, height);

                cromwell::emitBox(buffer, px, height, py,
                                  northSouth ? 0.30f : 0.04f,
                                  full ? 0.40f : 0.20f,
                                  northSouth ? 0.04f : 0.30f, colour);
            }
        };

        if (selected.showsCoverShields()) {
            emitShields(selected.position());
            coverStorey = Lattice::storeyOfZ(selected.position().z);
        }

        if (view.hovered) {
            const Cell hoverCell = lattice.cellAt(*view.hovered);
            if (hoverCell != selected.position()) {
                emitShields(hoverCell);
                coverStorey = Lattice::storeyOfZ(hoverCell.z);
            }
        }
    }

    /* ONE STOREY BIT FOR THE WHOLE LAYER, which is honest for a layer that is
     * one or two adjacent cells and would not be for a layer that spanned the
     * map. If the shields ever cover the board, they split per storey the way
     * the visibility field does. */
    update(scene, cover_, coverKey, buffer, viewer,
           storeyFlag(coverStorey) | layerFlag(drawLayer::kOverlays));

    /* ---- 3. the hover plate --------------------------------------------- */
    buffer.clear();
    std::uint64_t hoverKey = kEmptyKey;
    int hoverStorey = 0;

    const bool animating = view.animator != nullptr && view.animator->isRunning();

    if (!animating && view.hovered) {
        const Cell hoverCell = lattice.cellAt(*view.hovered);

        const bool ok = view.grenadeArmed
                     || (view.hoverRestOk
                         && state.reach().cost(*view.hovered) <= state.sprintBudget());

        const bool wideHull = state.selectedUnit().footprint().isMultiTile()
                           && !view.grenadeArmed;

        const Color colour = view.grenadeArmed ? palette::kHoverGrenade
                           : ok ? palette::kHoverValid : palette::kHoverInvalid;

        const float size   = wideHull ? 1.96f : 0.96f;
        const float offset = size > 1.0f ? 1.0f : 0.5f;
        const float height = state.hoverPlateHeight(hoverCell) + 0.03f;

        hoverKey = hashInt(kHashSeed, *view.hovered);
        hoverKey = hashInt(hoverKey, colour.r * 65536 + colour.g * 256 + colour.b);
        hoverKey = hashInt(hoverKey, colour.a);
        hoverKey = hashFloat(hoverKey, size);
        hoverKey = hashFloat(hoverKey, height);

        cromwell::emitBox(buffer,
                          static_cast<float>(hoverCell.x) + offset, height,
                          static_cast<float>(hoverCell.y) + offset,
                          size, 0.012f, size, colour);

        hoverStorey = Lattice::storeyOfZ(hoverCell.z);
    }

    update(scene, hover_, hoverKey, buffer, viewer,
           storeyFlag(hoverStorey) | layerFlag(drawLayer::kOverlays));

    /* ---- 4. the path preview --------------------------------------------
     *
     * A RIBBON PER SEGMENT, ORIENTED ALONG IT — see emitRibbon.
     *
     * THIS WAS AN AXIS-ALIGNED BOX SPANNING THE SEGMENT AND THAT WAS WRONG, in
     * a way worth keeping because the reasoning that produced it was confident
     * and specific. The note said a diagonal's bounding box "reads as a
     * slightly wider link rather than as a rotated one". It does not. Every
     * step of this game's paths is a tile long, so a diagonal's axis-aligned
     * box is a FULL TILE SQUARE lying on the ground — and a path across open
     * ground came out as a row of white plates with the occasional thin bar
     * where a step happened to be axis-aligned. Reported as "it renders
     * multiple squares".
     *
     * The general lesson: a bounding box is only a reasonable stand-in for a
     * shape when it is much longer than it is wide IN THE AXES IT IS BOUND TO.
     * A one-tile diagonal is the exact case where it is not, and it is also the
     * commonest step a tile game takes.
     *
     * ROUTING IT THROUGH DebugDraw INSTEAD would have kept the one-pixel line
     * and was rejected: the path preview is gameplay feedback a player relies
     * on, and putting it behind the dev panel's debug-geometry toggle would let
     * a checkbox nobody associates with movement turn it off. */
    buffer.clear();
    std::uint64_t pathKey = kEmptyKey;
    int pathStorey = 0;

    if (view.preview != nullptr && view.preview->size() >= 2) {
        const std::vector<PathPoint>& path = *view.preview;
        pathKey = kHashSeed;

        /* WIDER THAN A LINE, NARROWER THAN A STEP. `DrawLine3D` is one pixel at
         * any distance, which is what makes immediate-mode geometry read as
         * pasted on; a ribbon recedes with the world. A twentieth of a tile is
         * about the apparent thickness of that line at this camera's height. */
        constexpr float kWidth = 0.05f;

        for (std::size_t i = 0; i + 1 < path.size(); i++) {
            const PathPoint& a = path[i];
            const PathPoint& b = path[i + 1];

            pathKey = hashFloat(pathKey, a.x);
            pathKey = hashFloat(pathKey, a.y);
            pathKey = hashFloat(pathKey, a.height);

            /* LIFTED CLEAR OF THE FLOOR by the same 0.12 the line used. */
            emitRibbon(buffer,
                       Vector3{ a.x, a.height + 0.12f, a.y },
                       Vector3{ b.x, b.height + 0.12f, b.y },
                       kWidth, RAYWHITE);
        }

        const PathPoint& last = path.back();
        pathKey = hashFloat(pathKey, last.x);
        pathKey = hashFloat(pathKey, last.y);
        pathKey = hashFloat(pathKey, last.height);

        /* THE STOREY THE PATH STARTS ON. A path that climbs a ramp spans two,
         * and one bit has to be chosen: the start is the right one, because a
         * preview whose far end is on a hidden floor should still show the part
         * the player can see. Splitting the preview per storey is the same fix
         * the visibility field gets and is not worth it for a dozen boxes. */
        pathStorey = static_cast<int>(std::floor(path.front().height / kStoreyHeight));
    }

    update(scene, path_, pathKey, buffer, viewer,
           storeyFlag(pathStorey) | layerFlag(drawLayer::kOverlays));
}

}  // namespace game
