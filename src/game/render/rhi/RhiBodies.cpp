#include "game/render/rhi/RhiBodies.hpp"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/geometry/BoxEmitter.hpp"
#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/render/RenderScene.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "game/render/Palette.hpp"
#include "game/render/DrawLayers.hpp"
#include "game/render/scene/RenderFilter.hpp"

/* FOR centreOffset ONLY — where a body stands relative to its cell, which is a
 * fact about footprints rather than about raylib. Reused rather than restated
 * so a 2x2 hull cannot end up half a tile apart in the two renderers; it moves
 * here when UnitRenderer is deleted at parity. */
#include "game/render/scene/UnitRenderer.hpp"
#include "game/units/kinds/Unit.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

namespace game {
namespace {

/* BYTE OVER 255, WITH NO sRGB DECODE — matching what the static world's vertex
 * colours already do, since UByte4Normalised hands the shader the same 0..1
 * without one. Decoding here and not there would make a body and a wall painted
 * the same palette colour come out different shades, which looks like a
 * lighting bug and is a colour-space one. */
cromwell::Vec4 tintOf(Color colour)
{
    constexpr float kToUnit = 1.0f / 255.0f;
    return cromwell::Vec4{ static_cast<float>(colour.r) * kToUnit,
                           static_cast<float>(colour.g) * kToUnit,
                           static_cast<float>(colour.b) * kToUnit,
                           1.0f };
}

/* THE UNIT CUBE'S OWN EXTENT. emitBox centres on the position it is given, so a
 * 1x1x1 box at the origin spans half a unit each way — which is exactly what
 * the transform below scales and translates. Stating it here rather than at the
 * registration keeps the mesh and its bounds one fact; see
 * RenderableDesc::withMesh on why those two must never be set separately. */
const cromwell::Aabb kCubeBounds{ cromwell::Vec3{ -0.5f, -0.5f, -0.5f },
                                  cromwell::Vec3{ 0.5f, 0.5f, 0.5f } };

}  // namespace

RhiBodies::RhiBodies(cromwell::rhi::IRenderDevice& device) : device_(device) {}

RhiBodies::~RhiBodies() { release(); }

void RhiBodies::removeAll()
{
    if (scene_ != nullptr) {
        for (Body& body : bodies_)
            for (int i = 0; i < body.partCount; i++)
                if (body.parts[i].valid()) scene_->remove(body.parts[i]);
    }
    bodies_.clear();
    renderableCount_ = 0;
}

void RhiBodies::release()
{
    /* THE RENDERABLES FIRST, THEN THE MESH THEY NAME. See RhiStatics::release —
     * a scene holds a reference, so destroying the cube while a renderable
     * still points at it leaves the scene able to ask a dead handle to draw. */
    removeAll();
    scene_ = nullptr;

    /* THE MESH AND ITS BUFFER ARE SEPARATE OBJECTS and both are ours — the
     * device deliberately does not destroy a mesh's buffers with it, because
     * meshes routinely share them. This one does not, so both go. */
    if (cube_.valid())         device_.destroy(cube_);
    if (cubeVertices_.valid()) device_.destroy(cubeVertices_);
    cube_ = {};
    cubeVertices_ = {};
}

bool RhiBodies::build()
{
    release();

    /* THE SAME EMITTER THE WORLD IS BUILT WITH, at the origin and unit sized —
     * so a body's faces carry the same normals, tangents and UVs a wall does,
     * and the one shader shades both without knowing which it has. Building a
     * cube by hand here would be the moment bodies started reading as stickers
     * on a lit scene.
     *
     * WHITE, because the colour arrives per renderable as a tint. */
    cromwell::MeshVertexBuffer buffer;
    cromwell::emitBox(buffer, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, WHITE);

    const std::vector<std::uint8_t> packed = buffer.interleave();
    const uint32_t vertices = static_cast<uint32_t>(buffer.vertexCount());
    if (vertices == 0) return false;

    cromwell::rhi::BufferDesc desc;
    desc.name  = "unit cube";
    desc.bytes = packed.size();
    desc.usage = cromwell::rhi::BufferUsageVertex;

    /* Uploaded once and never touched again — a cube does not change shape.
     * Everything that varies per body is on the renderable. */
    desc.access = cromwell::rhi::BufferAccess::CpuToGpuOnce;

    cubeVertices_ = device_.createBuffer(desc);
    if (!cubeVertices_.valid()) return false;

    device_.updateBuffer(cubeVertices_, packed.data(), packed.size(), 0);

    /* NON-INDEXED, matching the emitter's triangle soup and RhiStatics. */
    cube_ = device_.createMesh(cromwell::MeshVertexBuffer::deviceLayout(),
                               cubeVertices_, vertices);
    return cube_.valid();
}

int RhiBodies::partsOf(const Unit& unit, std::array<Part, kMaxParts>& out)
{
    const bool enemy = unit.team() == Team::Enemy;

    /* THE PART OFFSETS ARE UnitRenderer::drawBody's, VERBATIM. Copied rather
     * than shared for the duration of the migration, exactly as RhiStatics
     * copies StaticsMesh's slot arithmetic — a shared helper threaded through
     * both would be work thrown away the day one of them is deleted. Changing a
     * body's shape means changing it in both places until then. */
    switch (unit.presentation().visual()) {
        case VisualKind::Vehicle: {
            const Color hull = enemy ? palette::kEnemy : palette::kPlayerVehicle;
            out[0] = Part{ cromwell::Vec3{ 0.0f, 0.28f, 0.0f },
                           cromwell::Vec3{ 1.82f, 0.50f, 1.82f }, tintOf(hull) };
            out[1] = Part{ cromwell::Vec3{ 0.0f, 0.70f, 0.0f },
                           cromwell::Vec3{ 0.90f, 0.34f, 0.90f },
                           tintOf(palette::kVehicleTurret) };
            out[2] = Part{ cromwell::Vec3{ 0.0f, 0.72f, 0.95f },
                           cromwell::Vec3{ 0.13f, 0.13f, 1.15f },
                           tintOf(palette::kVehicleBarrel) };
            return 3;
        }
        case VisualKind::Infantry: {
            const Color colour = enemy ? palette::kEnemy : palette::kPlayerSoldier;
            out[0] = Part{ cromwell::Vec3{ 0.0f, 0.45f, 0.0f },
                           cromwell::Vec3{ 0.42f, 0.90f, 0.42f }, tintOf(colour) };
            return 1;
        }
    }
    return 0;
}

void RhiBodies::reregister(cromwell::RenderScene& scene, const UnitRoster& roster)
{
    removeAll();
    scene_ = &scene;

    std::array<Part, kMaxParts> parts;

    for (const std::unique_ptr<Unit>& unit : roster) {
        Body body;
        body.unit = unit.get();
        body.partCount = partsOf(*unit, parts);

        for (int i = 0; i < body.partCount; i++) {
            /* THE MATERIAL, ONCE, ON THE RENDERABLE — AND THIS IS A REAL BUG
             * MADE UNREPEATABLE. Bodies used to be submitted after the statics
             * in the same pass, and the statics bound a material per bucket, so
             * a body that bound none inherited whichever surface kind was drawn
             * last. The cutaway decides which buckets are submitted, so changing
             * the ISO LEVEL changed what every unit was made of: mirror-smooth
             * at one storey cut, matte at another, with nothing in the material
             * system to explain it because the material system was never asked.
             *
             * A renderable cannot express that. It names its material as data
             * and the engine binds it; there is no "inherit" to fall into. */
            const cromwell::RenderableDesc desc =
                cromwell::RenderableDesc()
                    .withMesh(cube_, kCubeBounds)
                    .withMaterial(cromwell::DeviceMaterials::idOf(cromwell::SurfaceKind::Body))
                    .withTint(parts[i].tint);

            /* NO FILTER FLAGS HERE, DELIBERATELY, AND THE OMISSION IS THE
             * INTERESTING PART. sync() below writes the whole word every frame
             * — a body changes storey by walking up a ramp — so anything set
             * here is overwritten before it is ever read.
             *
             * Setting the draw-layer bit at registration and nowhere else is
             * exactly the mistake that was made, and it produced a "units"
             * checkbox that moved and changed NOTHING while the same checkbox
             * for statics and overlays worked: those two set their flags where
             * they are last written, and this one set them where they are last
             * OVERWRITTEN. Measured as a zero-pixel difference against the
             * frame with units switched on, which is indistinguishable from the
             * switch never having been wired at all.
             *
             * The rule: WHEN A FIELD IS WRITTEN WHOLESALE PER FRAME, IT HAS
             * EXACTLY ONE PLACE TO BE SET, and it is that write. */

            body.parts[i] = scene.add(desc);
            if (body.parts[i].valid()) renderableCount_++;
        }
        bodies_.push_back(body);
    }
}

void RhiBodies::sync(cromwell::RenderScene& scene, const UnitRoster& roster,
                     const World& world, const Unit* animating,
                     float animatedX, float animatedHeight, float animatedY)
{
    /* ONE ZONE FOR THE SYSTEM, per CLAUDE.md. It is a walk over a roster of
     * dozens writing a transform each, so it earns exactly one row and no
     * sub-zones - and it earns that one because an UNZONED per-frame system
     * does not show up as a zero, it shows up as nothing at all and inflates
     * whatever encloses it. */
    CW_PROFILE_ZONE_N("body sync");

    if (!cube_.valid()) return;

    /* ---- has the roster changed SHAPE since last frame? -----------------
     *
     * A POINTER COMPARE PER UNIT, not a hash and not a generation counter. The
     * roster is a handful of units and this is the cheapest thing that is
     * actually correct: a unit added, removed or reordered shows up here, and
     * anything else is the ordinary frame where every pointer matches and the
     * loop below just writes transforms.
     *
     * WHY NOT TRUST THE ROSTER TO BE STABLE. Because it is stable TODAY —
     * `isDead()` is a state rather than a deletion — and a renderer that
     * assumed so would break silently the day reinforcements or a corpse
     * cleanup arrived, with soldiers wearing each other's positions. The check
     * costs a compare per unit per frame and removes the assumption. */
    bool shapeChanged = false;
    {
        std::size_t index = 0;
        for (const std::unique_ptr<Unit>& unit : roster) {
            if (index >= bodies_.size() || bodies_[index].unit != unit.get()) {
                shapeChanged = true;
                break;
            }
            index++;
        }
        if (!shapeChanged && index != bodies_.size()) shapeChanged = true;
        if (scene_ != &scene) shapeChanged = true;
    }

    if (shapeChanged) reregister(scene, roster);

    std::array<Part, kMaxParts> parts;
    std::size_t index = 0;

    for (const std::unique_ptr<Unit>& unit : roster) {
        if (index >= bodies_.size()) break;
        const Body& body = bodies_[index++];

        /* WHERE THIS BODY STANDS. The walking unit is at the position the
         * animator interpolated rather than at the cell it logically occupies;
         * everything else is at its cell.
         *
         * 2x2 hulls centre on their footprint and 1x1 bodies on their own tile
         * — the same offset UnitRenderer::centreOffset computes, and reused
         * from it rather than restated so the two cannot disagree about where a
         * vehicle stands. */
        const float offset = UnitRenderer::centreOffset(*unit);

        float x = static_cast<float>(unit->position().x) + offset;
        float base = unit->baseHeight(world);
        float y = static_cast<float>(unit->position().y) + offset;

        if (unit.get() == animating) {
            x = animatedX;
            base = animatedHeight;
            y = animatedY;
        }

        /* THE STOREY THE CUTAWAY WILL JUDGE IT BY. Recomputed every frame
         * because a unit changes storey by walking up a ramp, and a filter flag
         * latched at registration would leave it visible under a cut it should
         * be hidden by — which reads as "the iso level does not hide that
         * soldier" and has nothing to do with the iso level. */
        /* THE STOREY IT IS STANDING ON, PLUS THE CATEGORY IT IS IN, and both
         * belong in this one write because this write is TOTAL — see the note
         * at the registration above, which is where the layer bit was set the
         * first time and where it was silently thrown away every frame after.
         *
         * The layer bit is what the dev panel's "units" checkbox hides, and the
         * view hides it in every DERIVED view too, so switching units off takes
         * their shadows with them rather than leaving unit-shaped darkness on a
         * floor with no units on it. See View::withAlwaysHiddenFlags. */
        const cromwell::FilterFlags flags =
            storeyFlag(Lattice::storeyOfZ(unit->position().z))
            | layerFlag(drawLayer::kUnits);

        /* ---- AND WHAT THIS BODY IS, for the custom depth buffer -----------
         *
         * ONE ID PER UNIT, from its roster index plus one — Unreal's
         * CustomDepthStencilValue, and the same numbering the raylib path uses
         * (kFirstUnitStencil counts from 1). PLUS ONE because zero means "not
         * in the pass", so unit 0 would otherwise be the one soldier that could
         * never be outlined.
         *
         * WRITTEN HERE, IN THE PER-FRAME SYNC, and not at registration — for
         * exactly the reason the layer bit is: this write is TOTAL, and a value
         * set once at registration is silently overwritten before anything
         * reads it. That mistake produced a units checkbox that measured zero
         * pixels; see MIGRATION.md §5.
         *
         * CLAMPED, because the value is a byte and a roster longer than 254
         * would wrap onto somebody else's id — two soldiers sharing an outline,
         * which reads as a selection bug. A tactical board has a dozen; the
         * clamp costs nothing and makes the ceiling visible. */
        const std::size_t ordinal = index;   /* already advanced past this unit */
        const std::uint8_t stencil =
            (ordinal <= 254) ? static_cast<std::uint8_t>(ordinal) : 0;

        const bool visible = !unit->isDead();

        const int partCount = partsOf(*unit, parts);
        for (int i = 0; i < partCount && i < body.partCount; i++) {
            const cromwell::RenderableId id = body.parts[i];
            if (!id.valid()) continue;

            const cromwell::Vec3 centre{ x + parts[i].offset.x,
                                         base + parts[i].offset.y,
                                         y + parts[i].offset.z };

            /* SCALE THEN TRANSLATE. The other order scales the translation too
             * and puts every body somewhere out along the ray from the origin
             * through where it should have been — which reads as "the units are
             * in the wrong place" rather than as a matrix order mistake. */
            scene.setTransform(id, cromwell::Mat4::translation(centre)
                                 * cromwell::Mat4::scaling(parts[i].size));
            scene.setFilterFlags(id, flags);
            scene.setCustomStencil(id, stencil);
            scene.setVisible(id, visible);
        }
    }
}

}  // namespace game
