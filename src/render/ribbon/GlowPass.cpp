#include "render/ribbon/GlowPass.hpp"

#include "render/gpu/ShaderLibrary.hpp"
#include "render/ribbon/RibbonConstants.hpp"

namespace xcom {

GlowPass::~GlowPass()
{
    if (blurShader_.id) UnloadShader(blurShader_);
}

bool GlowPass::loadShader()
{
    blurShader_  = ShaderLibrary::load(nullptr, "ribbon_glow.fs.glsl");
    locTexelDir_ = GetShaderLocation(blurShader_, "uTexelDir");
    locScale_    = GetShaderLocation(blurShader_, "uScale");
    return blurShader_.id != 0;
}

void GlowPass::resize(int screenWidth, int screenHeight)
{
    int halfWidth  = screenWidth / 2;
    int halfHeight = screenHeight / 2;
    if (halfWidth < 1) halfWidth = 1;
    if (halfHeight < 1) halfHeight = 1;

    const bool ok = source_.create(screenWidth, screenHeight)
                  & pingA_.create(halfWidth, halfHeight)
                  & pingB_.create(halfWidth, halfHeight)
                  & sum_.create(halfWidth, halfHeight);

    available_ = blurShader_.id != 0 && ok;
    if (!available_)
        TraceLog(LOG_WARNING, "RIBBON: no HDR glow targets - ribbon will not bloom");
}

void GlowPass::blit(Texture2D source, float destWidth, float destHeight)
{
    const Rectangle sourceRect = { 0.0f, 0.0f,
                                   static_cast<float>(source.width),
                                   -static_cast<float>(source.height) };
    const Rectangle destRect = { 0.0f, 0.0f, destWidth, destHeight };
    DrawTexturePro(source, sourceRect, destRect, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
}

void GlowPass::setBlurUniforms(float dirX, float dirY, float scale) const
{
    const float direction[2] = { dirX, dirY };
    SetShaderValue(blurShader_, locTexelDir_, direction, SHADER_UNIFORM_VEC2);
    SetShaderValue(blurShader_, locScale_,    &scale,    SHADER_UNIFORM_FLOAT);
}

/* BLEND_ADD_COLORS onto a cleared target is a plain copy that, unlike
 * BLEND_ALPHA, does not multiply the colour by an alpha we are not using. */
void GlowPass::blurPass(HdrTarget& destination, Texture2D source,
                        float stepX, float stepY, float scale) const
{
    HdrTarget::Scope scope(destination);
    ClearBackground(BLANK);
    BeginBlendMode(BLEND_ADD_COLORS);
    setBlurUniforms(stepX, stepY, scale);
    blit(source, destination.width(), destination.height());
    EndBlendMode();
}

void GlowPass::render(const RibbonRenderer& ribbons,
                      const RibbonPassSettings& settings,
                      Texture2D sceneDepth)
{
    if (!available_) return;

    const float sourceWidth  = source_.width();
    const float sourceHeight = source_.height();
    const float blurWidth    = pingA_.width();
    const float blurHeight   = pingA_.height();

    /* 1. the ribbon again, overbright, on black, at FULL resolution.
     *    BLEND_ADDITIVE premultiplies by the material's alpha, which is what
     *    we want the halo to be made of, and it means a band doubling back on
     *    itself reads hotter at the crossing — exactly what a real bloom would
     *    do with the same geometry. */
    {
        HdrTarget::Scope scope(source_);
        ClearBackground(BLANK);
        BeginBlendMode(BLEND_ADDITIVE);
        BeginMode3D(settings.camera);
        ribbons.submit(settings, tuning_.emissive, sourceWidth, sourceHeight, sceneDepth);
        EndMode3D();
        EndBlendMode();
    }

    /* 1b. box-filter 2:1 down into the blur chain. At exactly half size each
     *     destination pixel centre maps to the corner shared by its four
     *     source texels, so bilinear weights them 1/4 each and the plain
     *     stretched blit IS the 2x2 average — no downsample shader needed, and
     *     no rounding to one of the four the way a nearest fetch would. */
    BeginShaderMode(blurShader_);
    setBlurUniforms(0.0f, 0.0f, 1.0f);
    {
        HdrTarget::Scope scope(pingA_);
        ClearBackground(BLANK);
        BeginBlendMode(BLEND_ADD_COLORS);
        blit(source_.texture(), blurWidth, blurHeight);
        EndBlendMode();
    }
    EndShaderMode();

    /* 2. widening blur, summed. A single wide Gaussian smears the core away
     *    and leaves a flat smudge; a single tight one is a rim light, not a
     *    glow. Doubling the tap spacing each iteration and adding every
     *    iteration into the sum gives both at once — hot centre, long soft
     *    skirt — for six half-res blits. Each octave is weighted down as it
     *    widens so the sum keeps falling off rather than turning into fog. */
    BeginShaderMode(blurShader_);
    {
        HdrTarget::Scope scope(sum_);
        ClearBackground(BLANK);
    }

    float weight = 1.0f;
    for (int i = 0; i < tuning_.glowSteps; i++) {
        const float step = static_cast<float>(1 << i);
        blurPass(pingB_, pingA_.texture(), step / blurWidth, 0.0f, 1.0f);
        blurPass(pingA_, pingB_.texture(), 0.0f, step / blurHeight, 1.0f);

        {
            HdrTarget::Scope scope(sum_);
            BeginBlendMode(BLEND_ADD_COLORS);
            /* zero spacing: the shader collapses to a straight scaled copy */
            setBlurUniforms(0.0f, 0.0f, weight);
            blit(pingA_.texture(), blurWidth, blurHeight);
            EndBlendMode();
        }
        weight *= tuning_.glowFalloff;
    }
    EndShaderMode();

    /* 3. add the halo back over the frame. BLEND_ADD_COLORS, not
     *    BLEND_ADDITIVE: the sum is already premultiplied, and its alpha is
     *    meaningless. */
    BeginBlendMode(BLEND_ADD_COLORS);
    BeginShaderMode(blurShader_);
    setBlurUniforms(0.0f, 0.0f, tuning_.glowGain);
    blit(sum_.texture(),
         static_cast<float>(GetScreenWidth()),
         static_cast<float>(GetScreenHeight()));
    EndShaderMode();
    EndBlendMode();
}

}  // namespace xcom
