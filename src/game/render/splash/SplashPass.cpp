#include "game/render/splash/SplashPass.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "game/render/splash/SplashMaps.hpp"

namespace game {

using namespace cromwell;

SplashPass::~SplashPass()
{
    if (shader_.id)   UnloadShader(shader_);
    if (skyMap_.id)   UnloadTexture(skyMap_);
    if (waterMap_.id) UnloadTexture(waterMap_);
    if (image_.id)    UnloadTexture(image_);
}

bool SplashPass::loadShader()
{
    const Shader loaded = ShaderLibrary::load(nullptr, kShaderName);
    if (loaded.id == 0) return false;

    /* Only now is the old one dropped. Unloading first and then failing to
     * compile would leave the splash unanimated for the rest of the run
     * because of one mistyped character — which is the opposite of what a
     * reload key is for. */
    if (shader_.id != 0) UnloadShader(shader_);
    shader_ = loaded;

    locTime_        = GetShaderLocation(shader_, "uTime");
    locRamp_        = GetShaderLocation(shader_, "uRamp");
    locAspect_      = GetShaderLocation(shader_, "uAspect");
    locWaterMap_    = GetShaderLocation(shader_, "uWaterMap");
    locHasWaterMap_ = GetShaderLocation(shader_, "uHasWaterMap");
    locSkyMap_      = GetShaderLocation(shader_, "uSkyMap");
    locHasSkyMap_   = GetShaderLocation(shader_, "uHasSkyMap");
    locUvPerPixel_  = GetShaderLocation(shader_, "uUvPerPixel");
    return true;
}

void SplashPass::loadWaterMap()
{
    if (waterMap_.id != 0) { UnloadTexture(waterMap_); waterMap_ = Texture2D{}; }
    if (skyMap_.id   != 0) { UnloadTexture(skyMap_);   skyMap_   = Texture2D{}; }

    SplashMaps::Result built = SplashMaps::build();

    if (SplashMaps::valid(built.water)) {
        waterMap_ = LoadTextureFromImage(built.water);   /* copies; does not adopt */
        UnloadImage(built.water);
    } else {
        LOGGER->info("SPLASH: no water mask - falling back to the tilted-line formula");
    }

    if (SplashMaps::valid(built.sky)) {
        skyMap_ = LoadTextureFromImage(built.sky);
        UnloadImage(built.sky);
    } else {
        LOGGER->info("SPLASH: no sky mask - the clouds will guess where the sky is");
    }

    applyMapFiltering(skyMap_);

    applyMapFiltering(waterMap_);
}

/* BILINEAR AND NO MIPMAPS for both maps. This is data, not picture: a depth
 * ramp is read as a distance and its smoothness is what keeps the wave crests
 * smooth, so filtering between texels is wanted — but a mip chain is not.
 * Nothing here is minified enough to need one, and a coarse level would blur a
 * mask edge back over the barges and spires those masks exist to protect.
 *
 * CLAMP because the ripple reads slightly outside the frame at the bottom edge,
 * and the sane answer there is "the same water", not the far side of the
 * picture. */
void SplashPass::applyMapFiltering(Texture2D& map)
{
    if (map.id == 0) return;
    SetTextureFilter(map, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(map, TEXTURE_WRAP_CLAMP);
}

void SplashPass::reload()
{
    /* Nothing to reload onto, and nothing to say about it: without an image
     * the splash is a line of text and there is no shader in play. */
    if (image_.id == 0) return;

    if (loadShader()) LOGGER->info("SPLASH: {} reloaded", kShaderName);
    else              LOGGER->warn("SPLASH: {} failed to compile - keeping the previous one",
                                   kShaderName);

    /* THE MAP RELOADS TOO, and it is arguably the more important half: painting
     * a water mask is a loop of paint, save, look, and having to restart the
     * game between passes is how a mask ends up half finished. A map that
     * appears for the first time between reloads is picked up as well. */
    loadWaterMap();
}

void SplashPass::ensureLoaded()
{
    if (tried_) return;
    tried_ = true;

    const char* root = ShaderLibrary::rootContaining(kImagePath);
    if (root == nullptr) {
        /* Info, not a warning. There is no image in a fresh checkout by design,
         * and a frightening line in the log for the expected case is how people
         * learn to stop reading the log.
         *
         * Through LOGGER rather than TraceLog, and that distinction is easy to
         * get wrong: raylib's LOG_INFO is mapped to this project's DEBUG level
         * — see RaylibLogBridge, which makes that trade deliberately because
         * raylib narrates every texture id it creates — so a TraceLog info line
         * does not appear at the default level at all. This one has to. */
        LOGGER->info("SPLASH: no {} found - falling back to the text splash", kImagePath);
        return;
    }

    image_ = LoadTexture(TextFormat("%s/%s", root, kImagePath));
    if (image_.id == 0) {
        LOGGER->warn("SPLASH: {}/{} could not be decoded", root, kImagePath);
        return;
    }

    /* The image is 1920x1080 and the window opens at 1280x720, so the common
     * case is MINIFICATION — which without a mip chain is a point-sampled
     * undersample of a detailed painting and shimmers on every resize. Order
     * matters: TRILINEAR no-ops on a texture with one level, so it has to come
     * after the chain exists.
     *
     * THE CHAIN IS ALSO LOAD-BEARING FOR THE SHADER, not just for filtering:
     * the light shafts deliberately sample a coarse mip so that a radial blur
     * of a sharp painting does not stack up as ghost copies of Big Ben, and the
     * fog takes its colour from a very coarse one. Without mipmaps both fall
     * back to level 0 and both look wrong.
     *
     * CLAMP for the same reason: the shafts march their taps towards the sun
     * and the ripple displaces its lookup, so both read outside 0..1 near the
     * edges. Under REPEAT that wraps the far side of the painting into frame —
     * sky appearing at the bottom of the river. */
    GenTextureMipmaps(&image_);
    SetTextureFilter(image_, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(image_, TEXTURE_WRAP_CLAMP);

    loadWaterMap();

    if (!loadShader()) {
        /* Degrades to the still image. Worth a warning, unlike the missing
         * texture: the shader IS in git — assets/shaders is the one deliberate
         * exception in .gitignore — so its absence means something is actually
         * wrong with the checkout or the working directory. */
        LOGGER->warn("SPLASH: {} did not load - the splash will not animate", kShaderName);
    }
}

void SplashPass::setUniforms(float seconds, Vector2 uvPerPixel) const
{
    /* THE TEXTURE'S aspect, not the window's, and that is exact rather than an
     * approximation — see the uniform's comment in splash.fs.glsl. The blit is
     * a cover fit, which forces the visible source rectangle to the window's
     * aspect, and the two ratios cancel. */
    const float aspect = static_cast<float>(image_.width) /
                         static_cast<float>(image_.height);

    float ramp = seconds / kRampSeconds;
    if (ramp > 1.0f) ramp = 1.0f;
    if (ramp < 0.0f) ramp = 0.0f;

    SetShaderValue(shader_, locTime_,   &seconds, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locRamp_,   &ramp,    SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locAspect_, &aspect,  SHADER_UNIFORM_FLOAT);

    /* HOW MUCH UV ONE SCREEN PIXEL COVERS. The shader builds its water normal
     * from screen-space derivatives, which are per pixel, and without this the
     * surface would tilt differently at every window size — glassy in a large
     * window, choppy in a small one. It has to be computed from the SOURCE
     * rectangle rather than the whole image, because the cover fit crops. */
    const float perPixel[2] = { uvPerPixel.x, uvPerPixel.y };
    SetShaderValue(shader_, locUvPerPixel_, perPixel, SHADER_UNIFORM_VEC2);

    /* A flag rather than testing the sampler in GLSL, because an unbound
     * sampler2D does not read as anything defined — it reads as whatever
     * texture unit 1 last had, which during a splash is nothing in particular
     * and during a later frame could be a lightmap. */
    const float hasMap = waterMap_.id != 0 ? 1.0f : 0.0f;
    SetShaderValue(shader_, locHasWaterMap_, &hasMap, SHADER_UNIFORM_FLOAT);

    const float hasSky = skyMap_.id != 0 ? 1.0f : 0.0f;
    SetShaderValue(shader_, locHasSkyMap_, &hasSky, SHADER_UNIFORM_FLOAT);
    if (waterMap_.id != 0) SetShaderValueTexture(shader_, locWaterMap_, waterMap_);
    if (skyMap_.id   != 0) SetShaderValueTexture(shader_, locSkyMap_,   skyMap_);
}

void SplashPass::draw(float seconds)
{
    ensureLoaded();
    if (image_.id == 0) return;

    const float screenWidth  = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    if (screenWidth <= 0.0f || screenHeight <= 0.0f) return;

    const float imageWidth  = static_cast<float>(image_.width);
    const float imageHeight = static_cast<float>(image_.height);

    /* COVER, NOT FIT, and the crop is taken from the SOURCE rectangle so the
     * image fills the window at any aspect without letterboxing or stretching.
     * A splash is a backdrop: losing a strip of sky costs nothing, whereas a
     * black bar or a squashed clock tower both read as a bug.
     *
     * It also happens to be what the shader's roundness maths assumes — a
     * source rectangle with the window's aspect — so changing this to a fit
     * would put an ellipse around the sun. */
    Rectangle source{ 0.0f, 0.0f, imageWidth, imageHeight };
    const float windowAspect = screenWidth / screenHeight;
    const float imageAspect  = imageWidth / imageHeight;

    if (windowAspect > imageAspect) {
        source.height = imageWidth / windowAspect;        /* wider: trim top and bottom */
        source.y      = (imageHeight - source.height) * 0.5f;
    } else {
        source.width = imageHeight * windowAspect;        /* taller: trim the sides */
        source.x     = (imageWidth - source.width) * 0.5f;
    }

    const Rectangle destination{ 0.0f, 0.0f, screenWidth, screenHeight };

    if (shader_.id != 0) {
        /* UNIFORMS AFTER BeginShaderMode, AND THE ORDER IS LOAD-BEARING for
         * exactly one of them: the water map.
         *
         * Binding a second texture in raylib does not bind anything. It
         * registers the texture in rlgl's list of extra units, and the actual
         * glBindTexture happens when the batch is next drawn. BeginShaderMode
         * DRAWS THE BATCH before switching shader — and drawing a batch clears
         * that list. So a SetShaderValueTexture issued before it is registered,
         * immediately forgotten, and the sampler is left pointing at an unbound
         * unit, which reads as solid black.
         *
         * The symptom is worth writing down because it looks like anything but
         * a binding problem: every float uniform works, the shader compiles,
         * the fog and the glow animate perfectly — and the water is completely
         * still, because its mask sampled zero everywhere. */
        BeginShaderMode(shader_);
        setUniforms(seconds,
                    Vector2{ source.width  / imageWidth  / screenWidth,
                             source.height / imageHeight / screenHeight });
        DrawTexturePro(image_, source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
        EndShaderMode();
    } else {
        DrawTexturePro(image_, source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
    }
}

}  // namespace game
