#include "render/post/AmbientOcclusion.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {

AmbientOcclusion::~AmbientOcclusion()
{
    destroyTargets();
    if (occlusionShader_.id) UnloadShader(occlusionShader_);
    if (blurShader_.id) UnloadShader(blurShader_);
}

/* A cosine-ish hemisphere around +z, packed tighter toward the origin so most
 * samples land near the point being shaded — occlusion falls off with distance,
 * so uniform spacing spends most of its taps where they matter least.
 *
 * Generated from a fixed seed rather than rand(): --shot has to produce the
 * same frame twice. */
void AmbientOcclusion::buildKernel()
{
    unsigned int state = 0x9E3779B9u;
    const auto nextUnit = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;   /* [0,1) */
    };

    for (int i = 0; i < kKernelSize; i++) {
        Vector3 direction{ nextUnit() * 2.0f - 1.0f,
                           nextUnit() * 2.0f - 1.0f,
                           nextUnit() };
        direction = Vector3Normalize(direction);

        /* Packed toward the origin, but not on top of it — see kBias. */
        const float t = static_cast<float>(i) / static_cast<float>(kKernelSize);
        direction = Vector3Scale(direction, 0.25f + 0.75f * t * t);

        kernel_[static_cast<std::size_t>(i) * 3 + 0] = direction.x;
        kernel_[static_cast<std::size_t>(i) * 3 + 1] = direction.y;
        kernel_[static_cast<std::size_t>(i) * 3 + 2] = direction.z;
    }
}

bool AmbientOcclusion::load()
{
    occlusionShader_ = ShaderLibrary::load(nullptr, "ssao.fs.glsl");
    blurShader_      = ShaderLibrary::load(nullptr, "ssao_blur.fs.glsl");
    if (occlusionShader_.id == 0 || blurShader_.id == 0) {
        TraceLog(LOG_WARNING, "SSAO: shaders missing - ambient occlusion is off");
        return false;
    }

    locDepth_             = GetShaderLocation(occlusionShader_, "uDepth");
    locNormals_           = GetShaderLocation(occlusionShader_, "uNormals");
    locProjection_        = GetShaderLocation(occlusionShader_, "uProjection");
    locInverseProjection_ = GetShaderLocation(occlusionShader_, "uInverseProjection");
    locView_              = GetShaderLocation(occlusionShader_, "uView");
    locResolution_        = GetShaderLocation(occlusionShader_, "uResolution");
    locRadius_            = GetShaderLocation(occlusionShader_, "uRadius");
    locBias_              = GetShaderLocation(occlusionShader_, "uBias");
    locStrength_          = GetShaderLocation(occlusionShader_, "uStrength");
    locKernel_            = GetShaderLocation(occlusionShader_, "uKernel");

    locBlurResolution_ = GetShaderLocation(blurShader_, "uResolution");
    locBlurDepth_      = GetShaderLocation(blurShader_, "uDepth");
    locBlurInverseProjection_ =
        GetShaderLocation(blurShader_, "uInverseProjection");

    buildKernel();

    /* The kernel never changes, so it goes up once. */
    SetShaderValueV(occlusionShader_, locKernel_, kernel_.data(),
                    SHADER_UNIFORM_VEC3, kKernelSize);

    shadersLoaded_ = true;
    return true;
}

void AmbientOcclusion::destroyTargets()
{
    if (raw_.id) UnloadRenderTexture(raw_);
    if (blurred_.id) UnloadRenderTexture(blurred_);
    raw_ = RenderTexture2D{ 0 };
    blurred_ = RenderTexture2D{ 0 };
    targetsValid_ = false;
}

