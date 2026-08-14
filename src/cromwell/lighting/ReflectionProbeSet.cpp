#include "cromwell/lighting/ReflectionProbeSet.hpp"

/* GL DIRECTLY, THROUGH THE ONE DOOR. rlgl has no API for cubemap arrays — no
 * create, no per-layer framebuffer attach — so this file calls glTexImage3D
 * and glFramebufferTextureLayer itself.
 *
 * It used to include glad.h. It now goes through render/gpu/GL.hpp, which owns
 * that include and the ordering rule behind it, so there is one boundary
 * rather than a growing list of files that each reached past rlgl for their
 * own good reason. See the note in CMakeLists.txt. */
#include "cromwell/gpu/GL.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace cromwell {
namespace {

/* The six faces, in GL's order and GL's orientations — identical to
 * EnvironmentProbe's table and for the identical reason: a cubemap's faces are
 * defined in a LEFT-handed space, so four of the six up vectors point down. A
 * face rendered with the intuitive up vector comes out mirrored, which is far
 * harder to spot than one that is obviously broken. */
struct CubeFace {
    Vector3 forward;
    Vector3 up;
};

constexpr CubeFace kFaces[6] = {
    { {  1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* +X */
    { { -1.0f,  0.0f,  0.0f }, { 0.0f, -1.0f,  0.0f } },   /* -X */
    { {  0.0f,  1.0f,  0.0f }, { 0.0f,  0.0f,  1.0f } },   /* +Y */
    { {  0.0f, -1.0f,  0.0f }, { 0.0f,  0.0f, -1.0f } },   /* -Y */
    { {  0.0f,  0.0f,  1.0f }, { 0.0f, -1.0f,  0.0f } },   /* +Z */
    { {  0.0f,  0.0f, -1.0f }, { 0.0f, -1.0f,  0.0f } },   /* -Z */
};

}  // namespace

ReflectionProbeSet::~ReflectionProbeSet() { releaseResources(); }

void ReflectionProbeSet::releaseResources()
{
    if (preview_.id)       { UnloadRenderTexture(preview_); preview_ = RenderTexture2D{}; }
    if (previewShader_.id) { UnloadShader(previewShader_);  previewShader_ = Shader{}; }
    if (framebufferId_)    { rlUnloadFramebuffer(framebufferId_); framebufferId_ = 0; }
    if (arrayId_) {
        glDeleteTextures(1, &arrayId_);
        arrayId_ = 0;
    }
    /* The depth renderbuffer was an attachment of the framebuffer just
     * deleted, which frees it. */
    depthBufferId_ = 0;
}

bool ReflectionProbeSet::create()
{
    releaseResources();

    /* GL 4.0 for GL_TEXTURE_CUBE_MAP_ARRAY. The context is asked for 4.3 in
     * CMakeLists; checking rather than assuming, because the failure mode of
     * assuming is a GL_INVALID_ENUM per call and six blank faces. */
    if (!GLAD_GL_VERSION_4_0) {
        TraceLog(LOG_WARNING,
                 "PROBES: no GL 4.0 - no cubemap arrays, surfaces keep the analytic sky");
        return false;
    }

    glGenTextures(1, &arrayId_);
    if (arrayId_ == 0) {
        TraceLog(LOG_WARNING, "PROBES: no array texture - surfaces keep the analytic sky");
        return false;
    }

    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);

    /* RGBA16F, and the depth of a cube array is layers x 6 — every face of
     * every probe is one slice. Null data is legal here in a way it was not
     * for rlLoadTextureCubemap, which is the one nicety of dropping to raw GL.
     *
     * Alpha carries COVERAGE exactly as the single probe's did: faces clear to
     * transparent black and lit geometry writes 1, so the shader can tell
     * "world in this direction" from "open sky" and fall back to the analytic
     * sky gradient for the latter. */
    glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA16F,
                 kFaceSize, kFaceSize, kMaxProbes * 6, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);

    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);

    /* Filters ACROSS face boundaries rather than clamping at them. Without it
     * every probe shows three seams meeting at each corner, which on a smooth
     * surface reads as a crack in the reflected world. It is context state,
     * not texture state, so it is set once and applies to every cubemap the
     * frame samples — including the single-probe path, which wanted it too. */
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    framebufferId_ = rlLoadFramebuffer();
    if (framebufferId_ == 0) {
        TraceLog(LOG_WARNING, "PROBES: no framebuffer - surfaces keep the analytic sky");
        releaseResources();
        return false;
    }

    /* One shared depth renderbuffer for every face of every probe: it is
     * needed WHILE a face renders and never read afterwards, and the faces are
     * drawn one at a time. */
    rlEnableFramebuffer(framebufferId_);
    depthBufferId_ = rlLoadTextureDepth(kFaceSize, kFaceSize, true);
    rlFramebufferAttach(framebufferId_, depthBufferId_, RL_ATTACHMENT_DEPTH,
                        RL_ATTACHMENT_RENDERBUFFER, 0);

    /* Layer 0 attached purely so completeness can be asked once here rather
     * than discovered on the first capture. */
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arrayId_, 0, 0);
    const bool complete = rlFramebufferComplete(framebufferId_);
    rlDisableFramebuffer();

    if (!complete) {
        TraceLog(LOG_WARNING, "PROBES: incomplete framebuffer - keeping the analytic sky");
        releaseResources();
        return false;
    }

    constexpr int kPreviewFace = 128;
    preview_ = LoadRenderTexture(kPreviewFace * 6, kPreviewFace);
    previewShader_ = ShaderLibrary::load(nullptr, "cubemap_preview.fs.glsl");
    if (previewShader_.id != 0) {
        const int unit = 1;
        const int location = GetShaderLocation(previewShader_, "uCubemap");
        if (location >= 0) SetShaderValue(previewShader_, location, &unit, SHADER_UNIFORM_INT);
        locPreviewLayer_ = GetShaderLocation(previewShader_, "uLayer");
    } else {
        TraceLog(LOG_WARNING, "PROBES: no preview shader - the probes cannot be inspected");
    }

    TraceLog(LOG_INFO, "PROBES: %dx%d HDR cubemap array, %d layers",
             kFaceSize, kFaceSize, kMaxProbes);
    return true;
}

