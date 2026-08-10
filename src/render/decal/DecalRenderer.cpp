#include "render/decal/DecalRenderer.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {
namespace {

/* Map slots on the decal material. The first three are this decal's own art;
 * the last two are the frame's prepass attachments.
 *
 * THE PREPASS PLANES TRAVEL IN MAP SLOTS RATHER THAN ON FIXED UNITS, which is
 * the opposite of what PbrShader does with the shadow map and for a good
 * reason: this pass draws with DrawMesh, and DrawMesh binds material.maps[i]
 * itself. SetShaderValueTexture would hand the texture to rlgl's batch instead,
 * which DrawMesh never flushes — the sampler would point at an unbound unit and
 * every decal would silently unproject a depth buffer of zeroes. RibbonShader
 * documents the same trap; it cost that system a total disappearance. */
enum : int {
    kMapDecalAlbedo = 0,
    kMapDecalPacked = 1,
    kMapDecalNormal = 2,
    kMapSceneDepth  = 3,
    kMapSceneNormal = 4,
};

}  // namespace

DecalRenderer::~DecalRenderer()
{
    if (cube_.vertexCount) UnloadMesh(cube_);
    if (shader_.id) UnloadShader(shader_);

    /* The material's maps are borrowed — every texture in them belongs to
     * either DecalSet or the prepass — so only the material shell is released,
     * and not through UnloadMaterial, which would take the textures with it. */
}

bool DecalRenderer::load()
{
    shader_ = ShaderLibrary::load("decal.vs.glsl", "decal.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "DECAL: shaders missing - decals are off");
        return false;
    }

    locInverseModel_          = GetShaderLocation(shader_, "uInverseModel");
    locInverseViewProjection_ = GetShaderLocation(shader_, "uInverseViewProjection");
    locResolution_            = GetShaderLocation(shader_, "uResolution");
    locTint_                  = GetShaderLocation(shader_, "uTint");
    locFactors_               = GetShaderLocation(shader_, "uFactors");
    locFade_                  = GetShaderLocation(shader_, "uFade");
    locWrap_                  = GetShaderLocation(shader_, "uWrap");

    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapDecalAlbedo] =
        GetShaderLocation(shader_, "uDecalAlbedo");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapDecalPacked] =
        GetShaderLocation(shader_, "uDecalPacked");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapDecalNormal] =
        GetShaderLocation(shader_, "uDecalNormal");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapSceneDepth] =
        GetShaderLocation(shader_, "uSceneDepth");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapSceneNormal] =
        GetShaderLocation(shader_, "uSceneNormals");

    /* -0.5 to +0.5 on every axis, which is the local space Decal.hpp is written
     * against. Twelve triangles, submitted once per decal. */
    cube_ = GenMeshCube(1.0f, 1.0f, 1.0f);
    if (cube_.vertexCount == 0) {
        TraceLog(LOG_WARNING, "DECAL: could not build the projector box - decals are off");
        return false;
    }

    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    return true;
}

void DecalRenderer::draw(const Decal& decal, const DecalSet& decals) const
{
    const DecalSet::MaterialTextures& textures = decals.textures(decal.material);

    /* A decal whose material never loaded has no albedo, and an albedo is the
     * one map that cannot fall back to a constant — its ALPHA is the decal's
     * shape. Drawing it would ink the whole box as a solid rectangle. */
    if (textures.albedo.id == 0) return;

    material_.maps[kMapDecalAlbedo].texture = textures.albedo;
    material_.maps[kMapDecalPacked].texture = textures.packed;
    material_.maps[kMapDecalNormal].texture = textures.normal;

    /* raylib does not build the inverse for us, and this pass needs it for
     * every fragment: it is what turns a recovered world position into the
     * decal's own space, where the box is the unit cube and the UV is a shift. */
    SetShaderValueMatrix(shader_, locInverseModel_, MatrixInvert(decal.transform));

    const float tint[4] = { decal.tint.r / 255.0f, decal.tint.g / 255.0f,
                            decal.tint.b / 255.0f, decal.tint.a / 255.0f };
    const float factors[4] = { decal.roughness, decal.metalness,
                               decal.normalStrength, decal.opacity };
    const float fade[4] = { decal.angleFadeStart, decal.angleFadeEnd,
                            decal.depthFade, decal.emissive };

    const float wrap = decal.wrap ? 1.0f : 0.0f;

    SetShaderValue(shader_, locTint_,    tint,    SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locFactors_, factors, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locFade_,    fade,    SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locWrap_,    &wrap,   SHADER_UNIFORM_FLOAT);

    DrawMesh(cube_, material_, decal.transform);
}

