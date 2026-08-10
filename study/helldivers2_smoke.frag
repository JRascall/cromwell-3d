#version 430 core
//
// Helldivers 2 "family 1" smoke billboard - the workhorse.
//
// 113 materials, 14% of all particle->material references. This is the shader
// worth replicating: their smoke is a lit volume approximation, not a sprite.
//
// PROVENANCE. Read study/helldivers2_vfx.md for the full derivation. Marked per
// block below, because the honesty matters if you are going to build on this:
//
//   [BYTECODE]  transcribed from decompiled GLSL, exact
//   [DERIVED]   algebraically simplified from bytecode, same result
//   [RECONSTRUCTED] parameter exists in family 1's cbuffer and its role is clear
//                   from the name and from how sibling families use it, but the
//                   exact expression was not in a dumped permutation
//
// The prepass permutations dumped for family 1 materials cover alpha, fade and
// flipbook exactly. The colour/SSS pass was read from a sibling material
// (0x0347831766f06cd1), which uses the same wrapped-diffuse translucency but a
// slightly different parameter spelling. So: lighting model derived, exact
// smoke tint blend reconstructed.

in vec4  v_color;          // rgb tint (linear), w particle alpha
in vec2  v_cell_uv;        // [0,1] within the flipbook cell
in vec3  v_world;
in vec3  v_normal;         // billboard normal
in float v_view_depth;     // linear view depth
in float v_life;           // age / lifetime, [0,1]
in float v_fade;           // per-particle depth fade scalar

uniform sampler2D tex_texture_rgba_map;   // flipbook: density in ALPHA
uniform sampler2D tex_linear_depth;       // full-res linear depth
uniform sampler2D tex_terrain_albedo;     // screen-space ground colour

layout(std140, binding = 1) uniform Billboard {
    vec3  smoke_color;             vec3  smoke_color_secondary;
    vec3  sss_color;               vec3  weather_effect_color;
    vec2  rows_and_columns;
    float use_flipbook_blending;   float lifetime_exponent;
    float alpha_exp;               float alpha_mult;
    float angle_fade_exp;
    float camera_fade_distance;    float dist_fade_offset;
    float depth_distance_fade;
    float use_two_colors;          float use_particle_color;
    float color_mult_down;
    float sss_intensity;           float sss_intensity_start;
    float sss_wrap;                float sss_diffusion;
    float sss_gradient;            float sss_over_life;
    float sss_smoke_color;
    float luminocity_min;          float luminocity_max;
    float luminocity_curve;
    float emissiveness_base;       float ao_lowest_value;
    float sample_terrain_albedo;   float terrain_color_lerp;
    float weather_effect_color_lerp;
    float normal_strength;         float rnd_nrm_strength;
};

uniform vec3  u_camera_pos;
uniform vec3  u_light_dir;         // toward the light
uniform vec3  u_light_color;
uniform vec2  u_render_resolution;

layout(location = 0) out vec4 out_color;

vec2 cell_uv(uint index, vec2 uv)
{
    uint cols = uint(rows_and_columns.x);
    vec2 cell = vec2(float(index % cols), float(index / cols));
    return (uv + cell) / rows_and_columns;
}