void ReflectionProbeSet::bindTo(int textureUnit) const
{
    rlActiveTextureSlot(textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);
    rlActiveTextureSlot(0);
}

void ReflectionProbeSet::unbindFrom(int textureUnit)
{
    rlActiveTextureSlot(textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    rlActiveTextureSlot(0);
}

bool ReflectionProbeSet::addProbe(const ProbeVolume& probe)
{
    if (!valid()) return false;

    /* The shader carries volume arrays of kMaxProbes, and the array texture
     * has exactly that many layers, so a probe past the ceiling has nowhere
     * to live. Dropping it here rather than asserting is deliberate: the
     * caller decides which rooms matter, and it can only make that call if a
     * full set fails softly. */
    if (probeCount() >= kMaxProbes) return false;

    probes_.push_back(probe);

    /* Every existing layer now describes a probe list that has changed shape,
     * and the new one has never been captured at all. */
    cursor_ = 0;
    markAllStale();
    return true;
}

void ReflectionProbeSet::capture(const std::function<void(Vector3 eye)>& drawScene,
                                 int faceCount)
{
    if (!valid() || !drawScene) return;
    if (probes_.empty()) return;
    if (faceCount <= 0) return;

    const int pairCount = probeCount() * 6;
    faceCount = std::min(faceCount, pairCount);

    const Matrix projection = MatrixPerspective(90.0 * DEG2RAD, 1.0, 0.05,
                                                static_cast<double>(captureFar_));

    rlEnableFramebuffer(framebufferId_);
    rlViewport(0, 0, kFaceSize, kFaceSize);
    rlEnableDepthTest();
    rlEnableDepthMask();

    /* CULLING OFF. A cubemap's faces are defined in a LEFT-handed space, so a
     * view matrix built for one flips handedness — and flipping handedness
     * reverses triangle winding, turning every front face into a back face the
     * driver then discards. The framebuffer is complete, the draws are
     * submitted, the log is clean, and nothing lands. Not culling is correct
     * whether the handedness argument holds on every face or not. */
    rlDisableBackfaceCulling();

    for (int step = 0; step < faceCount; step++) {
        const int pair  = cursor_ % pairCount;
        const int probe = pair / 6;
        const int face  = pair % 6;
        cursor_ = (cursor_ + 1) % pairCount;

        /* DRAINED FIRST, or the check below is not a check. glGetError reports
         * the OLDEST error still queued and clears one entry — so an error
         * left behind by any earlier GL call in the frame would be read here
         * and blamed on the attach. Emptying the queue immediately before the
         * call is what makes the next glGetError describe THIS call. */
        if (!attachChecked_)
            while (glGetError() != GL_NO_ERROR) { }

        /* THE LAYER-FACE INDEX, which is what a cube array attachment wants:
         * one flat slice number, not a (layer, face) pair. */
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  arrayId_, 0, probe * 6 + face);

        /* CHECKED, ONCE, AND LOUDLY. A rejected attach does not clear the
         * colour attachment — it leaves whatever was there, which is layer 0
         * from create(). Every probe would then capture onto layer 0, every
         * interior would sample an empty layer and fall back to the sky, and
         * the one populated layer would hold whichever room was captured last.
         * That failure looks exactly like "the reflections are wrong" and
         * gives no hint that the write, not the read, is at fault. */
        if (!attachChecked_) {
            attachChecked_ = true;
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR)
                TraceLog(LOG_WARNING, "PROBES: layer attach rejected (GL 0x%04x) - "
                                      "every probe is writing to layer 0", error);
            else if (!rlFramebufferComplete(framebufferId_))
                TraceLog(LOG_WARNING, "PROBES: layered framebuffer incomplete");
            /* rlFramebufferComplete binds and unbinds, so put the target back
             * before anything is drawn into it. */
            rlEnableFramebuffer(framebufferId_);
        }

        rlViewport(0, 0, kFaceSize, kFaceSize);

        /* PER FACE, not once around the loop. The OPAQUE pass replaces and
         * must not composite — but drawScene draws glass afterwards, and that
         * pass turns blending back on because a window has to composite over
         * what is behind it or its coverage alpha lands in the cubemap as a
         * sky-bright hole. See the glass block in Application::drawGeometryLit.
         * Resetting here is what stops the previous face leaving it on. */
        rlDisableColorBlend();

        /* TRANSPARENT black, not a sky colour — alpha is how the shader tells
         * "world in this direction" from "open sky". */
        rlClearColor(0, 0, 0, 0);
        rlClearScreenBuffers();

        const CubeFace& current = kFaces[face];
        const Vector3 eye = probes_[static_cast<std::size_t>(probe)].capture;

        rlSetMatrixProjection(projection);
        rlSetMatrixModelview(MatrixLookAt(eye, Vector3Add(eye, current.forward), current.up));

        drawScene(eye);

        /* Anything the caller left batched belongs to THIS face. */
        rlDrawRenderBatchActive();

        if (staleFaces_ > 0) staleFaces_--;
    }

    rlEnableBackfaceCulling();
    rlEnableColorBlend();
    rlDisableDepthTest();
    rlDisableFramebuffer();

    rlSetMatrixProjection(MatrixIdentity());
    rlSetMatrixModelview(MatrixIdentity());

    updatePreview();

    /* The caller's own pass owns the viewport from here: restoring it to the
     * window is wrong when the scene target is supersampled, so whoever calls
     * capture() re-establishes it by entering its own target. */
}

void ReflectionProbeSet::setPreviewProbe(int index)
{
    if (probes_.empty()) { previewProbe_ = 0; return; }
    previewProbe_ = std::clamp(index, 0, probeCount() - 1);
}

void ReflectionProbeSet::updatePreview()
{
    if (preview_.id == 0 || previewShader_.id == 0) return;

    BeginTextureMode(preview_);
    BeginShaderMode(previewShader_);

    if (locPreviewLayer_ >= 0) {
        const float layer = static_cast<float>(previewProbe_);
        SetShaderValue(previewShader_, locPreviewLayer_, &layer, SHADER_UNIFORM_FLOAT);
    }

    /* Bound by hand, and NOT with SetShaderValueTexture: that registers a 2D
     * texture with rlgl's batch, which would rebind unit 1 as GL_TEXTURE_2D
     * and leave the sampler reading nothing. */
    rlActiveTextureSlot(1);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, arrayId_);

    DrawRectangle(0, 0, preview_.texture.width, preview_.texture.height, WHITE);
    rlDrawRenderBatchActive();          /* flush while the array is still bound */

    rlActiveTextureSlot(1);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    rlActiveTextureSlot(0);

    EndShaderMode();
    EndTextureMode();
}

}  // namespace cromwell
