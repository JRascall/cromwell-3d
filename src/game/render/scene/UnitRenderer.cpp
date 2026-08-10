#include "game/render/scene/UnitRenderer.hpp"

#include "game/units/roster/UnitRoster.hpp"
#include "cromwell/material/PbrMaterial.hpp"
#include "game/render/Palette.hpp"

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

UnitRenderer::UnitRenderer(const World& world)
    : world_(world), box_(), pass_(LoadMaterialDefault()) {}

float UnitRenderer::centreOffset(const Unit& unit)
{
    return unit.footprint().isMultiTile() ? 1.0f : 0.5f;
}

void UnitRenderer::beginPass(const Material& passMaterial)
{
    pass_.shader = passMaterial.shader;

    /* Element-wise, NOT `pass_.maps = passMaterial.maps` — see the header. The
     * shadow map the lit pass parks in MATERIAL_MAP_NORMAL has to come across
     * with it, so this copies every slot rather than just the diffuse one.
     *
     * kMapCount is our own slot map (PbrMaterial.hpp) rather than raylib's
     * MAX_MATERIAL_MAPS, which lives in its config.h and not its public
     * header. Anything past our slots is unused by every shader here. */
    for (int slot = 0; slot < kMapCount; slot++)
        pass_.maps[slot] = passMaterial.maps[slot];
}

void UnitRenderer::drawPart(float offsetX, float offsetY, float offsetZ,
                            float sizeX, float sizeY, float sizeZ, Color albedo)
{
    pass_.maps[MATERIAL_MAP_DIFFUSE].color = albedo;

    box_.draw(pass_,
              pendingX_ + offsetX, pendingBase_ + offsetY, pendingY_ + offsetZ,
              sizeX, sizeY, sizeZ);
}

void UnitRenderer::drawBody(const Unit& unit)
{
    const bool enemy = unit.team() == Team::Enemy;

    switch (unit.presentation().visual()) {
        case VisualKind::Vehicle: {
            const Color hull = enemy ? palette::kEnemy : palette::kPlayerVehicle;
            drawPart(0.0f, 0.28f, 0.0f, 1.82f, 0.50f, 1.82f, hull);
            drawPart(0.0f, 0.70f, 0.0f, 0.90f, 0.34f, 0.90f, palette::kVehicleTurret);
            drawPart(0.0f, 0.72f, 0.95f, 0.13f, 0.13f, 1.15f, palette::kVehicleBarrel);
            break;
        }
        case VisualKind::Infantry: {
            const Color colour = enemy ? palette::kEnemy : palette::kPlayerSoldier;
            drawPart(0.0f, 0.45f, 0.0f, 0.42f, 0.90f, 0.42f, colour);
            break;
        }
    }
}

void UnitRenderer::drawAt(const Unit& unit, float x, float baseHeight, float y,
                          const Material& material)
{
    pendingX_    = x;
    pendingY_    = y;
    pendingBase_ = baseHeight;
    beginPass(material);
    drawBody(unit);
}

void UnitRenderer::drawRoster(const UnitRoster& roster, int maxStorey, const Unit* skip,
                              const Material& material, const UnitTag& tag)
{
    for (const std::unique_ptr<Unit>& unit : roster) {
        if (unit->isDead() || unit.get() == skip) continue;
        if (Lattice::storeyOfZ(unit->position().z) > maxStorey) continue;

        /* After the skips, so a caller counting bodies counts DRAWN ones. */
        if (tag) tag(*unit);

        const float offset = centreOffset(*unit);
        drawAt(*unit,
               static_cast<float>(unit->position().x) + offset,
               unit->baseHeight(world_),
               static_cast<float>(unit->position().y) + offset,
               material);
    }
}

}  // namespace game
