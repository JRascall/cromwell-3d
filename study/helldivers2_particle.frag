#version 430 core
//
// Reference billboard particle fragment shader, reconstructed from Helldivers 2's
// own bytecode. See study/helldivers2_vfx.md for the derivation.
//
// Readable equivalent of the decompiled original: same maths, named variables,
// register shuffling removed. Covers items 1, 3 and 5 of the replication
// checklist. The volumetric-fog froxel lookup is deliberately left out - it only
// makes sense against a renderer that already has such a volume - but the split
// at 200 m and the analytic height fog are described in the doc.

in vec4  v_color;        // rgb tint (linear), w particle alpha
in vec2  v_uv;           // flipbook cell UV
in vec3  v_world;
in float v_view_depth;
in float v_angle_fade;

// Greyscale DENSITY sheet - a flipbook of masks, not of colour images.
uniform sampler2D tex_base_color;
// 128x1 colour ramp, indexed by density. This is where the colour comes from.
uniform sampler2D tex_gradient_map;
// Downsampled LINEAR depth pyramid. Sampled at a small mip on purpose: soft
// particles want smoothness, not precision, and a small mip gives both that and
// a cheaper fetch.
uniform sampler2D tex_linear_depth_mip;

layout(std140, binding = 1) uniform Billboard {
    float opacity_exp;          // density shaping, e.g. 1.0
    float fade_distance;        // soft-particle range in metres, e.g. 0.1
    float emissive_intensity;   // HDR, e.g. 22 - these are bloom sources
    float use_lut;              // 1 = colour from the gradient ramp
    float particle_color;       // 1 = modulate the ramp by the per-particle tint
    float depth_mip;            // which mip of the depth pyramid, e.g. 1.0
    vec2  render_resolution;
};

layout(location = 0) out vec4 out_color;

void main()
{
    // ---- 5. density shaping ---------------------------------------------
    // Single channel: the sheet stores density, never colour.
    float density = pow(texture(tex_base_color, v_uv).r, opacity_exp);

    float alpha = density * v_color.w * v_angle_fade;

    // ---- 3. soft particles against the depth pyramid ---------------------
    vec2  suv     = gl_FragCoord.xy / render_resolution;
    float scene_z = textureLod(tex_linear_depth_mip, suv, depth_mip).x;
    float soft    = clamp((scene_z - v_view_depth) / max(fade_distance, 1e-6), 0.0, 1.0);

    alpha = clamp(alpha * soft, 0.0, 1.0);
    if (alpha < 0.001) discard;          // the prepass variant uses < 0.5 instead

    // ---- 1. colour from a LUT indexed by DENSITY SQUARED -----------------
    //
    // The single most valuable idea in this shader. The ramp is indexed by how
    // dense the sprite is at this pixel, NOT by particle age - so a dense core
    // samples the hot end and thin edges sample the cool end. One greyscale puff
    // becomes a white-hot core with an orange body and a smoky rim, and it stays
    // internally consistent as the sprite deforms.
    //
    // Squaring biases the ramp toward the thin end, widening the cool region.
    // Age-indexed ramps are a different effect entirely, not a substitute.
    vec3 ramp = texture(tex_gradient_map, vec2(density * density, 0.0)).rgb;

    vec3 rgb = (use_lut == 1.0) ? ramp * v_color.rgb
                                : density * v_color.rgb;
    rgb = (particle_color == 1.0) ? rgb : ramp;

    rgb *= emissive_intensity;           // HDR on purpose

    out_color = vec4(rgb * alpha, alpha);   // premultiplied
}
