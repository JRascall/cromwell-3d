/* StaticsMesh.hpp — the baked static world, one mesh per storey and material.
 *
 * SINGLE RESPONSIBILITY: own the GPU meshes and draw them. What they contain
 * is StoreyGeometryEmitter's business.
 *
 * TWO AXES, FOR TWO DIFFERENT REASONS.
 *   per STOREY, so floor isolation stays a draw-time decision and costs
 *     nothing — the cutaway just stops iterating;
 *   per MATERIAL, because a draw call binds one texture set. Splitting here is
 *     what lets a wall and a window carry different albedo, normal and mrao
 *     maps while the whole world is still submitted in a couple of dozen
 *     calls.
 *
 * RAII: the destructor unloads. The C version needed a matching xcStaticsFree
 * on every exit path.
 */
#pragma once

#include "raylib.h"

#include "core/world/World.hpp"
#include "render/material/MaterialLibrary.hpp"
#include "render/style/SurfaceKind.hpp"

#include <array>
#include <vector>

namespace xcom {

class PbrShader;
class PrepassShader;

class StaticsMesh {
public:
    StaticsMesh();
    ~StaticsMesh();

    StaticsMesh(const StaticsMesh&) = delete;
    StaticsMesh& operator=(const StaticsMesh&) = delete;

    /* Safe to call repeatedly — rebuild after anything edits the dataset
     * (destruction, reset). That is the whole contract: edit the data, call
     * rebuild, and the world reflects it. */
    void rebuild(const World& world);

    /* Every sub-mesh through ONE shader, for the passes that only want the
     * geometry's silhouette: the sun's shadow map and the scene prepass.
     *
     * `castersOnly` drops the surfaces that transmit light rather than block
     * it — see castsSunShadow. The shadow map wants it; the prepass does not,
     * because a window is still solid geometry as far as the ribbon's depth
     * test and SSAO are concerned. */
    void draw(int maxStorey, const Material& material, bool castersOnly = false) const;

    /* The same geometry through the prepass shader, but pushing each kind's
     * roughness so the G-buffer's alpha means something. One shared program
     * still, so this is the same single pass — it just stops being blind to
     * which material it is drawing. */
    void drawPrepass(int maxStorey, const Material& material,
                     const MaterialLibrary& library, const PrepassShader& shader) const;

    /* Just one material class — used to draw the glass on its own into the
     * shadow map's transmission plane. */
    void drawKind(int maxStorey, SurfaceKind kind, const Material& material) const;

    /* Each sub-mesh through its own material and scalar factors.
     *
     * Split opaque from transparent because see-through surfaces have to come
     * last, once there is something behind them to see.
     *
     * `includeTransparent` folds them back in, for the flat geometry view —
     * there the whole point is to see where a surface EXISTS, so glass is
     * drawn solid in the ordinary pass with depth writes on rather than
     * blended afterwards. */
    void drawLit(int maxStorey, const MaterialLibrary& library, const PbrShader& shader,
                 bool includeTransparent = false) const;
    void drawTransparentLit(int maxStorey, const MaterialLibrary& library,
                            const PbrShader& shader) const;

    int triangleCount() const { return triangleCount_; }
    int drawCallCount() const { return drawCalls_; }

private:
    void unloadMeshes();

    struct StoreyMesh {
        Mesh mesh = { 0 };
        bool built = false;
    };

    /* [storey][kind] */
    std::vector<std::array<StoreyMesh, kSurfaceKindCount>> storeys_;
    int triangleCount_ = 0;
    int drawCalls_ = 0;
};

}  // namespace xcom
