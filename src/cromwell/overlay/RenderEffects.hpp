/* RenderEffects.hpp — one switch per LIGHTING TERM, as opposed to per pass.
 *
 * SINGLE RESPONSIBILITY: carry one bool per contribution to a surface's final
 * colour. Nothing here decides anything; PbrShader packs it into a mask and the
 * surface shader multiplies terms out.
 *
 * SEPARATE FROM ViewLayers, and the distinction is the whole point. A layer
 * answers "was this geometry submitted" — props, units, the ribbon. These
 * answer "did this term contribute to the pixel", which is a different
 * question and the one that actually gets asked when something looks wrong.
 * Turning props off tells you whether the thing you are staring at is a prop;
 * turning the ambient specular off tells you whether it is a reflection. No
 * amount of the former substitutes for the latter.
 *
 * WHY IT EXISTS. Finding that an artefact on a wall was not the reflection
 * probes took several rounds of rebuilding with terms commented out by hand,
 * and one wrong answer from a toggle that only gated a capture pass rather than
 * the sampling. Every term below is here so that question costs a click.
 *
 * A TOGGLE MUST REMOVE THE TERM, NOT FREEZE IT. That is the trap the probe
 * switch fell into: it stopped refreshing the cubemaps and left them bound, so
 * "off" still shaded with them and answered "not the reflections" when they
 * were still in the picture. Anything added here has to reach the shader.
 */
#pragma once

namespace cromwell {

struct RenderEffects {
    /* ---- direct light ------------------------------------------------- */
    bool directSun = true;    /* the sun's diffuse + specular response     */

    /* ---- ambient ------------------------------------------------------ */
    bool ambientDiffuse  = true;   /* sky irradiance on the albedo         */
    bool ambientSpecular = true;   /* sky + probe along the reflection ray */

    /* ---- modulators ---------------------------------------------------
     * Not light of their own; they scale what is above. Split out because a
     * modulator misbehaving looks exactly like the term it modulates being
     * wrong, and the only way to tell them apart is to remove one. */
    bool bakedOcclusion = true;    /* the mrao map's blue channel          */
    bool transmission   = true;    /* light through a surface from behind  */

    /* THE BIT VALUES THE SHADER READS. Kept beside the fields rather than in
     * the shader so that adding a term is one edit in one place; the shader
     * only ever tests bits. */
    enum Bit : int {
        kDirectSun       = 1 << 0,
        kAmbientDiffuse  = 1 << 1,
        kAmbientSpecular = 1 << 2,
        kBakedOcclusion  = 1 << 3,
        kTransmission    = 1 << 4,
    };

    /* A SUPPRESSION mask: a bit set means that term is switched OFF. Inverted
     * from the obvious direction on purpose — an int uniform that never
     * arrives reads as zero in GLSL, and zero has to mean "the normal image".
     * With enable bits, any failure to push this blacks out the entire scene,
     * which is a debug switch breaking the render it exists to explain. It did
     * exactly that once; hence the polarity and hence this note. */
    int suppressMask() const
    {
        return (directSun       ? 0 : kDirectSun)
             | (ambientDiffuse  ? 0 : kAmbientDiffuse)
             | (ambientSpecular ? 0 : kAmbientSpecular)
             | (bakedOcclusion  ? 0 : kBakedOcclusion)
             | (transmission    ? 0 : kTransmission);
    }

    bool allOn() const { return suppressMask() == 0; }
};

}  // namespace cromwell
