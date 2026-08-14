/* rhi/material_block.glsl — one surface's material, as every shader sees it.
 *
 * THE WHOLE DESCRIPTION, EVEN WHERE A SHADER USES PART OF IT. The depth prepass
 * reads roughness and nothing else; the opaque shader reads the first two
 * groups; the transparent one reads all five. All three declare the same block
 * because they all bind the SAME BUFFER — DeviceMaterials writes eighty bytes
 * per surface kind — and a shader that declared a shorter block would be
 * reading a prefix of it, which happens to work until someone reorders a field.
 *
 * ONE BLOCK FOR OPAQUE AND TRANSPARENT, and that is the design rather than an
 * economy. A translucent surface is not a different KIND of material: it is the
 * same base colour, roughness, metallic and normal, plus opacity and a
 * transmittance colour. Marks on a pane — dirt, frost, fingerprints — are a
 * TEXTURE feeding those same inputs, not a layer of their own, which is why
 * there is nothing here for them and nothing needed.
 *
 * The C++ half is MaterialBlockData in DeviceMaterials.cpp.
 */
#ifndef XCOM_RHI_MATERIAL_BLOCK
#define XCOM_RHI_MATERIAL_BLOCK

layout(std140, binding = 2) uniform MaterialBlock {
    vec4 uMaterialFactors;   /* roughness, metalness, normal strength, uv scale */
    vec4 uMaterialOptions;   /* channel packing, alpha mode, alpha cutoff, -    */

    /* OPACITY, and the Fresnel ramp that varies it with view angle: base,
     * falloff exponent, ceiling, and how much of the ramp to apply. */
    vec4 uOpacity;

    /* The colour picked up at grazing angles, and the thickness offset applied
     * to N.V before the ramp. */
    vec4 uTint;

    /* Light passed through from the far side: transmittance colour, amount. */
    vec4 uTransmission;

    /* What the SUN becomes crossing this surface, per channel — read in the
     * shadow pass, not the lit one. See rhi/transmission.fs.glsl. */
    vec4 uSunTransmittance;
};

#endif
