#version 330
/* model_preview.fs.glsl - the studio target, resolved onto a UI-ready texture.
 *
 * tonemap.fs.glsl's sibling: same exposure, same curve, same sRGB encode, so a
 * model in a UI panel is shaded and graded exactly as the same model standing
 * on the board. It differs in one thing only, and that thing is alpha.
 *
 * WHY IT CANNOT JUST BE ToneMapPass. That shader writes alpha 1, because it
 * blits onto an opaque backbuffer. A preview's whole point is that there is no
 * background, so coverage has to survive the resolve.
 *
 * IT ALSO UN-PREMULTIPLIES, and this is the part that is easy to skip and
 * visible the moment you skip it. Two separate things premultiply the source:
 *
 *   - the box downsample. The target is drawn at twice the panel's size, so
 *     this blit's bilinear tap averages a 2x2 block; a block half covered by
 *     the model averages (colour, 1) with (0, 0) and lands on (colour/2, 0.5).
 *     That IS the premultiplied form - the colour has already been scaled by
 *     coverage.
 *   - the alpha blending inside the target, which composited the model over a
 *     transparent-black clear in the first place.
 *
 * Handing that to ImGui or DrawTexturePro, both of which blend straight alpha,
 * scales the colour by coverage a SECOND time and every silhouette pixel comes
 * out darker than it should - the dark fringe around a cut-out. Dividing the
 * coverage back out here means the texture that leaves this pass is ordinary
 * straight-alpha RGBA8 that any consumer can blend correctly with no special
 * mode and no knowledge of where it came from.
 *
 * Not exactly reversible, and knowingly so: the curve is non-linear, so
 * tonemapping an unpremultiplied edge is not the same as unpremultiplying a
 * tonemapped one. The difference lives in a one-pixel border and is far below
 * what the double-darkening costs.
 */
#include "common/filmic.glsl"

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform float uExposure;

void main()
{
    vec4 source = texture(texture0, fragTexCoord);

    /* Nothing covered this pixel. Return transparent rather than dividing by
     * zero - and transparent BLACK, so a consumer that ignores alpha entirely
     * gets the background it would have picked anyway. */
    if (source.a <= 0.0) {
        finalColor = vec4(0.0);
        return;
    }

    vec3 radiance = source.rgb / source.a;
    finalColor = vec4(filmicDisplay(radiance, uExposure), source.a);
}
