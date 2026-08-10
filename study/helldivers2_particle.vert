#version 430 core
//
// Reference billboard particle vertex shader, reconstructed from Helldivers 2's
// own bytecode. See study/helldivers2_vfx.md for the derivation and for which
// shader each block came from.
//
// This is the readable equivalent of the decompiled original, not a copy of it:
// same maths, named variables, dead register shuffling removed. Items 2-4 and 6
// of the replication checklist are here; the fragment shader carries 1, 3 and 5.

// ---- per-vertex (quad corner) -------------------------------------------
layout(location = 0) in vec2  in_corner;      // (-1,-1)..(1,1)

// ---- per-instance (one particle) ----------------------------------------
layout(location = 1) in vec3  in_position;    // world space
layout(location = 2) in vec4  in_color;       // BGRA, sRGB-ish, w = alpha
layout(location = 3) in vec3  in_tangent;     // billboard right  \ from the CPU:
layout(location = 4) in vec3  in_binormal;    // billboard up     / camera-facing,
                                              //   velocity-aligned or fixed-axis
layout(location = 5) in vec2  in_size;        // world units
layout(location = 6) in float in_rotation;    // radians, about the view axis
layout(location = 7) in float in_frame;       // flipbook frame index

layout(std140, binding = 0) uniform Viewport {
    mat4  camera_view_projection;
    mat4  camera_inv_view;
    vec3  camera_pos;
    float frame_number;
    vec2  render_resolution;
    float upscaling_method;               // 0 disables jitter
    float debug_rendering;                // != 0 disables jitter
};

layout(std140, binding = 1) uniform Billboard {
    vec2  pivot_point;                    // (0.5,0.5) = centred
    vec2  global_size_mult;
    vec2  angle_fade_range;               // e.g. (0.7, 1.0)
    vec2  Frames;                         // flipbook grid, e.g. (4,4)
    float DistScale;                      // e.g. 100
    float MaxScale;                       // e.g. 18
};

out vec4 v_color;        // rgb tint (linear), w particle alpha
out vec2 v_uv;           // flipbook cell UV
out vec3 v_world;
out float v_view_depth;  // linear distance along view, for soft particles + fog
out float v_angle_fade;

// R2 low-discrepancy sequence: 1/phi2, 1/phi2^2 where phi2 = 1.324717957
// (the plastic number). Better prefix distribution than Halton.
const vec2 R2 = vec2(0.754878, 0.569840);

void main()
{
    // ---- 2. distance-compensated size -----------------------------------
    // Particles GROW with distance so they stay legible far away, clamped so
    // they never shrink below authored size.
    vec3  to_cam = camera_pos - in_position;
    float dist   = length(to_cam);
    vec3  view_dir = to_cam / max(dist, 1e-6);

    float scale = clamp(dist / DistScale, 1.0, MaxScale);
    vec2  size  = scale * in_size * global_size_mult;

    // ---- 3. pivot, rotation, billboard basis ----------------------------
    // pivot_point is in [0,1] over the quad; 0.5,0.5 is centred.
    vec2 ext = size * (in_corner * 0.5 + pivot_point - 0.5);

    float s = sin(in_rotation);
    float c = cos(in_rotation);
    vec3 right = in_tangent * c - in_binormal * s;
    vec3 up    = in_tangent * s + in_binormal * c;

    vec3 world = in_position + right * ext.x + up * ext.y;
    v_world = world;

    vec4 clip = camera_view_projection * vec4(world, 1.0);

    // ---- 6. TAA jitter, applied after the perspective divide -------------
    // Scaling back by w leaves the depth value untouched.
    vec2 jitter = fract(frame_number * R2 + 0.5) * 2.0 - 1.0;
    jitter *= clamp(upscaling_method, 0.0, 1.0);
    jitter  = (debug_rendering != 0.0) ? vec2(0.0) : jitter;
    jitter /= render_resolution;

    vec4 ndc = clip / clip.w;
    ndc.xy += jitter;
    gl_Position = ndc * clip.w;

    // ---- 4. angle fade: smoothstep on how edge-on the card is ------------
    // This is what stops flat cards reading as paper when the camera swings.
    vec3  normal = normalize(cross(right, up));
    float d = abs(dot(view_dir, normal));
    float t = clamp((d - angle_fade_range.x) /
                    max(angle_fade_range.y - angle_fade_range.x, 1e-6), 0.0, 1.0);
    v_angle_fade = t * t * (3.0 - 2.0 * t);

    // ---- 5. colour decode ------------------------------------------------
    // .zyx un-swizzles BGRA; the square is a gamma-2.0 sRGB->linear approximation.
    v_color = vec4(in_color.zyx * in_color.zyx, in_color.w);

    // ---- flipbook cell ---------------------------------------------------
    float frame = mod(floor(in_frame), Frames.x * Frames.y);
    vec2  cell  = vec2(mod(frame, Frames.x), floor(frame / Frames.x));
    v_uv = (in_corner * 0.5 + 0.5 + cell) / Frames;

    // Linear view depth, matching what the linear-depth pyramid stores.
    v_view_depth = dot(world - camera_pos, -camera_inv_view[2].xyz);
}
