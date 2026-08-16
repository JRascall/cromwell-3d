#version 450 core
/* preview.fs.glsl — one of the renderer's own buffers, made visible.
 *
 * See assets/shaders/CONVENTIONS.md. Driven by DeviceTexturePreviews, whose
 * header carries the argument for why a blit is needed at all rather than
 * handing the panel the texture.
 *
 * THIS IS A DIAGNOSTIC AND IT MUST NOT FLATTER. Every branch below either
 * selects channels or applies a monotone curve, so the ordering of values in
 * the source survives into the picture: brighter here is always larger there.
 * Anything that clipped, thresholded or false-coloured would make two different
 * buffers look the same, which is the one thing a panel like this may not do.
 *
 * The covering triangle is the shared fullscreen vertex stage in
 * ScenePipeline.cpp — there is no vertex buffer and no attribute read.
 */
layout(binding = 0) uniform sampler2D uSource;

layout(std140, binding = 1) uniform PreviewBlock {
    /* x = mode, matching DeviceTexturePreviews::Mode's order.
     * y = mode parameter; unused by the 2D modes.
     * zw = the target's size in pixels, which is how gl_FragCoord becomes a UV
     *      without a second uniform or a vertex stage that carries one. */
    vec4 uPreview;

    /* x = near, y = far — the projection the depth buffer was written with.
     * z = how far the display ramp spans, in world units.
     * See DeviceTexturePreviews::setDepthRange. */
    vec4 uDepthRange;
};

layout(location = 0) out vec4 outColour;

void main()
{
    /* ---- THE ONE FLIP, AND IT IS HERE ON PURPOSE ------------------------
     *
     * A render target's first texel row is at the BOTTOM of what was drawn into
     * it, and ImGui addresses an image from the TOP left. Copy a buffer texel
     * for texel and the panel shows it upside down — which is not obviously
     * wrong on a shadow map or an occlusion plane, and is exactly the kind of
     * wrong that gets attributed to the pass that produced it.
     *
     * So the flip lives in the COPY, once, and every preview leaves here as an
     * ordinary top-down image. That is the same promise `TexturePreviews` makes
     * on the raylib path and the reason DevView's texture panel says "drawn as
     * given — do not add a flip here": the panel cannot tell which buffer an
     * entry came from and so cannot be right about it. */
    vec2 uv = vec2(gl_FragCoord.x, uPreview.w - gl_FragCoord.y)
            / max(uPreview.zw, vec2(1.0));

    int mode = int(uPreview.x + 0.5);
    vec4 texel = texture(uSource, uv);

    vec3 shown;

    if (mode == 1) {
        /* Red as grey. One-channel planes — occlusion. */
        shown = texel.rrr;
    } else if (mode == 2) {
        /* The fourth channel as grey. The G-buffer's alpha is ROUGHNESS rather
         * than coverage; ScenePipeline's accessors say so and this is the only
         * way to see it. */
        shown = texel.aaa;
    } else if (mode == 3) {
        /* ---- depth, LINEARISED --------------------------------------------
         *
         * NOT A CONTRAST CURVE, AND THE FIRST VERSION OF THIS WAS ONE — which
         * is worth recording because it looked reasonable and measured as
         * useless. A perspective depth buffer is HYPERBOLIC: with a 0.1 near
         * plane and a 1000 far plane, everything from ten tiles outward sits
         * above 0.99, so the whole visible scene lands in the top one percent
         * of the range. Raising it to a power stretches that percent out, but
         * the amount of stretch that suits the near field crushes the far and
         * vice versa — measured on this board it produced a preview whose
         * ENTIRE range was 0 to 21 out of 255. A picture that dark is not a
         * depth buffer with poor contrast, it is indistinguishable from one
         * nothing was drawn into.
         *
         * So invert the projection instead. `near * far / (far - d * (far -
         * near))` is the eye-space distance the value came from, and dividing
         * that by a span the caller chose gives a ramp in WORLD UNITS — which
         * is the only version of this that means the same thing from one frame
         * to the next, and the only one on which "each band is an equal slice
         * of distance" is a true sentence rather than a hopeful one.
         *
         * `uDepthRange.y <= 0` means the buffer is ALREADY LINEAR — an
         * orthographic shadow map is — and the value is taken as it stands.
         *
         * THE BANDS ARE WHAT MAKE A GRADIENT READABLE. A smooth grey ramp shows
         * that depth varies; equal slices show BY HOW MUCH, which is what tells
         * a shadow map focused on the camera apart from one still framed on the
         * whole world. */
        float stored = clamp(texel.r, 0.0, 1.0);

        float nearPlane = max(uDepthRange.x, 0.0001);
        float farPlane  = uDepthRange.y;
        float span = max(uDepthRange.z, nearPlane + 0.0001);

        float eyeDistance = stored;
        if (farPlane > nearPlane) {
            float denominator = farPlane - stored * (farPlane - nearPlane);
            eyeDistance = denominator > 0.0001 ? (nearPlane * farPlane) / denominator : farPlane;
            eyeDistance = clamp((eyeDistance - nearPlane) / (span - nearPlane), 0.0, 1.0);
        }

        /* NEAR IS BRIGHT. Either sense is defensible and this one matches the
         * raylib previews, which matters more than the argument: two panels
         * showing the same buffer with the ramp inverted between them is a
         * comparison nobody can make. */
        float shownDepth = 1.0 - eyeDistance;

        float bands = 16.0;
        float band = floor(shownDepth * bands) / bands;
        shown = vec3(mix(band, shownDepth, 0.35));
    } else if (mode == 4) {
        /* ---- linear HDR radiance ----------------------------------------
         *
         * RANGE-COMPRESSED, AND DELIBERATELY NOT THE FRAME'S TONE CURVE.
         *
         * Reinhard over the raw radiance with no exposure: x/(1+x) is monotone,
         * maps zero to zero and infinity to one, and needs no number anybody
         * tuned. Borrowing filmicDisplay and the frame's exposure would look
         * nicer and would make this preview CHANGE WHEN THE EXPOSURE SLIDER
         * MOVED — a picture of the buffer that moves when the buffer does not
         * is a diagnostic that cannot be trusted to answer the question it is
         * open for.
         *
         * The square root afterwards is a display transfer, not a grade: the
         * result lands in an RGBA8 target that is read back as-is, and without
         * it every mid-tone is too dark to see. */
        shown = sqrt(texel.rgb / (1.0 + max(texel.rgb, vec3(0.0))));
    } else {
        /* Colour: as stored. Encoded normals and the transmission plane are
         * already 0..1 and mean what they look like. */
        shown = texel.rgb;
    }

    /* OPAQUE ALWAYS. The panel composites this over its own window background,
     * and a preview that carried the source's alpha would show the WINDOW
     * through the buffer wherever the buffer happened to be transparent — read
     * as the buffer being empty there. */
    outColour = vec4(shown, 1.0);
}
