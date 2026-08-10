#version 330
/* tonemap.fs.glsl - linear HDR radiance in, display pixels out.
 *
 * THE ONLY STAGE THAT KNOWS WHAT A SCREEN IS. Everything upstream works in
 * unbounded linear radiance where the sun is genuinely forty times brighter
 * than a lit wall; this is where that range is compressed into the zero-to-one
 * a monitor can show, and where the sRGB transfer curve is applied. Doing it
 * anywhere else - or, as the unlit renderer did, not at all - is what makes
 * bright surfaces flatten into paper and shadowed ones crush to black.
 *
 * The curve itself is in common/filmic.glsl, shared with the model preview's
 * resolve - the two differ only in what they do with alpha, and a tone curve
 * that drifts between them would mean a UI thumbnail that does not match the
 * object it depicts.
 *
 * IT ALSO RESOLVES THE SUPERSAMPLING. The scene target is drawn at twice the
 * screen dimensions, so the bilinear tap this blit does lands exactly at the
 * centre of each 2x2 source block and averages all four - a box downsample for
 * free, and the antialiasing that a float FBO otherwise gives up next to the
 * backbuffer's MSAA.
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
    vec3 radiance = texture(texture0, fragTexCoord).rgb;

    /* Opaque: this is the backbuffer, and the scene's alpha plane carries
     * coverage for the probe's benefit rather than anything the window wants. */
    finalColor = vec4(filmicDisplay(radiance, uExposure), 1.0);
}