void DecalRenderer::render(const DecalSet& decals, const Camera3D& camera,
                           const DecalBuffer& buffer,
                           Texture2D sceneDepth, Texture2D sceneNormals,
                           const Decal* preview) const
{
    if (!valid() || !buffer.valid()) return;

    DecalBuffer::Scope scope(buffer);

    /* CLEARED EVEN WITH NOTHING TO DRAW, and this is not a formality: the lit
     * pass samples these planes unconditionally, so a buffer left holding last
     * frame's decals would keep painting them after the last one was removed —
     * and the dev tool's ghost, which is rebuilt every frame, would smear a
     * trail of itself across the world as the cursor moved. */
    ClearBackground(DecalBuffer::kEmpty);

    const std::vector<Decal>& list = decals.inDrawOrder();
    if (list.empty() && preview == nullptr) return;

    /* Rebuilt from the camera rather than read back out of rlgl, exactly as
     * AmbientOcclusion does and for the same reason — and it MUST agree with
     * the prepass, because that is the depth buffer being unprojected. The
     * aspect comes from the buffer, which is the prepass's own size. */
    const float aspect = static_cast<float>(buffer.width())
                       / static_cast<float>(buffer.height());
    const Matrix view = GetCameraMatrix(camera);
    const Matrix projection = MatrixPerspective(camera.fovy * DEG2RAD, aspect,
                                                rlGetCullDistanceNear(),
                                                rlGetCullDistanceFar());
    /* raylib's MatrixMultiply(A, B) reaches GLSL as B * A, so this is the
     * shader's `projection * view` and its inverse takes clip space to world. */
    const Matrix inverseViewProjection = MatrixInvert(MatrixMultiply(view, projection));

    material_.maps[kMapSceneDepth].texture  = sceneDepth;
    material_.maps[kMapSceneNormal].texture = sceneNormals;

    BeginMode3D(camera);

    SetShaderValueMatrix(shader_, locInverseViewProjection_, inverseViewProjection);
    const float resolution[2] = { static_cast<float>(buffer.width()),
                                  static_cast<float>(buffer.height()) };
    SetShaderValue(shader_, locResolution_, resolution, SHADER_UNIFORM_VEC2);

    /* ---- the state the DBuffer blend needs — see the header ---------------- */
    rlDisableDepthTest();
    rlDisableDepthMask();
    rlSetCullFace(RL_CULL_FACE_FRONT);

    /* rgb:   src + dst * src.a   — premultiplied over
     * alpha:       dst * src.a   — transmittance MULTIPLIES, so N decals leave
     *                              prod(1 - coverage) behind for the base. */
    rlSetBlendFactorsSeparate(RL_ONE, RL_SRC_ALPHA, RL_ZERO, RL_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);

    for (const Decal& decal : list) draw(decal, decals);

    /* LAST, so the ghost sits over everything already down — including a decal
     * it is about to be placed on top of, which is exactly the comparison the
     * preview is for. */
    if (preview != nullptr) draw(*preview, decals);

    /* EVERY ONE OF THESE HAS TO GO BACK. rlgl caches its blend mode, so leaving
     * CUSTOM_SEPARATE set means the next pass that asks for ordinary alpha
     * blending is told it already has it and gets this equation instead —
     * which, on the lit pass, composites the whole world premultiplied. */
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlSetCullFace(RL_CULL_FACE_BACK);
    rlEnableDepthMask();
    rlEnableDepthTest();

    EndMode3D();
}

}  // namespace xcom
