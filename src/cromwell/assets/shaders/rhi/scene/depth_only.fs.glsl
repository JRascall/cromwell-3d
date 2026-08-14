#version 450 core
/* depth_only.fs.glsl - the colour half of a pass that has no colour.
 *
 * KEPT, EVEN THOUGH THE DEVICE NO LONGER NEEDS IT TO BE.
 *
 * The raylib version of this shader exists because rlgl cannot set the draw
 * buffer to NONE, so its depth-only FBO must carry a colour attachment to be
 * complete, and that attachment must be written. IRenderDevice has no such
 * limitation - a PassDesc with colourCount == 0 makes the backend call
 * glDrawBuffer(GL_NONE), which is a real depth-only pass.
 *
 * IT WRITES ONE CHANNEL AGAIN NOW, and for a better reason than GL's.
 *
 * The shadow pass carries a TRANSMISSION PLANE: how much sunlight survived the
 * journey to each texel, so a window can cast a coloured patch rather than a
 * hole. An opaque caster's contribution to that plane is "nothing was tinted
 * on the way here" — which is 1, the same value the pass clears to.
 *
 * Writing it explicitly rather than relying on the clear is what keeps the
 * plane correct when an opaque caster stands in FRONT of a pane: the glass
 * would otherwise have already written its own transmittance there, and the
 * wall in front of it must overwrite that. Light stopped by a wall never
 * reached the window to be tinted.
 *
 * See assets/shaders/CONVENTIONS.md.
 */

layout(location = 0) out vec4 outTransmission;

void main()
{
    outTransmission = vec4(1.0, 0.0, 0.0, 1.0);
}