void AmbientOcclusion::resize(int width, int height)
{
    destroyTargets();
    if (width < 1 || height < 1) return;

    width_  = width;
    height_ = height;

    /* Full resolution, not the customary half. The occluders here are thin —
     * a wall is 0.09 tiles thick and a ladder rail 0.05 — and at half res
     * their contact shading is a couple of pixels wide before the blur gets to
     * it. The scene is a few thousand untextured triangles; it can afford the
     * fill. */
    raw_     = LoadRenderTexture(width, height);
    blurred_ = LoadRenderTexture(width, height);
    targetsValid_ = (raw_.id != 0 && blurred_.id != 0);

    if (targetsValid_) {
        SetTextureFilter(blurred_.texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(blurred_.texture, TEXTURE_WRAP_CLAMP);
        SetTextureWrap(raw_.texture, TEXTURE_WRAP_CLAMP);
    } else {
        TraceLog(LOG_WARNING, "SSAO: could not allocate targets - occlusion is off");
    }
}

void AmbientOcclusion::render(const Camera3D& camera, Texture2D depth, Texture2D normals)
{
    if (!active()) return;

    /* Rebuilt rather than read back from rlgl: these passes run in 2D, where
     * the current projection is the target's ortho and not the camera's. */
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Matrix view = GetCameraMatrix(camera);
    const Matrix projection = MatrixPerspective(camera.fovy * DEG2RAD, aspect,
                                                rlGetCullDistanceNear(),
                                                rlGetCullDistanceFar());

    const float resolution[2] = { static_cast<float>(width_), static_cast<float>(height_) };
    const float radius = tuning_.radius, bias = tuning_.bias, strength = tuning_.strength;

    {
        BeginTextureMode(raw_);
        ClearBackground(WHITE);
        BeginShaderMode(occlusionShader_);

        /* AFTER BeginShaderMode, not before. SetShaderValueTexture registers
         * the sampler with rlgl's batch, and BeginShaderMode flushes that
         * batch — which clears the registration. Registering first would bind
         * nothing. (The opposite trap to DrawMesh, which never flushes the
         * batch at all; see RibbonShader::setDepthTexture.) */
        SetShaderValueTexture(occlusionShader_, locDepth_, depth);
        SetShaderValueTexture(occlusionShader_, locNormals_, normals);

        SetShaderValueMatrix(occlusionShader_, locProjection_, projection);
        SetShaderValueMatrix(occlusionShader_, locInverseProjection_, MatrixInvert(projection));
        SetShaderValueMatrix(occlusionShader_, locView_, view);
        SetShaderValue(occlusionShader_, locResolution_, resolution, SHADER_UNIFORM_VEC2);
        SetShaderValue(occlusionShader_, locRadius_, &radius, SHADER_UNIFORM_FLOAT);
        SetShaderValue(occlusionShader_, locBias_, &bias, SHADER_UNIFORM_FLOAT);
        SetShaderValue(occlusionShader_, locStrength_, &strength, SHADER_UNIFORM_FLOAT);

        DrawRectangle(0, 0, width_, height_, WHITE);

        EndShaderMode();
        EndTextureMode();
    }

    {
        BeginTextureMode(blurred_);
        ClearBackground(WHITE);
        BeginShaderMode(blurShader_);

        /* AFTER BeginShaderMode, for the same reason as the occlusion pass:
         * BeginShaderMode flushes the batch, and a sampler registered before
         * it would have its registration cleared and bind nothing. */
        SetShaderValueTexture(blurShader_, locBlurDepth_, depth);
        SetShaderValueMatrix(blurShader_, locBlurInverseProjection_,
                             MatrixInvert(projection));
        SetShaderValue(blurShader_, locBlurResolution_, resolution, SHADER_UNIFORM_VEC2);

        /* DrawTexturePro is only here to bind raw_ as texture0 — the shader
         * takes its coordinate from gl_FragCoord, so the source rectangle's
         * orientation does not matter. */
        const Rectangle source{ 0.0f, 0.0f, static_cast<float>(width_),
                                static_cast<float>(height_) };
        DrawTexturePro(raw_.texture, source, source, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);

        EndShaderMode();
        EndTextureMode();
    }
}

Texture2D AmbientOcclusion::texture() const
{
    if (active()) return blurred_.texture;

    /* rlgl's 1x1 white: the lit shader multiplies its ambient by one and never
     * has to know the effect is missing. */
    return Texture2D{ rlGetTextureIdDefault(), 1, 1, 1,
                      PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
}

}  // namespace xcom
