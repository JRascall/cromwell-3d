/* SplashPass.hpp — the animated splash screen.
 *
 * SINGLE RESPONSIBILITY: get one still image onto the whole window, under the
 * shader that animates it, and let that shader be reloaded while looking at it.
 *
 * WHY IT IS ITS OWN CLASS. FrameRenderer draws everything, and "everything" is
 * how a 1,300-line file happens. The splash shares nothing with the scene — no
 * camera, no lighting, no render targets, one texture and one shader — so it is
 * separable at no cost.
 *
 * IT OWNS NO TUNING, AND THAT IS DELIBERATE. Every number that decides how the
 * splash LOOKS — where the water starts, where the sun is, how the river lies,
 * how strong each effect is — is a constant at the top of
 * assets/shaders/splash.fs.glsl. This class passes only what the shader cannot
 * know for itself: the clock, the ramp, and the texture's aspect.
 *
 * The reason is reload(). With the look living entirely in a text file the
 * splash can re-read, tuning it is edit-and-look; with half of it in constants
 * here, every change to the half that mattered would be a rebuild and a
 * relaunch. Anything moved back into this file quietly takes that away, so
 * don't — add a constant to the shader instead.
 *
 * A MISSING IMAGE IS NORMAL, NOT AN ERROR. Everything under assets/ is
 * gitignored (see the note in .gitignore about binaries in git history), so a
 * fresh checkout genuinely has no splash image, and so does anyone who has not
 * been handed the content drop. available() reports that plainly and the caller
 * falls back to the text splash. The shader is treated the same way one step
 * down: no shader means the image is blitted unmoved, which is a worse splash
 * rather than no splash.
 */
#pragma once

#include "raylib.h"

namespace game {

class SplashPass {
public:
    ~SplashPass();

    /* Fills the current window with the image, cover-fitted, animated to
     * `seconds` since the splash appeared. Loads on the first call and does
     * nothing without an image. */
    void draw(float seconds);

    /* Re-reads the shader AND the water map from disk, keeping the painting.
     * For tuning: the whole look is in those two files, so this is the entire
     * edit loop — paint or edit, save, press F5, look. Silently keeps the
     * shader it already has if the new one will not compile: a typo mid-edit
     * should cost the frame it was typed in, not the session. */
    void reload();

    /* Whether there is an image to draw. The caller needs this to decide
     * whether to draw the text splash instead. */
    bool available() const { return image_.id != 0; }

private:
    /* Both are found through ShaderLibrary's search path, so they resolve from
     * the project root and from builds/win alike, and a game-side file wins
     * over an engine-side one of the same name. */
    static constexpr const char* kImagePath  = "textures/cromwell.png";
    static constexpr const char* kShaderName = "splash.fs.glsl";

    /* The water and sky maps are not files — they are packed from painted
     * masks by SplashMaps, which owns their names. Without them the shader
     * guesses: a straight tilted line for the river, which cannot follow one
     * round a bridge pier or a moored barge, and a brightness threshold for the
     * sky, which on this painting cannot tell a hazy tower from the air behind
     * it. */

    /* Seconds for the effects to reach full strength. The image has to appear
     * as it was painted and then come to life: opening mid-shimmer looks like a
     * video that was already running before the window was. Short, because an
     * ordinary splash is only Application::kSplashMinimumSeconds long — though under
     * --splash it stays up indefinitely, which is what makes the ramp worth
     * having a name rather than a literal. */
    static constexpr float kRampSeconds = 0.5f;

    Texture2D image_{};
    Texture2D waterMap_{};
    Texture2D skyMap_{};
    Shader    shader_{};
    bool      tried_ = false;


    /* Uniform locations, resolved on every (re)load. -1 is raylib's "not
     * present", which SetShaderValue ignores — so a shader edited to drop one
     * degrades rather than crashes. */
    int locTime_ = -1;
    int locRamp_ = -1;
    int locAspect_ = -1;
    int locWaterMap_ = -1;
    int locHasWaterMap_ = -1;
    int locSkyMap_ = -1;
    int locHasSkyMap_ = -1;
    int locUvPerPixel_ = -1;

    /* Loads image and shader if they have not been tried. Deferred rather than
     * done at startup because the splash is the only consumer, a scripted run
     * usually never shows one, and startup is precisely the time the splash
     * exists to cover. */
    void ensureLoaded();

    /* Compiles the shader and resolves its uniforms. Returns false and leaves
     * shader_ untouched if it did not compile. */
    bool loadShader();

    /* (Re-)builds the optional water and sky maps. Absence of either is
     * normal and leaves the shader on its fallback for that feature. */
    void loadWaterMap();

    static void applyMapFiltering(Texture2D& map);

    void setUniforms(float seconds, Vector2 uvPerPixel) const;
};

}  // namespace game
