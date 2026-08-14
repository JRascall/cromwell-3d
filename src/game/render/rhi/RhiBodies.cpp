#include "game/render/rhi/RhiBodies.hpp"

#include "cromwell/material/DeviceMaterials.hpp"

#include "cromwell/geometry/BoxEmitter.hpp"
#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/render/IGeometrySource.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "game/render/Palette.hpp"

/* FOR centreOffset ONLY — where a body stands relative to its cell, which is a
 * fact about footprints rather than about raylib. Reused rather than restated
 * so a 2x2 hull cannot end up half a tile apart in the two renderers; it moves
 * here when UnitRenderer is deleted at parity. */
#include "game/render/scene/UnitRenderer.hpp"
#include "game/units/kinds/Unit.hpp"
#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

namespace game {

RhiBodies::RhiBodies(cromwell::rhi::IRenderDevice& device) : device_(device) {}

RhiBodies::~RhiBodies() { release(); }

void RhiBodies::release()
{
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
     * WHITE, because the colour arrives per draw in the object push. See
     * rhi/object.glsl. */
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
     * Everything that varies per body is a push constant. */
    desc.access = cromwell::rhi::BufferAccess::CpuToGpuOnce;

    cubeVertices_ = device_.createBuffer(desc);
    if (!cubeVertices_.valid()) return false;

    device_.updateBuffer(cubeVertices_, packed.data(), packed.size(), 0);

    /* NON-INDEXED, matching the emitter's triangle soup and RhiStatics. */
    cube_ = device_.createMesh(cromwell::MeshVertexBuffer::deviceLayout(),
                               cubeVertices_, vertices);
    return cube_.valid();
}

void RhiBodies::submitPart(cromwell::rhi::ICommandEncoder& encoder,
                           float centreX, float centreY, float centreZ,
                           float sizeX, float sizeY, float sizeZ,
                           unsigned char r, unsigned char g, unsigned char b) const
{
    /* BYTE OVER 255, WITH NO sRGB DECODE — matching what the static world's
     * vertex colours already do, since UByte4Normalised hands the shader the
     * same 0..1 without one. Decoding here and not there would make a body and
     * a wall painted the same palette colour come out different shades, which
     * looks like a lighting bug and is a colour-space one. */
    constexpr float kToUnit = 1.0f / 255.0f;

    const cromwell::ObjectPush push = cromwell::ObjectPush::box(
        cromwell::Vec3{ centreX, centreY, centreZ },
        cromwell::Vec3{ sizeX, sizeY, sizeZ },
        static_cast<float>(r) * kToUnit,
        static_cast<float>(g) * kToUnit,
        static_cast<float>(b) * kToUnit);

    encoder.pushConstants(&push, sizeof push);
    encoder.draw(cube_);
    drawCalls_++;
}

void RhiBodies::submitBody(cromwell::rhi::ICommandEncoder& encoder, const Unit& unit,
                           float x, float baseHeight, float y) const
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
            submitPart(encoder, x, baseHeight + 0.28f, y, 1.82f, 0.50f, 1.82f,
                       hull.r, hull.g, hull.b);
            submitPart(encoder, x, baseHeight + 0.70f, y, 0.90f, 0.34f, 0.90f,
                       palette::kVehicleTurret.r, palette::kVehicleTurret.g,
                       palette::kVehicleTurret.b);
            submitPart(encoder, x, baseHeight + 0.72f, y + 0.95f, 0.13f, 0.13f, 1.15f,
                       palette::kVehicleBarrel.r, palette::kVehicleBarrel.g,
                       palette::kVehicleBarrel.b);
            break;
        }
        case VisualKind::Infantry: {
            const Color colour = enemy ? palette::kEnemy : palette::kPlayerSoldier;
            submitPart(encoder, x, baseHeight + 0.45f, y, 0.42f, 0.90f, 0.42f,
                       colour.r, colour.g, colour.b);
            break;
        }
    }
}

void RhiBodies::submit(cromwell::rhi::ICommandEncoder& encoder, const UnitRoster& roster,
                       const World& world, int maxStorey,
                       const Unit* animating, float animatedX, float animatedHeight,
                       float animatedY,
                       const cromwell::DeviceMaterials* materials) const
{
    drawCalls_ = 0;
    if (!cube_.valid()) return;

    /* WHAT A SOLDIER IS MADE OF, bound ONCE rather than per body — every body
     * on the board is the same material, and rebinding it per unit would be a
     * uniform buffer bind per draw to say the same thing.
     *
     * BINDING IT AT ALL IS THE FIX for a bug that read as nonsense: bodies are
     * drawn after the statics in the same pass, and the statics bind a material
     * per bucket, so a body that bound none inherited whichever surface kind was
     * drawn last. The cutaway decides which buckets are submitted, so changing
     * the ISO LEVEL changed what every unit was made of — mirror-smooth at one
     * storey cut, matte at another. Nothing in the material system could explain
     * it, because the material system was never asked. */
    if (materials != nullptr) materials->bind(encoder, cromwell::SurfaceKind::Body);

    for (const std::unique_ptr<Unit>& unit : roster) {
        if (unit->isDead() || unit.get() == animating) continue;
        if (Lattice::storeyOfZ(unit->position().z) > maxStorey) continue;

        /* 2x2 hulls centre on their footprint, 1x1 bodies on their own tile —
         * the same offset UnitRenderer::centreOffset computes, and reused from
         * it rather than restated so the two cannot disagree about where a
         * vehicle stands. */
        const float offset = UnitRenderer::centreOffset(*unit);

        submitBody(encoder, *unit,
                   static_cast<float>(unit->position().x) + offset,
                   unit->baseHeight(world),
                   static_cast<float>(unit->position().y) + offset);
    }

    /* THE WALKING BODY, at the position the animator interpolated rather than
     * at the cell it logically occupies. Drawn last, which costs nothing here —
     * these are opaque boxes and the depth test decides what is in front. */
    if (animating != nullptr)
        submitBody(encoder, *animating, animatedX, animatedHeight, animatedY);
}

}  // namespace game
