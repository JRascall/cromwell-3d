#version 330
/* depth_only.fs.glsl - the colour half of a pass that has no colour.
 *
 * The framebuffer has a colour attachment only because OpenGL calls an FBO
 * with none incomplete unless the draw buffer is set to NONE, which rlgl
 * cannot express (see ShadowMap.hpp). Depth is written by the fixed-function
 * stage either way; this exists so the plane is not left undefined.
 */
out vec4 finalColor;

void main()
{
    finalColor = vec4(1.0);
}