void main()
{
    // ---- flipbook, lifetime-driven, cross-blended ------------ [BYTECODE] --
    // The frame index rides pow(life, lifetime_exponent) so the animation can
    // ease in or out; consecutive frames are mixed so low framecounts do not
    // strobe. Density lives in ALPHA here, not red.
    float life  = pow(clamp(v_life, 0.0, 1.0), lifetime_exponent);
    float total = rows_and_columns.x * rows_and_columns.y;
    float fpos  = total * life;
    uint  i0    = uint(fpos);

    float density = texture(tex_texture_rgba_map, cell_uv(i0, v_cell_uv)).a;
    if (use_flipbook_blending > 0.5) {
        uint  i1 = min(uint(total) - 1u, i0 + 1u);
        float d1 = texture(tex_texture_rgba_map, cell_uv(i1, v_cell_uv)).a;
        density  = mix(density, d1, fract(fpos));
    }

    // ---- alpha shaping --------------------------------------- [BYTECODE] --
    float alpha = clamp(pow(density, alpha_exp) * alpha_mult, 0.0, 1.0) * v_color.w;

    // ---- soft particles, full-res, per-particle range --------- [BYTECODE] --
    // Family 1 uses full-resolution linear depth and a PER-PARTICLE fade range,
    // unlike the simpler family which samples a mip-6 pyramid at a constant.
    vec2  suv     = gl_FragCoord.xy / u_render_resolution;
    float scene_z = texture(tex_linear_depth, suv).x;
    float range   = max(v_fade * depth_distance_fade, 1e-4);
    alpha *= clamp((scene_z - v_view_depth) / range, 0.0, 1.0);

    // ---- angle fade: smoothstep THEN exponent ---------------- [BYTECODE] --
    vec3  to_cam   = u_camera_pos - v_world;
    float dist     = length(to_cam);
    vec3  view_dir = to_cam / max(dist, 1e-6);

    float d = clamp(dot(v_normal, view_dir), 0.0, 1.0);
    alpha *= pow(d * d * (3.0 - 2.0 * d), angle_fade_exp);

    // ---- camera proximity fade -------------------------------- [BYTECODE] --
    // Squared, so walking into a plume does not fill the screen.
    float p = clamp((dist - dist_fade_offset) / max(camera_fade_distance, 0.001),
                    0.0, 1.0);
    alpha *= p * p;

    if (alpha < 0.001) discard;

    // ---- base colour: two-tone over density ------------- [RECONSTRUCTED] --
    // use_two_colors blends a primary and secondary smoke tint. Indexing by
    // density (rather than age) is the same trick the simple family plays with
    // its 128x1 LUT: dense core reads one colour, thin edges the other.
    vec3 base = (use_two_colors > 0.5)
              ? mix(smoke_color_secondary, smoke_color, density)
              : smoke_color;
    if (use_particle_color > 0.5) base *= v_color.rgb;
    base *= mix(1.0, color_mult_down, 1.0 - density);

    // ---- subsurface scattering ------------------------ [DERIVED/BYTECODE] --
    // Wrapped diffuse. At sss_wrap = 0 this is plain Lambert; at 1 it is pure
    // back-lighting, so the plume glows where the sun is behind it. That single
    // mix is what makes their smoke read as a volume rather than a decal.
    float ndl     = dot(v_normal, u_light_dir);
    float wrapped = mix(ndl, 1.0 - ndl, clamp(sss_wrap, 0.0, 1.0));
    wrapped = max(wrapped, 0.0);

    // SSS strength ramps over the particle's life and is masked by density:
    // thin wisps transmit, dense cores do not.
    float sss_amount = mix(sss_intensity_start, sss_intensity,
                           clamp(v_life * sss_over_life, 0.0, 1.0));
    float thin = pow(1.0 - density, max(sss_gradient, 1e-4));   // [RECONSTRUCTED]
    float sss  = clamp(sss_amount * thin, 0.0, 1.0);

    vec3 sss_tint = mix(base, sss_color, sss_smoke_color);
    vec3 scatter  = sss_tint * u_light_color * wrapped * sss * sss_diffusion;

    // ---- direct term, ambient occlusion floor ---------- [RECONSTRUCTED] --
    float ao = mix(ao_lowest_value, 1.0, density);
    vec3  lit = base * u_light_color * max(ndl, 0.0) * ao;

    // ---- luminosity remap ------------------------------ [RECONSTRUCTED] --
    // Compresses the plume into a controlled brightness band so explosions do
    // not blow out and ambient smoke does not vanish.
    vec3  col = lit + scatter;
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float t   = pow(clamp(lum, 0.0, 1.0), max(luminocity_curve, 1e-4));
    col *= mix(luminocity_min, luminocity_max, t) / max(lum, 1e-4);
    col += base * emissiveness_base;

    // ---- environment integration ----------------------- [RECONSTRUCTED] --
    // Smoke samples the ground it sits on and tints toward the weather state.
    // This is the bit most home-grown particle systems skip, and it is a large
    // part of why HD2's smoke sits IN the scene instead of on top of it.
    if (sample_terrain_albedo > 0.5) {
        vec3 ground = texture(tex_terrain_albedo, suv).rgb;
        col = mix(col, col * ground, terrain_color_lerp);
    }
    col = mix(col, weather_effect_color, weather_effect_color_lerp);

    out_color = vec4(col * alpha, alpha);      // premultiplied
}
