/* Before raylib.h — GL.hpp brings glad in first, and glad must precede anything
 * that could pull a system GL header. See the ordering note in GL.hpp. */
#include "cromwell/gpu/GL.hpp"

#include "cromwell/sdf/WorldText.hpp"

#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

namespace cromwell::sdf {

WorldText::~WorldText()
{
    unload();
}

bool WorldText::load(const std::string& cwfontPath)
{
    unload();

    if (!font_.load(cwfontPath)) return false;

    /* No vertex shader of our own: raylib's default already does mvp and passes
     * the texcoord and colour through, which is the entire vertex stage this
     * needs. Everything interesting is in the fragment shader, and a bespoke
     * vertex shader would only be a second file to keep in step with rlgl's
     * attribute locations. */
    shader_ = ShaderLibrary::load(nullptr, "msdf_text.fs.glsl");
    shaderReady_ = shader_.id != 0;
    if (!shaderReady_) {
        font_.unload();
        return false;
    }

    pxRangeLoc_ = GetShaderLocation(shader_, "pxRange");
    outlineColourLoc_ = GetShaderLocation(shader_, "outlineColour");
    outlineWidthLoc_ = GetShaderLocation(shader_, "outlineWidthPx");

    /* Set once. The range is a property of the BAKE and cannot change while a
     * font is loaded, so pushing it per draw would be a uniform upload per
     * label to say the same thing. */
    const float range = font_.pxRange();
    SetShaderValue(shader_, pxRangeLoc_, &range, SHADER_UNIFORM_FLOAT);
    return true;
}

void WorldText::unload()
{
    if (shaderReady_) {
        UnloadShader(shader_);
        shader_ = Shader{};
        shaderReady_ = false;
    }
    font_.unload();
    quads_.clear();
}

bool WorldText::ready() const
{
    return shaderReady_ && font_.loaded();
}

void WorldText::setOutline(Color colour, float widthPixels)
{
    outlineColour_ = colour;
    outlineWidthPx_ = widthPixels;
}

void WorldText::draw(const Camera3D& camera, std::string_view text, Vector3 position,
                     float emHeight, Color colour)
{
    if (!ready() || text.empty() || emHeight <= 0.0f) return;

    CW_PROFILE_ZONE_N("world text");

    quads_.clear();
    font_.layout(text, quads_);
    if (quads_.empty()) return;

    /* THE BILLBOARD BASIS. Taken from the camera's forward and up rather than
     * from the view matrix's rows, because a caller may hand us a camera that
     * is not the one currently bound — a minimap pass, a shadow debug view —
     * and the matrix is whatever was last set.
     *
     * `up` is re-derived from the cross product rather than used as given:
     * Camera3D::up is a hint, not necessarily perpendicular to the view
     * direction, and using it unmodified skews every quad the moment the camera
     * pitches. */
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3CrossProduct(forward, camera.up);
    if (Vector3LengthSqr(right) < 1e-8f) {
        /* Looking straight along the up axis — the cross product degenerates.
         * Any perpendicular will do, and the text is edge-on to nothing in
         * particular, so pick one rather than emit NaNs. */
        right = Vector3{ 1.0f, 0.0f, 0.0f };
    }
    right = Vector3Normalize(right);
    const Vector3 up = Vector3Normalize(Vector3CrossProduct(right, forward));

    /* Centring. The layout put the run's origin at the start of the baseline,
     * so both offsets are applied here rather than baked into the quads — a
     * caller that later wants left-aligned text changes this and nothing else.
     *
     * Vertically: the glyphs span from the ascender to the descender, and in
     * this Y-DOWN space the ascender is the negative one. The midpoint of that
     * span is what should land on `position`. */
    const float halfWidth = font_.measure(text) * 0.5f;
    const float midline = (font_.ascender() + font_.descender()) * 0.5f;

    BeginShaderMode(shader_);

    const Vector4 outline = ColorNormalize(outlineColour_);
    SetShaderValue(shader_, outlineColourLoc_, &outline, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, outlineWidthLoc_, &outlineWidthPx_, SHADER_UNIFORM_FLOAT);

    rlSetTexture(font_.atlas().id);
    rlCheckRenderBatchLimit(4 * static_cast<int>(quads_.size()));
    rlBegin(RL_QUADS);
    rlColor4ub(colour.r, colour.g, colour.b, colour.a);

    for (const MsdfQuad& quad : quads_) {
        /* Em to world. X shifts left by half the run; Y is negated because the
         * layout is Y-down and `up` points up. */
        const float x0 = (quad.left - halfWidth) * emHeight;
        const float x1 = (quad.right - halfWidth) * emHeight;
        const float y0 = -(quad.top - midline) * emHeight;
        const float y1 = -(quad.bottom - midline) * emHeight;

        /* Four corners, built from the basis. Written out rather than looped:
         * the winding and the texcoord pairing have to match RL_QUADS' expected
         * order, and a loop over a table of signs hides exactly the thing worth
         * being able to read here. */
        const Vector3 topLeft = Vector3Add(position,
            Vector3Add(Vector3Scale(right, x0), Vector3Scale(up, y0)));
        const Vector3 bottomLeft = Vector3Add(position,
            Vector3Add(Vector3Scale(right, x0), Vector3Scale(up, y1)));
        const Vector3 bottomRight = Vector3Add(position,
            Vector3Add(Vector3Scale(right, x1), Vector3Scale(up, y1)));
        const Vector3 topRight = Vector3Add(position,
            Vector3Add(Vector3Scale(right, x1), Vector3Scale(up, y0)));

        rlTexCoord2f(quad.u0, quad.v0); rlVertex3f(topLeft.x, topLeft.y, topLeft.z);
        rlTexCoord2f(quad.u0, quad.v1); rlVertex3f(bottomLeft.x, bottomLeft.y, bottomLeft.z);
        rlTexCoord2f(quad.u1, quad.v1); rlVertex3f(bottomRight.x, bottomRight.y, bottomRight.z);
        rlTexCoord2f(quad.u1, quad.v0); rlVertex3f(topRight.x, topRight.y, topRight.z);
    }

    rlEnd();
    rlSetTexture(0);
    EndShaderMode();
}

}  // namespace cromwell::sdf
