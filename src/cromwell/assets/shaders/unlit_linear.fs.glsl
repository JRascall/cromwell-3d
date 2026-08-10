#version 330
/* unlit_linear.fs.glsl - authored colours, converted into the linear pass.
 *
 * The gameplay overlays - the LOS tint, the cover shields, the hover plate,
 * the path line, the blast flash - are UI that happens to live in the world.
 * They are not lit, and they should not be: a cover shield means the same
 * thing at noon and at dusk.
 *
 * But they are drawn INTO the linear HDR scene target, so that they still
 * depth-test against the world. Their palette entries are authored in sRGB, so
 * without this they would be interpreted as linear, come out far too bright,
 * and then get tonemapped on top of that. This is the whole shader: raylib's
 * standard batch inputs, decoded to linear, nothing else.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

void main()
{
    vec4 authored = texture(texture0, fragTexCoord) * colDiffuse * fragColor;

    /* Alpha is a blend weight, not a colour - it stays where it is. */
    finalColor = vec4(pow(authored.rgb, vec3(2.2)), authored.a);
}
