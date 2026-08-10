/* UnitRenderer.hpp — units, drawn as lit boxes.
 *
 * SINGLE RESPONSIBILITY: draw bodies. The static world is baked in
 * StaticsMesh; a unit is a handful of boxes whose position changes every
 * frame, so they are drawn as transforms of one shared cube (see BoxMesh)
 * rather than baked or re-uploaded.
 *
 * The soldier/vehicle difference is a PresentationComponent field, not a type
 * system rather than by a `kind == TANK` test in the middle of a draw call.
 *
 * THE MATERIAL IS PASSED IN, not owned, because the same bodies are drawn
 * three times a frame through different shaders: once into the sun's shadow
 * map, once into the ribbon's depth prepass, and once lit. Only the caller
 * knows which pass is running, and this class does not have to.
 */
#pragma once

/* raylib.h MUST precede rlgl.h — rlgl defines its own Matrix otherwise, and
 * the two collide. Including it here keeps every consumer safe. */
#include "raylib.h"

#include "game/units/kinds/Unit.hpp"
#include "game/world/World.hpp"
#include "cromwell/geometry/BoxMesh.hpp"

#include <functional>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

class Unit;
class UnitRoster;

class UnitRenderer {
public:
    explicit UnitRenderer(const World& world);

    /* Invoked immediately before each unit is drawn, for a pass that needs to
     * say something per object — the custom depth pass tags each body with its
     * own stencil value this way.
     *
     * A callback rather than a per-unit parameter because only the CALLER
     * knows what it wants to vary, and this class should not learn about
     * stencils, ids or outlines to let it. */
    using UnitTag = std::function<void(const Unit&)>;

    /* `skip` (may be nullptr) is drawn by the caller instead — used while a
     * unit is animating along a path, where its logical cell and its drawn
     * position differ. */
    void drawRoster(const UnitRoster& roster, int maxStorey, const Unit* skip,
                    const Material& material, const UnitTag& tag = {});

    /* Draw one unit at an arbitrary world position (the animating case). */
    void drawAt(const Unit& unit, float x, float baseHeight, float y,
                const Material& material);

    /* The centre offset a body is drawn at: 2x2 hulls centre on their
     * footprint, 1x1 bodies on their own tile. */
    static float centreOffset(const Unit& unit);

    /* The mesh recipe for one body, chosen by its PresentationComponent.
     * A closed set: adding a body means teaching this to draw it, which is
     * exactly the edit the compiler should demand. */
    void drawBody(const Unit& unit);

private:
    /* Copies the pass material's CONTENTS into pass_, which this class owns
     * and is free to tint.
     *
     * Material::maps is a HEAP POINTER, so `Material copy = other;` shares the
     * map array rather than duplicating it — writing a tint through such a
     * copy reaches back into whatever material it came from. Doing exactly
     * that is what silently repainted the whole static world in the last unit
     * drawn: the world is submitted before the bodies, so from the second
     * frame on it was picking up the previous frame's leftover tint. */
    void beginPass(const Material& passMaterial);

    /* Tints pass_ with this part's albedo and draws one box. In the depth
     * passes the tint is set and then ignored, which is cheaper than asking
     * the caller to say which kind of pass it is. */
    void drawPart(float offsetX, float offsetY, float offsetZ,
                  float sizeX, float sizeY, float sizeZ, Color albedo);

    const World& world_;
    BoxMesh      box_;

    /* Our own material, with our own map array. Never unloaded: its default
     * texture is rlgl's shared one, and UnloadMaterial would take that with
     * it. Same lifetime rule the material library follows. */
    Material pass_;

    /* set by drawAt before dispatching to a visit() */
    float pendingX_ = 0.0f;
    float pendingY_ = 0.0f;
    float pendingBase_ = 0.0f;
};

}  // namespace game
